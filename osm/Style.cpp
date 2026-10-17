// ======================================================================
/*!
 * \brief MapLibre/Mapbox GL style documents
 */
// ======================================================================

#include "Style.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace
{
std::int64_t modification_time(const std::filesystem::path& path)
{
  std::error_code ec;
  auto t = std::filesystem::last_write_time(path, ec);
  if (ec)
    return 0;
  return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

LayerType layer_type(const std::string& type)
{
  if (type == "background")
    return LayerType::Background;
  if (type == "fill")
    return LayerType::Fill;
  if (type == "line")
    return LayerType::Line;
  if (type == "symbol")
    return LayerType::Symbol;
  return LayerType::Other;
}

// Style names are restricted so that they cannot refer outside the style directory
bool valid_name(const std::string& name)
{
  if (name.empty() || name.size() > 100)
    return false;
  return std::all_of(name.begin(),
                     name.end(),
                     [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

}  // namespace

// ----------------------------------------------------------------------

const Expression* StyleLayer::paintProperty(const std::string& name) const
{
  auto it = paint.find(name);
  return it == paint.end() ? nullptr : &it->second;
}

const Expression* StyleLayer::layoutProperty(const std::string& name) const
{
  auto it = layout.find(name);
  return it == layout.end() ? nullptr : &it->second;
}

// ----------------------------------------------------------------------

Style::Style(const std::filesystem::path& theFile)
    : itsName(theFile.stem().string()), itsModified(modification_time(theFile))
{
  try
  {
    std::ifstream in(theFile);
    if (!in)
      throw Fmi::Exception(BCP, "Failed to open style file");

    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, in, &itsJson, &errors))
      throw Fmi::Exception(BCP, "Failed to parse style").addParameter("error", errors);

    itsTitle = itsJson.get("name", itsName).asString();

    const auto& layers = itsJson["layers"];
    if (!layers.isArray())
      throw Fmi::Exception(BCP, "Style has no layers array");

    for (const auto& json : layers)
    {
      StyleLayer layer;
      layer.id = json.get("id", "").asString();
      try
      {
        layer.type = layer_type(json.get("type", "").asString());
        if (layer.type == LayerType::Other)
          continue;

        layer.sourceLayer = json.get("source-layer", "").asString();
        layer.minzoom = json.get("minzoom", 0.0).asDouble();
        layer.maxzoom = json.get("maxzoom", 24.0).asDouble();

        if (json.isMember("filter"))
          layer.filter = Expression::compileFilter(json["filter"]);

        const auto& paint = json["paint"];
        if (paint.isObject())
          for (const auto& name : paint.getMemberNames())
            layer.paint.emplace(name, Expression::compile(paint[name]));

        const auto& layout = json["layout"];
        if (layout.isObject())
        {
          for (const auto& name : layout.getMemberNames())
          {
            if (name == "visibility")
              layer.visible = (layout[name].asString() != "none");
            else
              layer.layout.emplace(name, Expression::compile(layout[name]));
          }
        }

        if (!layer.sourceLayer.empty())
          itsSourceLayers.insert(layer.sourceLayer);
        itsLayers.push_back(std::move(layer));
      }
      catch (...)
      {
        // Skip the layer but keep the rest of the style usable
        Fmi::Exception ex(BCP, "Skipping unsupported style layer", nullptr);
        ex.addParameter("style", itsName).addParameter("layer", layer.id);
        ex.printError();
      }
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to read map style")
        .addParameter("file", theFile.string());
  }
}

// ----------------------------------------------------------------------

StyleStore::StyleStore(std::filesystem::path theDirectory) : itsDirectory(std::move(theDirectory))
{
}

StylePtr StyleStore::get(const std::string& theName) const
{
  try
  {
    if (!valid_name(theName))
      throw Fmi::Exception(BCP, "Invalid map style name").addParameter("style", theName);

    const auto path = itsDirectory / (theName + ".json");
    const auto modified = modification_time(path);
    if (modified == 0)
      throw Fmi::Exception(BCP, "Unknown map style").addParameter("style", theName);

    {
      std::lock_guard<std::mutex> lock(itsMutex);
      auto it = itsStyles.find(theName);
      if (it != itsStyles.end() && it->second->modified() == modified)
        return it->second;
    }

    // Parse outside the lock, styles are big
    auto style = std::make_shared<const Style>(path);
    std::lock_guard<std::mutex> lock(itsMutex);
    itsStyles[theName] = style;
    return style;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to get map style");
  }
}

std::vector<std::string> StyleStore::names() const
{
  std::vector<std::string> ret;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(itsDirectory, ec))
  {
    const auto& path = entry.path();
    if (path.extension() == ".json" && valid_name(path.stem().string()))
      ret.push_back(path.stem().string());
  }
  std::sort(ret.begin(), ret.end());
  return ret;
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
