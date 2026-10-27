// ======================================================================
/*!
 * \brief smartmet-osm-pgload - load PMTiles files into PostgreSQL
 *
 * Loads PMTiles files as a new version of a dataset, publishes versions
 * and removes old ones. See PgSource.h for the schema. Each file is loaded
 * in a single transaction, and publishing is a single transaction, so the
 * servers never see a partial version.
 *
 *   smartmet-osm-pgload --db "host=db dbname=osm user=osm" \
 *       --dataset protomaps --version 20261017 --publish planet.pmtiles
 *   smartmet-osm-pgload --db ... --list
 *   smartmet-osm-pgload --db ... --dataset protomaps --prune 1
 */
// ======================================================================

#include "../osm/PMTilesReader.h"
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <ctime>
#include <iostream>
#include <pqxx/pqxx>
#include <set>
#include <unordered_set>

namespace po = boost::program_options;
using SmartMet::Engine::OSM::Entry;
using SmartMet::Engine::OSM::PMTilesReader;

namespace
{
struct Options
{
  std::string db;
  std::string schema = "osm";
  std::string dataset;
  std::string version;
  std::vector<std::string> files;
  bool publish = false;
  bool replace = false;
  bool list = false;
  bool drop = false;
  int prune = -1;
};

bool valid_identifier(const std::string& theName)
{
  return !theName.empty() &&
         theName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}

// planet-20261017 → planet, see TileStore.cpp
std::string source_name(const std::string& stem)
{
  const auto pos = stem.find_last_not_of("0123456789");
  if (pos == std::string::npos || pos == 0 || stem.size() - pos - 1 < 8)
    return stem;
  const char sep = stem[pos];
  if (sep != '-' && sep != '_' && sep != '.')
    return stem;
  return stem.substr(0, pos);
}

// Statement with parameters: exec(query, params) appeared in libpqxx 7.10,
// which RHEL 8 and RHEL 9 do not have. exec_params is deprecated in newer versions.
template <typename... Args>
pqxx::result exec(pqxx::transaction_base& tx, const std::string& sql, Args&&... args)
{
#if PQXX_VERSION_MAJOR > 7 || (PQXX_VERSION_MAJOR == 7 && PQXX_VERSION_MINOR >= 10)
  return tx.exec(sql, pqxx::params{std::forward<Args>(args)...});
#else
  return tx.exec_params(sql, std::forward<Args>(args)...);
#endif
}

std::string today()
{
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  return fmt::format("{:04}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

std::string human(double bytes)
{
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  while (bytes >= 1024 && i < 4)
  {
    bytes /= 1024;
    ++i;
  }
  return fmt::format("{:.1f} {}", bytes, units[i]);
}

// ----------------------------------------------------------------------

void create_schema(pqxx::connection& conn, const std::string& s)
{
  pqxx::work tx(conn);
  tx.exec(fmt::format("CREATE SCHEMA IF NOT EXISTS {}", s));
  tx.exec(
      fmt::format("CREATE TABLE IF NOT EXISTS {}.sources ("
                  "id bigserial PRIMARY KEY, "
                  "dataset text NOT NULL, version text NOT NULL, source text NOT NULL, "
                  "tile_type smallint NOT NULL, tile_compression smallint NOT NULL, "
                  "minzoom smallint NOT NULL, maxzoom smallint NOT NULL, "
                  "west double precision NOT NULL, south double precision NOT NULL, "
                  "east double precision NOT NULL, north double precision NOT NULL, "
                  "metadata text, tiles bigint, bytes bigint, "
                  "loaded timestamptz NOT NULL DEFAULT now(), "
                  "UNIQUE (dataset, version, source))",
                  s));
  tx.exec(
      fmt::format("CREATE TABLE IF NOT EXISTS {}.datasets ("
                  "dataset text PRIMARY KEY, version text NOT NULL, "
                  "published timestamptz NOT NULL DEFAULT now())",
                  s));
  tx.commit();
}

std::optional<std::string> published_version(pqxx::transaction_base& tx,
                                             const std::string& s,
                                             const std::string& dataset)
{
  auto r = exec(tx, fmt::format("SELECT version FROM {}.datasets WHERE dataset = $1", s), dataset);
  if (r.empty())
    return {};
  return r[0][0].as<std::string>();
}

void drop_sources(pqxx::transaction_base& tx, const std::string& s, const pqxx::result& ids)
{
  for (const auto& row : ids)
  {
    const auto id = row[0].as<std::int64_t>();
    tx.exec(fmt::format("DROP TABLE IF EXISTS {0}.tiles_{1}, {0}.tiledata_{1}", s, id));
    exec(tx, fmt::format("DELETE FROM {}.sources WHERE id = $1", s), id);
  }
}

// ----------------------------------------------------------------------

void load(pqxx::connection& conn, const Options& opt, const std::string& file)
{
  const auto& s = opt.schema;
  PMTilesReader reader(file, 1024, SmartMet::Engine::OSM::FileAccess::Pread);
  const auto& h = reader.header();
  const auto source = source_name(std::filesystem::path(file).stem().string());

  std::cout << fmt::format("Loading {} as {}/{}/{}\n", file, opt.dataset, opt.version, source);

  pqxx::work tx(conn);

  if (published_version(tx, s, opt.dataset) == opt.version)
    throw std::runtime_error(
        fmt::format("{}/{} is published and cannot be modified", opt.dataset, opt.version));

  auto old = exec(tx,
                  fmt::format("SELECT id FROM {}.sources WHERE dataset = $1 "
                              "AND version = $2 AND source = $3",
                              s),
                  opt.dataset,
                  opt.version,
                  source);
  if (!old.empty())
  {
    if (!opt.replace)
      throw std::runtime_error(fmt::format(
          "{}/{}/{} is already loaded, use --replace", opt.dataset, opt.version, source));
    drop_sources(tx, s, old);
  }

  const auto id = exec(tx,
                       fmt::format("INSERT INTO {}.sources (dataset, version, source, tile_type, "
                                   "tile_compression, minzoom, maxzoom, west, south, east, north, "
                                   "metadata) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12) "
                                   "RETURNING id",
                                   s),
                       opt.dataset,
                       opt.version,
                       source,
                       static_cast<int>(h.tileType),
                       static_cast<int>(h.tileCompression),
                       static_cast<int>(h.minZoom),
                       static_cast<int>(h.maxZoom),
                       static_cast<double>(h.minLon),
                       static_cast<double>(h.minLat),
                       static_cast<double>(h.maxLon),
                       static_cast<double>(h.maxLat),
                       reader.metadata())[0][0]
                      .as<std::int64_t>();

  const auto tiles = fmt::format("{}.tiles_{}", s, id);
  const auto tiledata = fmt::format("{}.tiledata_{}", s, id);
  tx.exec(
      fmt::format("CREATE TABLE {} (tile_id bigint NOT NULL, run_length "
                  "integer NOT NULL, data_id bigint "
                  "NOT NULL)",
                  tiles));
  tx.exec(fmt::format("CREATE TABLE {} (data_id bigint NOT NULL, data bytea NOT NULL)", tiledata));
  // The tiles are already compressed
  tx.exec(fmt::format("ALTER TABLE {} ALTER COLUMN data SET STORAGE EXTERNAL", tiledata));

  // The directory entries. The data id is the offset of the tile data, which
  // is shared by identical tiles.
  std::int64_t count = 0;
  {
    auto stream = pqxx::stream_to::raw_table(tx, tiles, "tile_id, run_length, data_id");
    reader.forEachEntry(
        [&](const Entry& e)
        {
          stream.write_values(static_cast<std::int64_t>(e.tileId),
                              static_cast<std::int64_t>(e.runLength),
                              static_cast<std::int64_t>(e.offset));
          if (++count % 10000000 == 0)
            std::cout << fmt::format("  {} directory entries\n", count) << std::flush;
        });
    stream.complete();
  }

  // The tile contents, each only once. In clustered files new contents
  // appear in increasing offset order.
  std::int64_t contents = 0;
  std::int64_t bytes = 0;
  {
    auto stream = pqxx::stream_to::raw_table(tx, tiledata, "data_id, data");
    std::uint64_t next = 0;
    std::unordered_set<std::uint64_t> seen;
    reader.forEachEntry(
        [&](const Entry& e)
        {
          if (h.clustered)
          {
            if (e.offset < next)
              return;
            next = e.offset + e.length;
          }
          else if (!seen.insert(e.offset).second)
            return;

          const auto data = reader.readTileData(e);
          stream.write_values(static_cast<std::int64_t>(e.offset),
                              std::basic_string<std::byte>(
                                  reinterpret_cast<const std::byte*>(data.data()), data.size()));
          bytes += static_cast<std::int64_t>(data.size());
          if (++contents % 1000000 == 0)
            std::cout << fmt::format("  {} tiles, {}\n", contents, human(bytes)) << std::flush;
        });
    stream.complete();
  }

  std::cout << "  indexing\n" << std::flush;
  tx.exec(fmt::format("ALTER TABLE {} ADD PRIMARY KEY (tile_id)", tiles));
  tx.exec(fmt::format("ALTER TABLE {} ADD PRIMARY KEY (data_id)", tiledata));
  exec(tx,
       fmt::format("UPDATE {}.sources SET tiles = $1, bytes = $2 WHERE id = $3", s),
       contents,
       bytes,
       id);
  tx.commit();

  pqxx::nontransaction ntx(conn);
  ntx.exec(fmt::format("ANALYZE {}", tiles));
  ntx.exec(fmt::format("ANALYZE {}", tiledata));

  std::cout << fmt::format(
      "  {} directory entries, {} distinct tiles, {}\n", count, contents, human(bytes));
}

// ----------------------------------------------------------------------

void publish(pqxx::connection& conn, const Options& opt)
{
  const auto& s = opt.schema;
  pqxx::work tx(conn);
  auto n = exec(tx,
                fmt::format("SELECT count(*) FROM {}.sources WHERE dataset = "
                            "$1 AND version = $2",
                            s),
                opt.dataset,
                opt.version)[0][0]
               .as<int>();
  if (n == 0)
    throw std::runtime_error(
        fmt::format("{}/{} has no loaded sources, nothing to publish", opt.dataset, opt.version));

  exec(tx,
       fmt::format("INSERT INTO {}.datasets (dataset, version) VALUES ($1, $2) "
                   "ON CONFLICT (dataset) DO UPDATE SET version = EXCLUDED.version, "
                   "published = now()",
                   s),
       opt.dataset,
       opt.version);
  tx.commit();
  std::cout << fmt::format("Published {}/{}\n", opt.dataset, opt.version);
}

// ----------------------------------------------------------------------

void drop(pqxx::connection& conn, const Options& opt, const std::string& version)
{
  const auto& s = opt.schema;
  pqxx::work tx(conn);
  if (published_version(tx, s, opt.dataset) == version)
    throw std::runtime_error(
        fmt::format("{}/{} is published and cannot be dropped", opt.dataset, version));
  auto ids = exec(tx,
                  fmt::format("SELECT id FROM {}.sources WHERE dataset = $1 AND version = $2", s),
                  opt.dataset,
                  version);
  if (ids.empty())
    throw std::runtime_error(fmt::format("{}/{} is not loaded", opt.dataset, version));
  drop_sources(tx, s, ids);
  tx.commit();
  std::cout << fmt::format("Dropped {}/{}\n", opt.dataset, version);
}

// Drop all but the published version and the given number of newest other
// versions
void prune(pqxx::connection& conn, const Options& opt)
{
  std::vector<std::string> versions;
  {
    pqxx::read_transaction tx(conn);
    const auto current = published_version(tx, opt.schema, opt.dataset);
    auto rows = exec(tx,
                     fmt::format("SELECT version FROM {}.sources WHERE dataset = $1 "
                                 "GROUP BY version ORDER BY max(loaded) DESC",
                                 opt.schema),
                     opt.dataset);
    int kept = 0;
    for (const auto& row : rows)
    {
      auto version = row[0].as<std::string>();
      if (version == current)
        continue;
      if (kept++ < opt.prune)
        continue;
      versions.push_back(version);
    }
  }
  for (const auto& version : versions)
    drop(conn, opt, version);
}

// ----------------------------------------------------------------------

void list(pqxx::connection& conn, const Options& opt)
{
  pqxx::read_transaction tx(conn);
  auto rows =
      tx.exec(fmt::format("SELECT s.dataset, s.version, s.source, s.minzoom, "
                          "s.maxzoom, s.west, s.south, s.east, "
                          "s.north, coalesce(s.tiles, 0), coalesce(s.bytes, 0), "
                          "to_char(s.loaded, 'YYYY-MM-DD "
                          "HH24:MI'), d.version IS NOT NULL "
                          "FROM {0}.sources s LEFT JOIN {0}.datasets d ON d.dataset = "
                          "s.dataset AND d.version = "
                          "s.version ORDER BY s.dataset, s.loaded DESC, s.source",
                          opt.schema));

  std::cout << fmt::format("{:<16} {:<12} {:<16} {:>5}  {:<28} {:>12} {:>10}  {:<16} {}\n",
                           "dataset",
                           "version",
                           "source",
                           "zoom",
                           "bounds",
                           "tiles",
                           "size",
                           "loaded",
                           "");
  for (const auto& r : rows)
  {
    std::cout << fmt::format("{:<16} {:<12} {:<16} {:>2}-{:<2}  {:<28} {:>12} {:>10}  {:<16} {}\n",
                             r[0].as<std::string>(),
                             r[1].as<std::string>(),
                             r[2].as<std::string>(),
                             r[3].as<int>(),
                             r[4].as<int>(),
                             fmt::format("{:.1f},{:.1f},{:.1f},{:.1f}",
                                         r[5].as<double>(),
                                         r[6].as<double>(),
                                         r[7].as<double>(),
                                         r[8].as<double>()),
                             r[9].as<std::int64_t>(),
                             human(r[10].as<double>()),
                             r[11].as<std::string>(),
                             r[12].as<bool>() ? "published" : "");
  }
}

}  // namespace

// ----------------------------------------------------------------------

int main(int argc, char** argv)
try
{
  Options opt;

  po::options_description desc(
      "Usage: smartmet-osm-pgload [options] [file.pmtiles ...]\n\n"
      "Loads PMTiles files into PostgreSQL as a version of a dataset for the\n"
      "SmartMet OSM engine. Each file becomes a source named after the file,\n"
      "without a version suffix such as -20261017.\n\n"
      "Options");
  // clang-format off
  desc.add_options()
      ("help,h", "print this help")
      ("db", po::value(&opt.db), "libpq connection string, for example \"host=db dbname=osm "
                                 "user=osm\" (default: the PG* environment variables)")
      ("schema", po::value(&opt.schema)->default_value(opt.schema), "database schema")
      ("dataset,d", po::value(&opt.dataset), "dataset name")
      ("version,v", po::value(&opt.version), "version name (default: today as YYYYMMDD when loading)")
      ("publish,p", po::bool_switch(&opt.publish), "publish the version (after loading the files)")
      ("replace", po::bool_switch(&opt.replace), "replace sources already loaded into an unpublished version")
      ("list,l", po::bool_switch(&opt.list), "list the loaded versions")
      ("drop", po::bool_switch(&opt.drop), "drop an unpublished version")
      ("prune", po::value(&opt.prune), "drop all versions but the published one and this many newest others")
      ("file", po::value(&opt.files), "PMTiles file");
  // clang-format on

  po::positional_options_description pos;
  pos.add("file", -1);

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), vm);
  po::notify(vm);

