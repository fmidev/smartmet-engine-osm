// ======================================================================
/*!
 * \brief Server side rendering of vector tile datasets with MapLibre styles
 */
// ======================================================================

#pragma once

#include "MvtTile.h"
#include "Scene.h"
#include "Style.h"
#include "TileStore.h"
#include <macgyver/Cache.h>
#include <optional>
#include <string>

namespace Fmi
{
class Box;
class SpatialReference;
}  // namespace Fmi

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
class Renderer
{
 public:
  Renderer(const TileStore& theTiles, const StyleStore& theStyles, std::size_t theCacheBytes);

  // Render the area described by the CRS and the box (bbox and image size)
  ScenePtr render(const MapOptions& theOptions,
                  const Fmi::SpatialReference& theCRS,
                  const Fmi::Box& theBox) const;

  // A vector tile in the WebMercatorQuad tiling. Tiles beyond the available
  // zoom levels are cut from their ancestors (overzooming). Returns
  // std::nullopt if the dataset does not cover the tile.
  std::optional<std::string> getVectorTile(const std::string& theDataset,
                                           uint8_t z,
                                           uint32_t x,
                                           uint32_t y) const;

  // Decode a tile using the cache
  Mvt::TilePtr decode(const Tile& theTile,
                      const std::set<std::string>* theLayers,
                      std::size_t theLayerHash) const;

  Fmi::Cache::CacheStats cacheStats() const { return itsCache.statistics(); }

 private:
  struct TileSizeFunction
  {
    static std::size_t getSize(const Mvt::TilePtr& theTile) { return theTile->memoryUse(); }
  };

  const TileStore& itsTiles;
  const StyleStore& itsStyles;

  // Decoded tiles keyed by file id, tile id and the hash of the decoded layer set
  using Key = std::pair<std::pair<std::uint64_t, std::uint64_t>, std::size_t>;
  mutable Fmi::Cache::Cache<Key, Mvt::TilePtr, TileSizeFunction> itsCache;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
