// ======================================================================
/*!
 * \brief Vector tile datasets made of one or more PMTiles files
 *
 * A dataset is a set of PMTiles files sharing the same vector tile schema,
 * for example a planet file limited to zoom level 8 and a regional file with
 * all zoom levels. A tile request is answered from the most detailed file
 * containing the tile. If no file has the requested zoom level for the
 * location, the nearest ancestor tile is returned instead and the caller
 * is expected to overzoom it. Services can thus be localized just by
 * placing a more detailed file for the local region into the directory.
 *
 * The directories are rescanned periodically. Changed files are reopened
 * and readers of replaced files stay valid until their last user is done.
 * New files must be moved into place atomically (rename), overwriting a
 * mapped file in place crashes the server (SIGBUS). For the same reason
 * the files should be on a local disk, not on NFS.
 */
// ======================================================================

#pragma once

#include "Config.h"
#include "PMTilesReader.h"
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
// Information on a single file of a dataset
struct FileInfo
{
  std::filesystem::path path;
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
  std::int64_t modified = 0;   // latest modification time of the files
  std::uint64_t version = 0;   // changes whenever the set of files changes
  std::string metadata;        // JSON metadata of the most detailed file
  std::vector<FileInfo> files; // most detailed first
};

// A decompressed tile
struct Tile
{
  std::shared_ptr<const std::string> data;  // decompressed tile bytes (MVT)
  uint8_t z = 0;                            // coordinates of the returned tile, which
  uint32_t x = 0;                           // is the requested tile or its ancestor
  uint32_t y = 0;
  std::uint64_t source = 0;  // id of the file the tile came from, usable in cache keys
};

class TileStore
{
 public:
  explicit TileStore(const Config& theConfig);

  // Scan the directories and update the datasets. Thread-safe.
  void rescan();

  std::vector<std::string> datasets() const;
  std::optional<DatasetInfo> info(const std::string& theDataset) const;

  // Most detailed available tile at or above the given tile
  std::optional<Tile> getTile(const std::string& theDataset, uint8_t z, uint32_t x, uint32_t y) const;

  Fmi::Cache::CacheStats tileCacheStats() const { return itsTileCache.statistics(); }

 private:
  struct Dataset
  {
    DatasetInfo info;
    std::vector<std::shared_ptr<PMTilesReader>> readers;  // most detailed first
  };
  using Datasets = std::map<std::string, Dataset>;

  std::shared_ptr<const Datasets> snapshot() const;

  struct TileSizeFunction
  {
    static std::size_t getSize(const std::shared_ptr<const std::string>& theTile)
    {
      return theTile->size() + 100;
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

  using TileKey = std::pair<std::uint64_t, std::uint64_t>;  // reader id, tile id
  mutable Fmi::Cache::Cache<TileKey, std::shared_ptr<const std::string>, TileSizeFunction>
      itsTileCache;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
