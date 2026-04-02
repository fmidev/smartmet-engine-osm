// ======================================================================
/*!
 * \brief OSM engine implementation
 */
// ======================================================================

#include "Engine.h"
#include <macgyver/Exception.h>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

// ----------------------------------------------------------------------

Engine::Engine(const std::string& configFile) : itsConfigFile(configFile)
{
}

// ----------------------------------------------------------------------
/*!
 * \brief Open and memory-map all configured PMTiles sources
 */
// ----------------------------------------------------------------------

void Engine::init()
{
  try
  {
    itsConfig = std::make_unique<Config>(itsConfigFile);

    for (const auto& [name, sc] : itsConfig->sources())
    {
      try
      {
        itsSources.emplace(name, std::make_unique<PMTilesReader>(sc.file));
      }
      catch (const Fmi::Exception& e)
      {
        // Log the error but continue — a missing file should not prevent
        // other sources from loading.  The source will simply return nullopt.
        std::cerr << "OSM engine: failed to open source '" << name
                  << "': " << e.what() << std::endl;
      }
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM Engine::init failed");
  }
}

// ----------------------------------------------------------------------

void Engine::shutdown()
{
  // PMTilesReader destructors call munmap + close.
  itsSources.clear();
}

// ----------------------------------------------------------------------
/*!
 * \brief Return tile data from the mmap'd file (zero-copy)
 */
// ----------------------------------------------------------------------

std::optional<TileData> Engine::getTile(const std::string& source,
                                        uint8_t z,
                                        uint32_t x,
                                        uint32_t y) const
{
  try
  {
    auto it = itsSources.find(source);
    if (it == itsSources.end())
      return {};
    return it->second->getTile(z, x, y);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM Engine::getTile failed");
  }
}

// ----------------------------------------------------------------------

std::int64_t Engine::getDataTimestamp(const std::string& source) const
{
  auto it = itsSources.find(source);
  if (it == itsSources.end())
    return 0;
  return it->second->fileTimestamp();
}

// ----------------------------------------------------------------------

std::vector<std::string> Engine::getSources() const
{
  std::vector<std::string> names;
  names.reserve(itsSources.size());
  for (const auto& [name, _] : itsSources)
    names.push_back(name);
  return names;
}

// ----------------------------------------------------------------------

const PMTilesHeader* Engine::getHeader(const std::string& source) const
{
  auto it = itsSources.find(source);
  if (it == itsSources.end())
    return nullptr;
  return &it->second->header();
}

// ----------------------------------------------------------------------

Fmi::Cache::CacheStatistics Engine::getCacheStats() const
{
  // Leaf directory caches are unordered_maps internal to each PMTilesReader;
  // size reporting not exposed at this level.
  return {};
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet

// ======================================================================
//  Plugin entry points
// ======================================================================

extern "C" void* engine_class_creator(const char* configfile, void* /* user_data */)
{
  return new SmartMet::Engine::OSM::Engine(configfile);
}

extern "C" const char* engine_name()
{
  return "OSM";
}
