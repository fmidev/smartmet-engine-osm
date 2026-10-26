// ======================================================================
/*!
 * \brief Vector tile datasets made of one or more tile sources
 */
// ======================================================================

#include "TileStore.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace
{
// File identity used to detect replaced or modified files
struct FileSignature
{
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::uint64_t inode = 0;

  std::string str(const std::filesystem::path& path) const
  {
    return fmt::format("{}:{}:{}:{}", path.string(), size, mtime_ns, inode);
  }
};

std::optional<FileSignature> signature(const std::filesystem::path& path)
{
  struct stat st
  {
  };
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return {};
  FileSignature sig;
  sig.size = static_cast<std::uint64_t>(st.st_size);
  sig.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
  sig.inode = static_cast<std::uint64_t>(st.st_ino);
  return sig;
}

bool is_pmtiles(const std::filesystem::path& path)
{
  return path.extension() == ".pmtiles";
}

// The *.pmtiles files directly in the given directory, sorted for stable results
std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir)
{
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
  {
    if (is_pmtiles(entry.path()) && entry.is_regular_file(ec))
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

// Latitude of the northern edge of tile row y at zoom z
double tile_lat(uint32_t y, uint8_t z)
{
  const double n = M_PI - 2.0 * M_PI * y / std::ldexp(1.0, z);
  return 180.0 / M_PI * std::atan(std::sinh(n));
}

bool tile_intersects(const FileInfo& file, uint8_t z, uint32_t x, uint32_t y)
{
  const double n = std::ldexp(1.0, z);
  const double west = x / n * 360.0 - 180.0;
  const double east = (x + 1) / n * 360.0 - 180.0;
  const double north = tile_lat(y, z);
  const double south = tile_lat(y + 1, z);
  return west < file.east && east > file.west && south < file.north && north > file.south;
}

FileInfo source_info(const SourceHeader& h)
{
  FileInfo info;
  info.minzoom = h.minZoom;
  info.maxzoom = h.maxZoom;
  info.west = h.west;
  info.south = h.south;
  info.east = h.east;
  info.north = h.north;
  return info;
}

FileInfo file_info(const PMTilesReader& reader, const FileSignature& sig)
{
  FileInfo info = source_info(reader.summary());
  info.path = reader.path();
  info.size = sig.size;
  info.modified = sig.mtime_ns / 1000000000;
  return info;
}

// Split a file name stem into a series name and a version: planet-20261017
// is version 20261017 of the series planet. The version is a suffix of at
// least 8 digits after a '-', '_' or '.'. Names without one have no version.
std::pair<std::string, std::string> split_version(const std::string& stem)
{
  const auto pos = stem.find_last_not_of("0123456789");
  if (pos == std::string::npos || pos == 0 || stem.size() - pos - 1 < 8)
    return {stem, ""};
  const char sep = stem[pos];
  if (sep != '-' && sep != '_' && sep != '.')
    return {stem, ""};
  return {stem.substr(0, pos), stem.substr(pos + 1)};
}

// Order files so that the newest version of each series comes first
void sort_by_version(std::vector<std::filesystem::path>& files)
{
  std::stable_sort(files.begin(),
                   files.end(),
                   [](const auto& a, const auto& b)
                   {
                     const auto va = split_version(a.stem().string());
                     const auto vb = split_version(b.stem().string());
                     if (a.parent_path() != b.parent_path() || va.first != vb.first)
                       return a < b;
                     if (va.second.size() != vb.second.size())
                       return va.second.size() > vb.second.size();
                     return va.second > vb.second;
                   });
}

// Series key of a file: files of the same series replace each other
std::string series(const std::filesystem::path& path)
{
  return (path.parent_path() / split_version(path.stem().string()).first).string();
}

// Ordering of the sources of a dataset: most detailed first, then the
// smallest area first so that a regional source is preferred over a global
// one with the same zoom levels.
bool more_detailed(const FileInfo& fa, const FileInfo& fb)
{
  if (fa.maxzoom != fb.maxzoom)
    return fa.maxzoom > fb.maxzoom;
  const double area_a = (fa.east - fa.west) * (fa.north - fa.south);
  const double area_b = (fb.east - fb.west) * (fb.north - fb.south);
  if (area_a != area_b)
    return area_a < area_b;
  return fa.path < fb.path;
}

}  // namespace

// ----------------------------------------------------------------------

TileStore::TileStore(const Config& theConfig)
    : itsConfig(theConfig),
      itsDatasets(std::make_shared<const Datasets>()),
      itsTileCache(theConfig.tileCacheBytes())
{
  if (theConfig.database())
    itsDatabase = std::make_unique<PgDatabase>(*theConfig.database());
}

TileStore::~TileStore() = default;

// ----------------------------------------------------------------------

std::shared_ptr<const TileStore::Datasets> TileStore::snapshot() const
{
  std::lock_guard<std::mutex> lock(itsMutex);
  return itsDatasets;
}

// ----------------------------------------------------------------------
/*!
 * \brief The published sources in the database
 *
 * Sources of unchanged versions are reused, so their cached tiles stay
 * valid. If the database cannot be reached the previous sources are kept:
 * the tiles of a version do not change once it has been published.
 */
// ----------------------------------------------------------------------

std::map<std::string, std::vector<std::pair<std::shared_ptr<TileSource>, FileInfo>>>
TileStore::scanDatabase()
{
  std::map<std::string, std::vector<std::pair<std::shared_ptr<TileSource>, FileInfo>>> result;
  if (!itsDatabase)
    return result;

  try
  {
    auto records = itsDatabase->published();
    if (itsDatabaseFailed)
      std::cerr << fmt::format("OSM engine: database {} is available again\n", itsDatabase->name());
    itsDatabaseFailed = false;

    decltype(itsDatabaseSources) sources;
    for (const auto& r : records)
    {
      if (r.header.tileType != TileType::MVT)
        continue;
      auto it = itsDatabaseSources.find(r.id);
      if (it != itsDatabaseSources.end())
      {
        sources.insert(*it);
        continue;
      }
      FileInfo info = source_info(r.header);
      info.path = fmt::format("postgresql:{}/{}/{}", r.dataset, r.version, r.source);
      info.modified = r.loaded;
      sources.emplace(r.id, std::make_pair(itsDatabase->open(r), info));
    }
    itsDatabaseSources = std::move(sources);
  }
  catch (const std::exception& e)
  {
    if (!itsDatabaseFailed)
      std::cerr << fmt::format("OSM engine: cannot read the tile database, using the previous "
                               "sources: {}\n",
                               e.what());
    itsDatabaseFailed = true;
  }

  for (const auto& item : itsDatabaseSources)
  {
    const auto& [source, info] = item.second;
    const auto& dataset = static_cast<const PgSource&>(*source).record().dataset;
    result[dataset].emplace_back(source, info);
  }
  return result;
}

// ----------------------------------------------------------------------
/*!
 * \brief Scan the configured directories, files and the database
 *
 * Unchanged files keep their readers (and hence their cached tiles), new
 * and modified files are opened, and removed files are dropped. Files which
 * fail to open are reported once per version of the file. Readers which
 * have failed are always reopened.
 */
// ----------------------------------------------------------------------

void TileStore::rescan()
{
  try
  {
    std::lock_guard<std::mutex> scanlock(itsScanMutex);

    auto old = snapshot();

    // The sources of each dataset
    auto sources = scanDatabase();
    scanFiles(sources);

    auto datasets = std::make_shared<Datasets>();
    for (auto& [name, list] : sources)
      datasets->emplace(name, makeDataset(name, list));

    if (!changed(*old, *datasets))
      return;

    std::lock_guard<std::mutex> lock(itsMutex);
    ++itsVersion;
    for (auto& ds : *datasets)
      ds.second.info.version = itsVersion;
    itsDatasets = datasets;

    for (const auto& ds : *datasets)
    {
      const auto& info = ds.second.info;
      std::cout << fmt::format("OSM engine: dataset '{}' has {} source{}, zoom levels {}-{}\n",
                               info.name,
                               info.files.size(),
                               info.files.size() == 1 ? "" : "s",
                               info.minzoom,
                               info.maxzoom);
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM TileStore::rescan failed");
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief The files of each dataset in the configured directories and files
 */
// ----------------------------------------------------------------------

std::map<std::string, std::vector<std::filesystem::path>> TileStore::candidateFiles() const
{
  std::map<std::string, std::vector<std::filesystem::path>> candidates;

  const auto& root = itsConfig.directory();
  if (!root.empty())
  {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
      const auto& path = entry.path();
      if (entry.is_directory(ec))
      {
        auto files = list_files(path);
        if (!files.empty())
          candidates[path.filename().string()] = std::move(files);
      }
      else if (is_pmtiles(path) && entry.is_regular_file(ec))
        candidates[split_version(path.stem().string()).first].push_back(path);
    }
  }

  // Explicitly configured datasets override directory datasets of the same name
  for (const auto& [name, dc] : itsConfig.datasets())
  {
    std::vector<std::filesystem::path> files = dc.files;
    if (!dc.directory.empty())
    {
      auto dirfiles = list_files(dc.directory);
      files.insert(files.end(), dirfiles.begin(), dirfiles.end());
    }
    candidates[name] = std::move(files);
  }

  return candidates;
}

// ----------------------------------------------------------------------
/*!
 * \brief Add the newest usable file of each series to the sources of its dataset
 *
 * Files which may still be being written are skipped.
 */
// ----------------------------------------------------------------------

void TileStore::scanFiles(Sources& theSources)
{
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const auto min_age_ns = std::int64_t{itsConfig.minFileAge()} * 1000000000;

  std::map<std::string, OpenFile> openfiles;  // the files in use after this scan

  for (auto& [name, files] : candidateFiles())
  {
    sort_by_version(files);
    std::set<std::string> done;  // series with a file in use

    for (const auto& path : files)
    {
      if (done.count(series(path)) > 0)
        continue;  // an older version

      auto sig = signature(path);
      if (!sig || now_ns - sig->mtime_ns < min_age_ns)
        continue;

      const auto sigstr = sig->str(path);
      auto reader = openReader(path, sigstr);
      if (!reader)
        continue;

      done.insert(series(path));
      openfiles[path.string()] = OpenFile{sigstr, reader};
      theSources[name].emplace_back(reader, file_info(*reader, *sig));
    }
  }

  itsOpenFiles = std::move(openfiles);
}

// ----------------------------------------------------------------------
/*!
 * \brief The reader of a file, null if the file cannot be used
 *
 * The reader is reused if the file is unchanged and the reader works.
 */
// ----------------------------------------------------------------------

std::shared_ptr<PMTilesReader> TileStore::openReader(const std::filesystem::path& thePath,
                                                     const std::string& theSignature)
{
  auto it = itsOpenFiles.find(thePath.string());
  if (it != itsOpenFiles.end() && it->second.signature == theSignature &&
      !it->second.reader->failed())
    return it->second.reader;

  if (itsFailedFiles.count(theSignature) > 0)
    return nullptr;

  try
  {
    auto reader =
        std::make_shared<PMTilesReader>(thePath, itsConfig.leafCacheSize(), itsConfig.fileAccess());
    if (reader->header().tileType == TileType::MVT)
      return reader;
    std::cerr << fmt::format("OSM engine: {} does not contain vector tiles, ignored\n",
                             thePath.string());
  }
  catch (const std::exception& e)
  {
    std::cerr << fmt::format("OSM engine: failed to open {}: {}\n", thePath.string(), e.what());
  }
  itsFailedFiles.insert(theSignature);
  return nullptr;
}

// ----------------------------------------------------------------------
/*!
 * \brief A dataset of the sources, the most detailed sources first
 */
// ----------------------------------------------------------------------

TileStore::Dataset TileStore::makeDataset(const std::string& theName, SourceList& theSources)
{
  std::stable_sort(theSources.begin(),
                   theSources.end(),
                   [](const auto& a, const auto& b) { return more_detailed(a.second, b.second); });

  Dataset ds;
  ds.info.name = theName;
  for (const auto& [source, info] : theSources)
  {
    ds.sources.push_back(source);
    ds.info.files.push_back(info);
  }

  auto& info = ds.info;
  info.minzoom = 255;
  info.west = 180;
  info.south = 90;
  info.east = -180;
  info.north = -90;
  for (const auto& f : info.files)
  {
    info.minzoom = std::min(info.minzoom, f.minzoom);
    info.maxzoom = std::max(info.maxzoom, f.maxzoom);
    info.west = std::min(info.west, f.west);
    info.south = std::min(info.south, f.south);
    info.east = std::max(info.east, f.east);
    info.north = std::max(info.north, f.north);
    info.modified = std::max(info.modified, f.modified);
  }
  try
  {
    info.metadata = ds.sources.front()->metadata();
  }
  catch (...)
  {
    // Metadata is informational only
  }
  return ds;
}

// ----------------------------------------------------------------------
/*!
 * \brief True if any dataset has a different list of sources
 */
// ----------------------------------------------------------------------

bool TileStore::changed(const Datasets& theOld, const Datasets& theNew)
{
  if (theOld.size() != theNew.size())
    return true;
  return std::any_of(theNew.begin(),
                     theNew.end(),
                     [&](const auto& item)
                     {
                       auto it = theOld.find(item.first);
                       return it == theOld.end() || it->second.sources != item.second.sources;
                     });
}

// ----------------------------------------------------------------------

std::vector<std::string> TileStore::datasets() const
{
  std::vector<std::string> names;
  for (const auto& ds : *snapshot())
    names.push_back(ds.first);
  return names;
}

// ----------------------------------------------------------------------

std::optional<DatasetInfo> TileStore::info(const std::string& theDataset) const
{
  auto datasets = snapshot();
  auto it = datasets->find(theDataset);
  if (it == datasets->end())
    return {};
  return it->second.info;
}

// ----------------------------------------------------------------------
/*!
 * \brief Find the most detailed tile at or above the requested tile
 *
 * If a source fails because its file was replaced, rescan and retry once.
 */
// ----------------------------------------------------------------------

std::optional<Tile> TileStore::getTile(const std::string& theDataset,
                                       uint8_t z,
                                       uint32_t x,
                                       uint32_t y)
{
  try
  {
    try
    {
      return findTile(theDataset, z, x, y);
    }
    catch (...)
    {
      auto datasets = snapshot();
      auto it = datasets->find(theDataset);
      if (it == datasets->end())
        throw;
      const auto& sources = it->second.sources;
      if (std::none_of(sources.begin(), sources.end(), [](const auto& s) { return s->failed(); }))
        throw;
    }

    std::cerr << fmt::format("OSM engine: reading dataset '{}' failed, rescanning\n", theDataset);
    rescan();
    return findTile(theDataset, z, x, y);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM TileStore::getTile failed")
        .addParameter("dataset", theDataset)
        .addParameter("z", std::to_string(z))
        .addParameter("x", std::to_string(x))
        .addParameter("y", std::to_string(y));
  }
}

// ----------------------------------------------------------------------

std::optional<Tile> TileStore::findTile(const std::string& theDataset,
                                        uint8_t z,
                                        uint32_t x,
                                        uint32_t y)
{
  if (z > 30 || x >= (1U << z) || y >= (1U << z))
    return {};

  auto datasets = snapshot();
  auto it = datasets->find(theDataset);
  if (it == datasets->end())
    return {};
  const auto& ds = it->second;

  for (int zz = std::min<int>(z, ds.info.maxzoom); zz >= 0; --zz)
  {
    const auto shift = z - zz;
    const uint32_t xx = x >> shift;
    const uint32_t yy = y >> shift;
    const auto tileid = TileSource::tileToId(static_cast<uint8_t>(zz), xx, yy);

    for (std::size_t i = 0; i < ds.sources.size(); i++)
    {
      const auto& file = ds.info.files[i];
      if (zz < file.minzoom || zz > file.maxzoom)
        continue;
      if (!tile_intersects(file, static_cast<uint8_t>(zz), xx, yy))
        continue;

      const auto& source = ds.sources[i];
      const TileKey key{source->id(), tileid};

      Tile tile;
      tile.z = static_cast<uint8_t>(zz);
      tile.x = xx;
      tile.y = yy;
      tile.source = source->id();

      if (auto cached = itsTileCache.find(key))
      {
        if (!*cached)
          continue;  // known to be missing from this source
        tile.data = *cached;
        return tile;
      }

      auto data = source->getDecompressedTile(tile.z, xx, yy);
      if (!data)
      {
        itsTileCache.insert(key, nullptr);
        continue;
      }

      tile.data = std::make_shared<const std::string>(std::move(*data));
      itsTileCache.insert(key, tile.data);
      return tile;
    }
  }
  return {};
}

// ----------------------------------------------------------------------
/*!
 * \brief Fetch the tiles from batched sources with one request per source
 *
 * Only the first candidate source of each tile is fetched; getTile falls
 * back to the other sources as usual. Errors are left for getTile to handle.
 */
// ----------------------------------------------------------------------

void TileStore::prefetch(const std::string& theDataset, const std::vector<TileCoord>& theTiles)
{
  try
  {
    auto datasets = snapshot();
    auto it = datasets->find(theDataset);
    if (it == datasets->end())
      return;
    const auto& ds = it->second;

    std::map<std::size_t, std::vector<TileCoord>> todo;  // by source index
    for (const auto& t : theTiles)
    {
      for (std::size_t i = 0; i < ds.sources.size(); i++)
      {
        const auto& file = ds.info.files[i];
        if (t.z < file.minzoom || t.z > file.maxzoom || !tile_intersects(file, t.z, t.x, t.y))
          continue;
        if (ds.sources[i]->batched() &&
            !itsTileCache.find(TileKey{ds.sources[i]->id(), TileSource::tileToId(t.z, t.x, t.y)}))
          todo[i].push_back(t);
        break;
      }
    }

    for (const auto& [i, tiles] : todo)
    {
      const auto& source = ds.sources[i];
      auto data = source->getDecompressedTiles(tiles);
      for (std::size_t j = 0; j < tiles.size(); j++)
      {
        const auto& t = tiles[j];
        const TileKey key{source->id(), TileSource::tileToId(t.z, t.x, t.y)};
        if (data[j])
          itsTileCache.insert(key, std::make_shared<const std::string>(std::move(*data[j])));
        else
          itsTileCache.insert(key, nullptr);
      }
    }
  }
  catch (...)
  {
    // getTile reports the errors
  }
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
