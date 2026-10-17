// ======================================================================
/*!
 * \brief Rendered map as a list of SVG elements
 *
 * The renderer produces SVG elements in pixel coordinates so that the
 * result can be embedded into other SVG documents (WMS/Dali products) or
 * turned into a standalone image (OGC API Maps).
 */
// ======================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
// Options for rendering a map
struct MapOptions
{
  std::string dataset;                // dataset name in the data directory
  std::string style;                  // style name in the style directory
  std::optional<double> zoom;         // style zoom level, by default from the resolution
  double zoomOffset = 0;              // adjustment to the automatic zoom level
  bool labels = true;                 // render symbol layers
  bool icons = true;                  // draw a dot for point symbols with an icon
  std::vector<std::string> include;   // style layer ids or prefixes ending in '*' to render
  std::vector<std::string> exclude;   // style layer ids or prefixes ending in '*' to skip
  std::string fontFamily = "Noto Sans, DejaVu Sans, sans-serif";  // fallback fonts
  double margin = 0;                  // extra pixels to render outside the image
  int precision = 1;                  // decimals in pixel coordinates
  int maxTiles = 400;                 // the zoom level is reduced if more tiles are needed
};

struct SceneElement
{
  std::string tag;                                         // path, text, rect or circle
  std::string layer;                                       // style layer id
  std::vector<std::pair<std::string, std::string>> attributes;  // escaped SVG attributes
  std::string content;  // escaped inner XML for text elements
};

struct Scene
{
  std::size_t width = 0;
  std::size_t height = 0;
  double zoom = 0;            // style zoom level used
  int tileZoom = 0;           // tile zoom level used
  std::size_t tiles = 0;      // number of tiles used
  std::string attribution;    // attribution of the data
  std::vector<SceneElement> elements;

  // Standalone SVG document
  std::string svg() const;

  // Elements only, for embedding into another SVG document
  std::string svgElements() const;
};

using ScenePtr = std::shared_ptr<const Scene>;

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
