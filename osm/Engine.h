// ======================================================================
/*!
 * \brief OSM engine
 *
 * Provides memory-mapped access to PMTiles v3 files containing pre-tiled
 * OpenStreetMap vector data.
 *
 * Each configured source is a single .pmtiles file that is opened once
 * at startup and memory-mapped with mmap(MAP_SHARED, PROT_READ). The
 * kernel's page cache manages resident pages; the application never
 * performs explicit memory management for tile data.
 *
 * The engine exposes two backends to the Dali WMS plugin:
 *
 *  1. File-based (this engine):
 *       getTile(source, z, x, y)  →  raw MVT bytes (zero-copy from mmap)
 *
 *  2. PostGIS-based:
 *       Use the existing GIS engine directly (OSMLayer already does this).
 *
 * Usage in OSMLayer configuration:
 *   "backend": "pmtiles",
 *   "source":  "scandinavia"
 */
// ======================================================================

#pragma once

#include "Config.h"
#include "PMTilesReader.h"
#include <macgyver/Cache.h>
#include <spine/SmartMetEngine.h>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

class Engine : public SmartMet::Spine::SmartMetEngine
{
 public:
  Engine() = delete;
  explicit Engine(const std::string& configFile);

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  // Return a zero-copy view of the raw tile bytes from the mmap'd file.
  // The returned TileData points into the memory-mapped region; the caller
  // must not free it and must not use it after the Engine is destroyed.
  // Returns std::nullopt if the source is unknown or the tile does not exist.
  std::optional<TileData> getTile(const std::string& source,
                                  uint8_t z,
                                  uint32_t x,
                                  uint32_t y) const;

  // File modification time of the source .pmtiles file (seconds since epoch).
  // Returns 0 if the source is unknown.
  std::int64_t getDataTimestamp(const std::string& source) const;

  // List all configured source names
  std::vector<std::string> getSources() const;

  // Header metadata for a source (e.g. zoom range, bounding box)
  const PMTilesHeader* getHeader(const std::string& source) const;

 protected:
  void init() override;
  void shutdown() override;

 private:
  Fmi::Cache::CacheStatistics getCacheStats() const override;

  std::string itsConfigFile;
  std::unique_ptr<Config> itsConfig;

  // One PMTilesReader per named source, opened during init()
  std::map<std::string, std::unique_ptr<PMTilesReader>> itsSources;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
