// ======================================================================
/*!
 * \brief Vector tile datasets made of one or more PMTiles files
 */
// ======================================================================

#include "TileStore.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace
{
// File identity used to detect replaced or modified files
struct FileSignature
{
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::uint64_t inode = 0;

  std::string str(const std::filesystem::path& path) const
  {
    return fmt::format("{}:{}:{}:{}", path.string(), size, mtime_ns, inode);
  }
};

std::optional<FileSignature> signature(const std::filesystem::path& path)
{
  struct stat st
  {
  };
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return {};
  FileSignature sig;
  sig.size = static_cast<std::uint64_t>(st.st_size);
  sig.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
  sig.inode = static_cast<std::uint64_t>(st.st_ino);
  return sig;
}

bool is_pmtiles(const std::filesystem::path& path)
{
  return path.extension() == ".pmtiles";
}

// The *.pmtiles files directly in the given directory, sorted for stable results
std::vector<std::filesystem::path> list_files(const std::filesystem::path& dir)
{
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
  {
    if (is_pmtiles(entry.path()) && entry.is_regular_file(ec))
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

// Latitude of the northern edge of tile row y at zoom z
double tile_lat(uint32_t y, uint8_t z)
{
  const double n = M_PI - 2.0 * M_PI * y / std::ldexp(1.0, z);
  return 180.0 / M_PI * std::atan(std::sinh(n));
}

bool tile_intersects(const FileInfo& file, uint8_t z, uint32_t x, uint32_t y)
{
  const double n = std::ldexp(1.0, z);
  const double west = x / n * 360.0 - 180.0;
  const double east = (x + 1) / n * 360.0 - 180.0;
  const double north = tile_lat(y, z);
  const double south = tile_lat(y + 1, z);
  return west < file.east && east > file.west && south < file.north && north > file.south;
}

FileInfo file_info(const PMTilesReader& reader, const FileSignature& sig)
{
  const auto& h = reader.header();
  FileInfo info;
  info.path = reader.path();
  info.size = sig.size;
  info.modified = sig.mtime_ns / 1000000000;
  info.minzoom = h.minZoom;
  info.maxzoom = h.maxZoom;
  info.west = h.minLon;
  info.south = h.minLat;
  info.east = h.maxLon;
  info.north = h.maxLat;
  return info;
}

}  // namespace

// ----------------------------------------------------------------------

TileStore::TileStore(const Config& theConfig)
    : itsConfig(theConfig),
      itsDatasets(std::make_shared<const Datasets>()),
      itsTileCache(theConfig.tileCacheBytes())
{
}

// ----------------------------------------------------------------------

std::shared_ptr<const TileStore::Datasets> TileStore::snapshot() const
{
  std::lock_guard<std::mutex> lock(itsMutex);
  return itsDatasets;
}

// ----------------------------------------------------------------------
/*!
 * \brief Scan the configured directories and files
 *
 * Unchanged files keep their readers (and hence their cached tiles), new
 * and modified files are opened, and removed files are dropped. Files which
 * fail to open are reported once per version of the file.
 */
// ----------------------------------------------------------------------

void TileStore::rescan()
{
  try
  {
    std::lock_guard<std::mutex> scanlock(itsScanMutex);

    // Collect the files of each dataset

    std::map<std::string, std::vector<std::filesystem::path>> candidates;

    const auto& root = itsConfig.directory();
    if (!root.empty())
    {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(root, ec))
      {
        const auto& path = entry.path();
        if (entry.is_directory(ec))
        {
          auto files = list_files(path);
          if (!files.empty())
            candidates[path.filename().string()] = std::move(files);
        }
        else if (is_pmtiles(path) && entry.is_regular_file(ec))
          candidates[path.stem().string()].push_back(path);
      }
    }

    // Explicitly configured datasets override directory datasets of the same name
    for (const auto& [name, dc] : itsConfig.datasets())
    {
      std::vector<std::filesystem::path> files = dc.files;
      if (!dc.directory.empty())
      {
        auto dirfiles = list_files(dc.directory);
        files.insert(files.end(), dirfiles.begin(), dirfiles.end());
      }
      candidates[name] = std::move(files);
    }

    auto old = snapshot();
    std::map<std::string, OpenFile> openfiles;  // the files in use after this scan

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const auto min_age_ns = std::int64_t{itsConfig.minFileAge()} * 1000000000;

    auto datasets = std::make_shared<Datasets>();
    bool changed = false;

    for (const auto& [name, files] : candidates)
    {
      Dataset ds;
      ds.info.name = name;

      for (const auto& path : files)
      {
        auto sig = signature(path);
        if (!sig)
          continue;

        // Skip files which may still be being written
        if (now_ns - sig->mtime_ns < min_age_ns)
          continue;

        // Reuse the reader if the file is unchanged
        const auto sigstr = sig->str(path);
        std::shared_ptr<PMTilesReader> reader;
        auto it = itsOpenFiles.find(path.string());
        if (it != itsOpenFiles.end() && it->second.signature == sigstr)
          reader = it->second.reader;

        if (!reader)
        {
          if (itsFailedFiles.count(sigstr) > 0)
            continue;
          try
          {
            reader = std::make_shared<PMTilesReader>(path, itsConfig.leafCacheSize());
            if (reader->header().tileType != TileType::MVT)
            {
              std::cerr << fmt::format("OSM engine: {} does not contain vector tiles, ignored\n",
                                       path.string());
              itsFailedFiles.insert(sigstr);
              continue;
            }
            changed = true;
          }
          catch (const std::exception& e)
          {
            std::cerr << fmt::format("OSM engine: failed to open {}: {}\n", path.string(), e.what());
            itsFailedFiles.insert(sigstr);
            continue;
          }
        }

        openfiles[path.string()] = OpenFile{sigstr, reader};
        ds.info.files.push_back(file_info(*reader, *sig));
        ds.readers.push_back(std::move(reader));
      }

      if (ds.readers.empty())
        continue;

      // Most detailed first, then the smallest area first so that a regional
      // file is preferred over a global one with the same zoom levels.
      std::vector<std::size_t> order(ds.readers.size());
      for (std::size_t i = 0; i < order.size(); i++)
        order[i] = i;
      std::sort(order.begin(),
                order.end(),
                [&ds](std::size_t a, std::size_t b)
                {
                  const auto& fa = ds.info.files[a];
                  const auto& fb = ds.info.files[b];
                  if (fa.maxzoom != fb.maxzoom)
                    return fa.maxzoom > fb.maxzoom;
                  const double area_a = (fa.east - fa.west) * (fa.north - fa.south);
                  const double area_b = (fb.east - fb.west) * (fb.north - fb.south);
                  if (area_a != area_b)
                    return area_a < area_b;
                  return fa.path < fb.path;
                });

      Dataset sorted;
      sorted.info.name = name;
      for (auto i : order)
      {
        sorted.readers.push_back(ds.readers[i]);
        sorted.info.files.push_back(ds.info.files[i]);
      }

      auto& info = sorted.info;
      info.minzoom = 255;
      info.west = 180;
      info.south = 90;
      info.east = -180;
      info.north = -90;
      for (const auto& f : info.files)
      {
        info.minzoom = std::min(info.minzoom, f.minzoom);
        info.maxzoom = std::max(info.maxzoom, f.maxzoom);
        info.west = std::min(info.west, f.west);
        info.south = std::min(info.south, f.south);
        info.east = std::max(info.east, f.east);
        info.north = std::max(info.north, f.north);
        info.modified = std::max(info.modified, f.modified);
      }
      try
      {
        info.metadata = sorted.readers.front()->metadata();
      }
      catch (...)
      {
        // Metadata is informational only
      }

      datasets->emplace(name, std::move(sorted));
    }

    // Detect removed files and datasets

    if (openfiles.size() != itsOpenFiles.size() || old->size() != datasets->size())
      changed = true;
    itsOpenFiles = std::move(openfiles);

    if (!changed)
      return;

    std::lock_guard<std::mutex> lock(itsMutex);
    ++itsVersion;
    for (auto& ds : *datasets)
      ds.second.info.version = itsVersion;
    itsDatasets = datasets;

    for (const auto& ds : *datasets)
    {
      const auto& info = ds.second.info;
      std::cout << fmt::format("OSM engine: dataset '{}' has {} file{}, zoom levels {}-{}\n",
                               info.name,
                               info.files.size(),
                               info.files.size() == 1 ? "" : "s",
                               info.minzoom,
                               info.maxzoom);
    }
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM TileStore::rescan failed");
  }
}

// ----------------------------------------------------------------------

std::vector<std::string> TileStore::datasets() const
{
  std::vector<std::string> names;
  for (const auto& ds : *snapshot())
    names.push_back(ds.first);
  return names;
}

// ----------------------------------------------------------------------

std::optional<DatasetInfo> TileStore::info(const std::string& theDataset) const
{
  auto datasets = snapshot();
  auto it = datasets->find(theDataset);
  if (it == datasets->end())
    return {};
  return it->second.info;
}

// ----------------------------------------------------------------------
/*!
 * \brief Find the most detailed tile at or above the requested tile
 */
// ----------------------------------------------------------------------

std::optional<Tile> TileStore::getTile(const std::string& theDataset,
                                       uint8_t z,
                                       uint32_t x,
                                       uint32_t y) const
{
  try
  {
    if (z > 30 || x >= (1U << z) || y >= (1U << z))
      return {};

    auto datasets = snapshot();
    auto it = datasets->find(theDataset);
    if (it == datasets->end())
      return {};
    const auto& ds = it->second;

    for (int zz = std::min<int>(z, ds.info.maxzoom); zz >= 0; --zz)
    {
      const auto shift = z - zz;
      const uint32_t xx = x >> shift;
      const uint32_t yy = y >> shift;
      const auto tileid = PMTilesReader::tileToId(static_cast<uint8_t>(zz), xx, yy);

      for (std::size_t i = 0; i < ds.readers.size(); i++)
      {
        const auto& file = ds.info.files[i];
        if (zz < file.minzoom || zz > file.maxzoom)
          continue;
        if (!tile_intersects(file, static_cast<uint8_t>(zz), xx, yy))
          continue;

        const auto& reader = ds.readers[i];
        const TileKey key{reader->id(), tileid};

        Tile tile;
        tile.z = static_cast<uint8_t>(zz);
        tile.x = xx;
        tile.y = yy;
        tile.source = reader->id();

        if (auto cached = itsTileCache.find(key))
        {
          tile.data = *cached;
          return tile;
        }

        auto raw = reader->getTile(tile.z, xx, yy);
        if (!raw)
          continue;

        tile.data = std::make_shared<const std::string>(reader->decompressTile(*raw));
        itsTileCache.insert(key, tile.data);
        return tile;
      }
    }
    return {};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM TileStore::getTile failed")
        .addParameter("dataset", theDataset)
        .addParameter("z", std::to_string(z))
        .addParameter("x", std::to_string(x))
        .addParameter("y", std::to_string(y));
  }
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
