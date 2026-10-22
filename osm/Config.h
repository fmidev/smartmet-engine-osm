// ======================================================================
/*!
 * \brief OSM engine configuration
 */
// ======================================================================

#pragma once

#include "PMTilesReader.h"
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

// Explicitly configured dataset: all *.pmtiles files in a directory, or a
// list of files. The files of a dataset must use the same vector tile schema.
struct DatasetConfig
{
  std::filesystem::path directory;
  std::vector<std::filesystem::path> files;
};

// Tile database
struct PgConfig
{
  std::string host = "localhost";
  unsigned int port = 5432;
  std::string database;
  std::string username;
  std::string password;
  unsigned int connectTimeout = 10;  // seconds
  std::string schema = "osm";
  unsigned int maxIdleConnections = 8;  // connections kept open between requests
};

class Config
{
 public:
  explicit Config(const std::string& configFile);

  // Root directory: each subdirectory is a dataset named after the subdirectory,
  // each *.pmtiles file directly in the root is a dataset named after the file.
  const std::filesystem::path& directory() const { return itsDirectory; }

  // Explicitly named datasets
  const std::map<std::string, DatasetConfig>& datasets() const { return itsDatasets; }

  // Maximum number of decoded leaf directories kept per file
  std::size_t leafCacheSize() const { return itsLeafCacheSize; }

  // Maximum total size of decompressed tiles kept in memory in bytes
  std::size_t tileCacheBytes() const { return std::size_t{itsTileCacheMB} * 1024 * 1024; }

  // Directory of the MapLibre styles
  const std::filesystem::path& styleDirectory() const { return itsStyleDirectory; }

  // Maximum total size of decoded tiles kept in memory in bytes
  std::size_t decodedCacheBytes() const { return std::size_t{itsDecodedCacheMB} * 1024 * 1024; }

  // Seconds between directory rescans, 0 disables rescanning
  int rescanInterval() const { return itsRescanInterval; }

  // Files modified less than this many seconds ago are not opened yet
  int minFileAge() const { return itsMinFileAge; }

  // How PMTiles files are read
  FileAccess fileAccess() const { return itsFileAccess; }

  // The tile database, if any
  const std::optional<PgConfig>& database() const { return itsDatabase; }

 private:
  std::filesystem::path itsDirectory;
  std::map<std::string, DatasetConfig> itsDatasets;
  unsigned int itsLeafCacheSize = 256;
  std::filesystem::path itsStyleDirectory = "/usr/share/smartmet/osm/styles";
  unsigned int itsTileCacheMB = 512;
  unsigned int itsDecodedCacheMB = 1024;
  int itsRescanInterval = 60;
  int itsMinFileAge = 10;
  FileAccess itsFileAccess = FileAccess::Auto;
  std::optional<PgConfig> itsDatabase;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
