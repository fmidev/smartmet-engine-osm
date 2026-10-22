// ======================================================================
/*!
 * \brief Vector tile datasets made of one or more tile sources
 *
 * A dataset is a set of sources sharing the same vector tile schema, for
 * example a planet file limited to zoom level 8 and a regional file with
 * all zoom levels. A source is a PMTiles file or a table loaded from one into
 * PostgreSQL. A tile request is answered from the most detailed source
 * containing the tile. If no source has the requested zoom level for the
 * location, the nearest ancestor tile is returned instead and the caller
 * is expected to overzoom it. Services can thus be localized just by
 * placing a more detailed file for the local region into the directory.
 *
 * The directories and the database are rescanned periodically. Changed files
 * are reopened and readers of replaced files stay valid until their last user
 * is done. Files must be replaced by moving the new file into place (rename),
 * never by writing over them. Of files whose names differ only by a version
 * suffix (planet-20261017.pmtiles, planet-20261117.pmtiles) only the newest
 * version is used, so new versions can be installed without touching the
 * old ones, which can then be deleted once the servers have switched.
 *
 * Files on network file systems are read with pread (see PMTilesReader).
 * If a read fails because the file was replaced, the store rescans and
 * retries the request once.
 */
// ======================================================================

#pragma once

#include "Config.h"
#include "PMTilesReader.h"
#include "PgSource.h"
#include <macgyver/Cache.h>
#include <macgyver/CacheStats.h>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
// Information on a single source of a dataset
struct FileInfo
{
  std::filesystem::path path;  // the file, or postgresql:dataset/version/source
  std::uint64_t size = 0;
  std::int64_t modified = 0;  // seconds since epoch
  uint8_t minzoom = 0;
  uint8_t maxzoom = 0;
  double west = -180;
  double south = -85.0511;
  double east = 180;
  double north = 85.0511;
};

// Summary of a dataset
struct DatasetInfo
{
  std::string name;
  uint8_t minzoom = 0;
  uint8_t maxzoom = 0;  // maximum zoom available somewhere
  double west = 0;
  double south = 0;
  double east = 0;
  double north = 0;
  std::int64_t modified = 0;   // latest modification or load time of the sources
  std::uint64_t version = 0;   // changes whenever the set of sources changes
  std::string metadata;        // JSON metadata of the most detailed source
  std::vector<FileInfo> files; // most detailed first
};

// A decompressed tile
struct Tile
{
  std::shared_ptr<const std::string> data;  // decompressed tile bytes (MVT)
  uint8_t z = 0;                            // coordinates of the returned tile, which
  uint32_t x = 0;                           // is the requested tile or its ancestor
  uint32_t y = 0;
  std::uint64_t source = 0;  // id of the source the tile came from, usable in cache keys
};

class TileStore
{
 public:
  explicit TileStore(const Config& theConfig);

  ~TileStore();

  TileStore(const TileStore&) = delete;
  TileStore& operator=(const TileStore&) = delete;
  TileStore(TileStore&&) = delete;
  TileStore& operator=(TileStore&&) = delete;

  // Scan the directories and the database and update the datasets. Thread-safe.
  void rescan();

  std::vector<std::string> datasets() const;
  std::optional<DatasetInfo> info(const std::string& theDataset) const;

  // Most detailed available tile at or above the given tile
  std::optional<Tile> getTile(const std::string& theDataset, uint8_t z, uint32_t x, uint32_t y);

  // Fetch tiles into the cache with as few requests as possible. Used before
  // a series of getTile calls when the sources have a high latency per request.
  void prefetch(const std::string& theDataset, const std::vector<TileCoord>& theTiles);

  Fmi::Cache::CacheStats tileCacheStats() const { return itsTileCache.statistics(); }

 private:
  struct Dataset
  {
    DatasetInfo info;
    std::vector<std::shared_ptr<TileSource>> sources;  // most detailed first
  };
  using Datasets = std::map<std::string, Dataset>;

  std::shared_ptr<const Datasets> snapshot() const;

  std::optional<Tile> findTile(const std::string& theDataset, uint8_t z, uint32_t x, uint32_t y);

  // The sources of each published dataset in the database
  std::map<std::string, std::vector<std::pair<std::shared_ptr<TileSource>, FileInfo>>> scanDatabase();

  struct TileSizeFunction
  {
    static std::size_t getSize(const std::shared_ptr<const std::string>& theTile)
    {
      return (theTile ? theTile->size() : 0) + 100;
    }
  };

  const Config& itsConfig;

  mutable std::mutex itsMutex;               // protects itsDatasets
  std::shared_ptr<const Datasets> itsDatasets;
  std::uint64_t itsVersion = 0;

  // State of the scans, protected by itsScanMutex
  struct OpenFile
  {
    std::string signature;  // path, size, modification time and inode
    std::shared_ptr<PMTilesReader> reader;
  };
  std::mutex itsScanMutex;
  std::map<std::string, OpenFile> itsOpenFiles;  // by path
  std::set<std::string> itsFailedFiles;          // signatures of files which failed to open

  // The database and its sources by id, protected by itsScanMutex. The previous
  // sources are kept if the database cannot be reached.
  std::unique_ptr<PgDatabase> itsDatabase;
  std::map<std::int64_t, std::pair<std::shared_ptr<PgSource>, FileInfo>> itsDatabaseSources;
  bool itsDatabaseFailed = false;

  // Decompressed tiles by source id and tile id. A null tile records that
  // the source does not have the tile.
  using TileKey = std::pair<std::uint64_t, std::uint64_t>;
  Fmi::Cache::Cache<TileKey, std::shared_ptr<const std::string>, TileSizeFunction> itsTileCache;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
