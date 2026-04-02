// ======================================================================
/*!
 * \brief OSM engine configuration implementation
 */
// ======================================================================

#include "Config.h"
#include <macgyver/Exception.h>
#include <libconfig.h++>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

Config::Config(const std::string& configFile)
{
  try
  {
    libconfig::Config cfg;
    cfg.readFile(configFile.c_str());

    // Optional leaf directory cache size
    cfg.lookupValue("leaf_cache_size", itsLeafCacheSize);

    // sources:
    // {
    //   scandinavia: { file = "/var/smartmet/osm/scandinavia.pmtiles"; }
    //   global:      { file = "/var/smartmet/osm/global.pmtiles"; }
    // }
    if (!cfg.exists("sources"))
      throw Fmi::Exception(BCP, "OSM engine config: 'sources' group is required");

    const auto& sources = cfg.lookup("sources");
    if (!sources.isGroup())
      throw Fmi::Exception(BCP, "OSM engine config: 'sources' must be a group");

    for (int i = 0; i < sources.getLength(); ++i)
    {
      const auto& src = sources[i];
      std::string name = src.getName();

      SourceConfig sc;
      std::string filePath;
      if (!src.lookupValue("file", filePath))
        throw Fmi::Exception(BCP, "OSM engine: source '" + name + "' missing 'file'");
      sc.file = filePath;

      itsSources.emplace(std::move(name), std::move(sc));
    }

    if (itsSources.empty())
      throw Fmi::Exception(BCP, "OSM engine config: no sources defined");
  }
  catch (const libconfig::ParseException& e)
  {
    throw Fmi::Exception(BCP, "OSM engine config parse error at line " +
                                   std::to_string(e.getLine()) + ": " + e.getError());
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM engine Config constructor failed");
  }
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
