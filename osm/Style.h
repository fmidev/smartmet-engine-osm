// ======================================================================
/*!
 * \brief MapLibre/Mapbox GL style documents
 *
 * The styles are read from the configured style directory, one style per
 * JSON file, and named after the file. Only the parts relevant to server
 * side rendering are parsed: background, fill, line and symbol layers.
 */
// ======================================================================

#pragma once

#include "Expression.h"
#include <filesystem>
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
enum class LayerType
{
  Background,
  Fill,
  Line,
  Symbol,
  Other
};

struct StyleLayer
{
  std::string id;
  LayerType type = LayerType::Other;
  std::string sourceLayer;
  double minzoom = 0;
  double maxzoom = 24;
  bool visible = true;
  std::optional<Expression> filter;
  std::map<std::string, Expression> paint;
  std::map<std::string, Expression> layout;

  const Expression* paintProperty(const std::string& name) const;
  const Expression* layoutProperty(const std::string& name) const;

  bool isVisibleAt(double zoom) const { return visible && zoom >= minzoom && zoom < maxzoom; }
};

class Style
{
 public:
  explicit Style(const std::filesystem::path& theFile);

  const std::string& name() const { return itsName; }
  const std::string& title() const { return itsTitle; }
  const std::vector<StyleLayer>& layers() const { return itsLayers; }
  const std::set<std::string>& sourceLayers() const { return itsSourceLayers; }

  // The original document, for clients rendering the style themselves
  const Json::Value& json() const { return itsJson; }

  std::int64_t modified() const { return itsModified; }

 private:
  std::string itsName;
  std::string itsTitle;
  Json::Value itsJson;
  std::vector<StyleLayer> itsLayers;
  std::set<std::string> itsSourceLayers;
  std::int64_t itsModified = 0;
};

using StylePtr = std::shared_ptr<const Style>;

// Cache of the styles in a directory, reloaded when the files change
class StyleStore
{
 public:
  explicit StyleStore(std::filesystem::path theDirectory);

  // Throws if there is no such style
  StylePtr get(const std::string& theName) const;

  // Names of the available styles
  std::vector<std::string> names() const;

 private:
  std::filesystem::path itsDirectory;
  mutable std::mutex itsMutex;
  mutable std::map<std::string, StylePtr> itsStyles;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
