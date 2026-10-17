// ======================================================================
/*!
 * \brief OSM engine
 *
 * Provides access to vector tile datasets stored as PMTiles v3 files,
 * typically OpenStreetMap basemaps such as the Protomaps builds.
 *
 * The data is not packaged: the engine serves whatever is found in the
 * configured directory. Each subdirectory of the directory is a dataset
 * whose files share the same tile schema, and each *.pmtiles file directly
 * in the directory is a single-file dataset. For example
 *
 *    /smartmet/data/osm/protomaps/planet-z8.pmtiles
 *    /smartmet/data/osm/protomaps/nordic-z15.pmtiles
 *
 * forms the dataset "protomaps" with global data up to zoom level 8 and
 * all the detail for the Nordic countries. Requests are answered from the
 * most detailed file available, falling back to ancestor tiles when no file
 * has the requested zoom level (the caller overzooms them).
 *
 * The files are memory-mapped, and the directories are rescanned
 * periodically. Install new files with an atomic rename.
 *
 * Maps are rendered server side with MapLibre styles read from the style
 * directory, producing SVG elements which can be embedded into WMS products
 * or turned into images by an OGC API Maps service.
 */
// ======================================================================

#pragma once

#include "Config.h"
#include "Renderer.h"
#include "Scene.h"
#include "Style.h"
#include "TileStore.h"
#include <spine/SmartMetEngine.h>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
class Engine : public SmartMet::Spine::SmartMetEngine
{
 public:
  // The interface is virtual so that plugins do not need to link against
  // the engine: the engine is optional and may not be loaded.

  Engine() = delete;
  explicit Engine(const std::string& configFile);

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  ~Engine() override;

  // Names of the available datasets
  virtual std::vector<std::string> getDatasets() const;

  // Dataset information, empty if there is no such dataset
  virtual std::optional<DatasetInfo> getDatasetInfo(const std::string& dataset) const;

  // The most detailed decompressed tile at or above the requested tile.
  // The returned tile coordinates tell which tile was found. Returns
  // std::nullopt if the dataset does not cover the location at all.
  virtual std::optional<Tile> getTile(const std::string& dataset,
                                      uint8_t z,
                                      uint32_t x,
                                      uint32_t y) const;

  // Modification time of the newest file of the dataset (seconds since epoch),
  // 0 if there is no such dataset
  virtual std::int64_t getDataTimestamp(const std::string& dataset) const;

  // Render a map. The box defines the area in the given CRS and the image size.
  // Throws if the dataset or the style is not available.
  virtual ScenePtr renderMap(const MapOptions& options,
                             const Fmi::SpatialReference& crs,
                             const Fmi::Box& box) const;

  // A vector tile (MVT) in the WebMercatorQuad tiling. Tiles beyond the
  // available resolution are cut from the nearest ancestor. Returns
  // std::nullopt if the dataset does not cover the tile.
  virtual std::optional<std::string> getVectorTile(const std::string& dataset,
                                                   uint8_t z,
                                                   uint32_t x,
                                                   uint32_t y) const;

  // Available map styles
  virtual std::vector<std::string> getStyles() const;

  // A map style, throws if there is no such style
  virtual StylePtr getStyle(const std::string& name) const;

  // Rescan the data directories immediately
  virtual void rescan();

 protected:
  void init() override;
  void shutdown() override;

 private:
  Fmi::Cache::CacheStatistics getCacheStats() const override;

  void rescanLoop();

  std::string itsConfigFile;
  std::unique_ptr<Config> itsConfig;
  std::unique_ptr<TileStore> itsStore;
  std::unique_ptr<StyleStore> itsStyles;
  std::unique_ptr<Renderer> itsRenderer;

  std::thread itsRescanThread;
  std::mutex itsRescanMutex;
  std::condition_variable itsRescanCondition;
  bool itsShutdownRequested = false;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
