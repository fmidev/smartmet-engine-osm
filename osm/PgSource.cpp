// ======================================================================
/*!
 * \brief Vector tiles stored in PostgreSQL
 */
// ======================================================================

#include "PgSource.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <macgyver/PostgreSQLConnection.h>
#include <map>
#include <utility>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
// ----------------------------------------------------------------------
/*!
 * \brief Connections opened on demand and kept for reuse
 *
 * A connection which fails a query is discarded instead of being returned
 * to the pool, so a database restart costs only the queries in progress.
 */
// ----------------------------------------------------------------------

class PgConnectionPool
{
 public:
  using Connection = Fmi::Database::PostgreSQLConnection;

  explicit PgConnectionPool(const PgConfig& theConfig) : itsConfig(theConfig)
  {
    itsOptions.host = theConfig.host;
    itsOptions.port = theConfig.port;
    itsOptions.database = theConfig.database;
    itsOptions.username = theConfig.username;
    itsOptions.password = theConfig.password;
    itsOptions.connect_timeout = theConfig.connectTimeout;
  }

  std::string name() const
  {
    return fmt::format("{} schema {}", itsOptions.getId(), itsConfig.schema);
  }

  template <typename F>
  auto with(F theFunction)
  {
    auto conn = acquire();
    auto result = theFunction(*conn);  // a failure drops the connection
    release(std::move(conn));
    return result;
  }

 private:
  std::unique_ptr<Connection> acquire()
  {
    {
      std::lock_guard<std::mutex> lock(itsMutex);
      if (!itsIdle.empty())
      {
        auto conn = std::move(itsIdle.back());
        itsIdle.pop_back();
        return conn;
      }
    }
    return std::make_unique<Connection>(itsOptions);
  }

  void release(std::unique_ptr<Connection> theConnection)
  {
    std::lock_guard<std::mutex> lock(itsMutex);
    if (itsIdle.size() < itsConfig.maxIdleConnections)
      itsIdle.push_back(std::move(theConnection));
  }

  const PgConfig& itsConfig;
  Fmi::Database::PostgreSQLConnectionOptions itsOptions;
  std::mutex itsMutex;
  std::vector<std::unique_ptr<Connection>> itsIdle;
};

// ----------------------------------------------------------------------

PgSource::PgSource(std::shared_ptr<PgConnectionPool> thePool,
                   std::string theSchema,
                   PgSourceRecord theRecord)
    : itsPool(std::move(thePool)), itsRecord(std::move(theRecord))
{
  setSummary(itsRecord.header);

  // The run containing each requested tile id. The tile ids and runs are
  // those of the PMTiles directories.
  itsQuery = fmt::format(
      "SELECT q.id, d.data FROM unnest($1::bigint[]) AS q(id) "
      "CROSS JOIN LATERAL (SELECT t.tile_id, t.run_length, "
      "t.data_id FROM {0}.tiles_{1} t "
      "WHERE t.tile_id <= q.id ORDER BY t.tile_id DESC LIMIT 1) r "
      "JOIN {0}.tiledata_{1} d ON d.data_id = r.data_id "
      "WHERE q.id < r.tile_id + r.run_length",
      theSchema,
      itsRecord.id);
}

// ----------------------------------------------------------------------

std::optional<std::string> PgSource::getDecompressedTile(uint8_t z, uint32_t x, uint32_t y) const
{
  return getDecompressedTiles({TileCoord{z, x, y}}).front();
}

// ----------------------------------------------------------------------

std::vector<std::optional<std::string>> PgSource::getDecompressedTiles(
    const std::vector<TileCoord>& theTiles) const
{
  try
  {
    std::vector<std::optional<std::string>> result(theTiles.size());

    // Tile ids of the requests within the zoom levels of the source
    std::multimap<std::int64_t, std::size_t> wanted;
    std::string ids = "{";
    for (std::size_t i = 0; i < theTiles.size(); i++)
    {
      const auto& t = theTiles[i];
      if (t.z < summary().minZoom || t.z > summary().maxZoom)
        continue;
      const auto id = static_cast<std::int64_t>(tileToId(t.z, t.x, t.y));
      if (wanted.count(id) == 0)
      {
        if (ids.size() > 1)
          ids += ',';
        ids += fmt::format("{}", id);
      }
      wanted.emplace(id, i);
    }
    ids += '}';

    if (wanted.empty())
      return result;

    auto rows = itsPool->with([&](const Fmi::Database::PostgreSQLConnection& conn)
                              { return conn.exec_params(itsQuery, ids); });

    for (const auto& row : rows)
    {
      const auto id = row[0].as<std::int64_t>();
      const auto data = row[1].as<std::basic_string<std::byte>>();
      auto tile = decompressToString(data.data(), data.size(), summary().tileCompression);
      auto range = wanted.equal_range(id);
      for (auto it = range.first; it != range.second; ++it)
        result[it->second] = tile;
    }
    return result;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to read tiles from PostgreSQL")
        .addParameter("dataset", itsRecord.dataset)
        .addParameter("version", itsRecord.version)
        .addParameter("source", itsRecord.source);
  }
}

// ----------------------------------------------------------------------

PgDatabase::PgDatabase(const PgConfig& theConfig)
    : itsConfig(theConfig), itsPool(std::make_shared<PgConnectionPool>(itsConfig))
{
}

PgDatabase::~PgDatabase() = default;

// ----------------------------------------------------------------------

std::string PgDatabase::name() const
{
  return itsPool->name();
}

// ----------------------------------------------------------------------

std::vector<PgSourceRecord> PgDatabase::published() const
{
  try
  {
    const auto sql = fmt::format(
        "SELECT s.id, s.dataset, s.version, s.source, s.tile_type, "
        "s.tile_compression, "
        "s.minzoom, s.maxzoom, s.west, s.south, s.east, s.north, s.metadata, "
        "extract(epoch FROM s.loaded)::bigint "
        "FROM {0}.datasets d JOIN {0}.sources s ON s.dataset = d.dataset AND "
        "s.version = "
        "d.version ORDER BY s.dataset, s.id",
        itsConfig.schema);

    auto rows = itsPool->with([&](const Fmi::Database::PostgreSQLConnection& conn)
                              { return conn.executeNonTransaction(sql); });

    std::vector<PgSourceRecord> records;
    for (const auto& row : rows)
    {
      PgSourceRecord r;
      r.id = row[0].as<std::int64_t>();
      r.dataset = row[1].as<std::string>();
      r.version = row[2].as<std::string>();
      r.source = row[3].as<std::string>();
      r.header.tileType = static_cast<TileType>(row[4].as<int>());
      r.header.tileCompression = static_cast<Compression>(row[5].as<int>());
      r.header.minZoom = static_cast<uint8_t>(row[6].as<int>());
      r.header.maxZoom = static_cast<uint8_t>(row[7].as<int>());
      r.header.west = row[8].as<double>();
      r.header.south = row[9].as<double>();
      r.header.east = row[10].as<double>();
      r.header.north = row[11].as<double>();
      r.metadata = row[12].is_null() ? std::string() : row[12].as<std::string>();
      r.loaded = row[13].as<std::int64_t>();
      records.push_back(std::move(r));
    }
    return records;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to list the published tile sources")
        .addParameter("database", name());
  }
}

// ----------------------------------------------------------------------

std::shared_ptr<PgSource> PgDatabase::open(const PgSourceRecord& theRecord) const
{
  return std::make_shared<PgSource>(itsPool, itsConfig.schema, theRecord);
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
