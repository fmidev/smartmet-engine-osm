// ======================================================================
/*!
 * \brief OSM engine configuration
 */
// ======================================================================

#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

struct SourceConfig
{
  std::filesystem::path file;  // Path to the .pmtiles file
};

class Config
{
 public:
  explicit Config(const std::string& configFile);

  // Named tile sources, e.g. "scandinavia", "global"
  const std::map<std::string, SourceConfig>& sources() const { return itsSources; }

  // Maximum number of leaf directories to keep in the per-file cache
  std::size_t leafCacheSize() const { return itsLeafCacheSize; }

 private:
  std::map<std::string, SourceConfig> itsSources;
  unsigned itsLeafCacheSize = 1024;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
