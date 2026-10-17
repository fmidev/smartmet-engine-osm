// ======================================================================
/*!
 * \brief OSM engine implementation
 */
// ======================================================================

#include "Engine.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <iostream>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
// ----------------------------------------------------------------------

Engine::Engine(const std::string& configFile) : itsConfigFile(configFile) {}

// ----------------------------------------------------------------------

Engine::~Engine()
{
  try
  {
    shutdown();
  }
  catch (...)
  {
    // Destructors must not throw
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Scan the data directories and start the rescan thread
 */
// ----------------------------------------------------------------------

void Engine::init()
{
  try
  {
    itsConfig = std::make_unique<Config>(itsConfigFile);
    itsStore = std::make_unique<TileStore>(*itsConfig);
    itsStore->rescan();
    itsStyles = std::make_unique<StyleStore>(itsConfig->styleDirectory());
    itsRenderer =
        std::make_unique<Renderer>(*itsStore, *itsStyles, itsConfig->decodedCacheBytes());

    if (itsStore->datasets().empty())
      std::cerr << fmt::format("OSM engine: no PMTiles datasets found in '{}'\n",
                               itsConfig->directory().string());

    if (itsConfig->rescanInterval() > 0)
      itsRescanThread = std::thread([this] { rescanLoop(); });
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM Engine::init failed");
  }
}

// ----------------------------------------------------------------------

void Engine::shutdown()
{
  {
    std::lock_guard<std::mutex> lock(itsRescanMutex);
    itsShutdownRequested = true;
  }
  itsRescanCondition.notify_all();
  if (itsRescanThread.joinable())
    itsRescanThread.join();
}

// ----------------------------------------------------------------------

void Engine::rescanLoop()
{
  const auto interval = std::chrono::seconds(itsConfig->rescanInterval());
  while (true)
  {
    {
      std::unique_lock<std::mutex> lock(itsRescanMutex);
      if (itsRescanCondition.wait_for(lock, interval, [this] { return itsShutdownRequested; }))
        return;
    }
    try
    {
      itsStore->rescan();
    }
    catch (...)
    {
      Fmi::Exception ex(BCP, "OSM engine rescan failed", nullptr);
      ex.printError();
    }
  }
}

// ----------------------------------------------------------------------

void Engine::rescan()
{
  if (itsStore)
    itsStore->rescan();
}

// ----------------------------------------------------------------------

std::vector<std::string> Engine::getDatasets() const
{
  if (!itsStore)
    return {};
  return itsStore->datasets();
}

// ----------------------------------------------------------------------

std::optional<DatasetInfo> Engine::getDatasetInfo(const std::string& dataset) const
{
  if (!itsStore)
    return {};
  return itsStore->info(dataset);
}

// ----------------------------------------------------------------------

std::optional<Tile> Engine::getTile(const std::string& dataset,
                                    uint8_t z,
                                    uint32_t x,
                                    uint32_t y) const
{
  try
  {
    if (!itsStore)
      return {};
    return itsStore->getTile(dataset, z, x, y);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM Engine::getTile failed");
  }
}

// ----------------------------------------------------------------------

ScenePtr Engine::renderMap(const MapOptions& options,
                           const Fmi::SpatialReference& crs,
                           const Fmi::Box& box) const
{
  if (!itsRenderer)
    throw Fmi::Exception(BCP, "OSM engine has not been initialized");
  return itsRenderer->render(options, crs, box);
}

// ----------------------------------------------------------------------

std::optional<std::string> Engine::getVectorTile(const std::string& dataset,
                                                 uint8_t z,
                                                 uint32_t x,
                                                 uint32_t y) const
{
  if (!itsRenderer)
    return {};
  return itsRenderer->getVectorTile(dataset, z, x, y);
}

// ----------------------------------------------------------------------

std::vector<std::string> Engine::getStyles() const
{
  if (!itsStyles)
    return {};
  return itsStyles->names();
}

// ----------------------------------------------------------------------

StylePtr Engine::getStyle(const std::string& name) const
{
  if (!itsStyles)
    throw Fmi::Exception(BCP, "OSM engine has not been initialized");
  return itsStyles->get(name);
}

// ----------------------------------------------------------------------

std::int64_t Engine::getDataTimestamp(const std::string& dataset) const
{
  auto info = getDatasetInfo(dataset);
  if (!info)
    return 0;
  return info->modified;
}

// ----------------------------------------------------------------------

Fmi::Cache::CacheStatistics Engine::getCacheStats() const
{
  Fmi::Cache::CacheStatistics ret;
  if (itsStore)
    ret.insert(std::make_pair("OSM::tiles", itsStore->tileCacheStats()));
  if (itsRenderer)
    ret.insert(std::make_pair("OSM::decoded_tiles", itsRenderer->cacheStats()));
  return ret;
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