  if (vm.count("help") > 0 || argc == 1)
  {
    std::cout << desc << "\n";
    return 0;
  }

  if (!valid_identifier(opt.schema))
    throw std::runtime_error("The schema name may contain only a-z, 0-9 and _");

  const bool need_dataset = !opt.files.empty() || opt.publish || opt.drop || opt.prune >= 0;
  if (need_dataset && opt.dataset.empty())
    throw std::runtime_error("--dataset is required");
  if (!opt.files.empty() && opt.version.empty())
    opt.version = today();
  if ((opt.publish || opt.drop) && opt.version.empty())
    throw std::runtime_error("--version is required");
  if (opt.drop && !opt.files.empty())
    throw std::runtime_error("--drop cannot be combined with loading files");

  pqxx::connection conn(opt.db);
  create_schema(conn, opt.schema);

  for (const auto& file : opt.files)
    load(conn, opt, file);
  if (opt.publish)
    publish(conn, opt);
  if (opt.drop)
    drop(conn, opt, opt.version);
  if (opt.prune >= 0)
    prune(conn, opt);
  if (opt.list)
    list(conn, opt);

  return 0;
}
catch (const std::exception& e)
{
  std::cerr << "smartmet-osm-pgload: " << e.what() << '\n';
  return 1;
}
