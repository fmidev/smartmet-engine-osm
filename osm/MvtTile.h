// ======================================================================
/*!
 * \brief Lightweight Mapbox Vector Tile decoder and encoder
 *
 * Decodes MVT 2.1 tiles directly from the protobuf wire format. Only the
 * layers needed by the style are decoded, which matters for detailed
 * basemap tiles full of buildings and points of interest.
 *
 * Reference: https://github.com/mapbox/vector-tile-spec/tree/master/2.1
 */
// ======================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace Mvt
{
// A property value of a feature
using PropertyValue = std::variant<std::monostate, bool, double, std::string>;

// Geometry types as in the MVT specification
enum class GeomType : uint8_t
{
  Unknown = 0,
  Point = 1,
  LineString = 2,
  Polygon = 3
};

struct Point
{
  double x;
  double y;
};

// Points (one point per part), linestrings or polygon rings. Polygon
// exterior rings are clockwise and holes counterclockwise in tile
// coordinates (y axis down), so the nonzero fill rule renders them right.
using Part = std::vector<Point>;

struct Feature
{
  GeomType type = GeomType::Unknown;
  std::vector<uint32_t> tags;  // key and value indexes into the layer tables
  std::vector<Part> parts;
};

struct Layer
{
  std::string name;
  uint32_t extent = 4096;
  std::vector<std::string> keys;
  std::vector<PropertyValue> values;
  std::vector<Feature> features;

  // Value of a feature property, nullptr if the feature does not have it
  const PropertyValue* get(const Feature& feature, std::string_view key) const;
};

struct Tile
{
  std::vector<Layer> layers;

  const Layer* layer(std::string_view name) const;

  // Approximate memory use for cache accounting
  std::size_t memoryUse() const;
};

using TilePtr = std::shared_ptr<const Tile>;

// Decode a tile. If wanted is not null, only the named layers are decoded.
Tile decode(std::string_view data, const std::set<std::string>* wanted = nullptr);

// Encode a tile. Coordinates are rounded to integers.
std::string encode(const Tile& tile);

// Crop and scale a tile to one of its descendants (overzooming). dz is the
// zoom level difference and (dx,dy) the position of the descendant within
// the tile, 0 <= dx,dy < 2^dz. The buffer is the amount of data kept outside
// the descendant in its own coordinate units.
Tile overzoom(const Tile& tile, int dz, uint32_t dx, uint32_t dy, double buffer = 64);

}  // namespace Mvt
}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
