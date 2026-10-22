// ======================================================================
/*!
 * \brief A source of vector tiles: a PMTiles file or a database table
 *
 * A dataset consists of one or more sources sharing the same tile schema.
 * Each source instance has a unique id, which the caches use in their keys:
 * a replaced file or a new version in the database gets a new instance and
 * hence never returns tiles cached from its predecessor.
 */
// ======================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
// PMTiles v3 compression codes
enum class Compression : uint8_t
{
  Unknown = 0,
  None = 1,
  Gzip = 2,
  Brotli = 3,
  Zstd = 4
};

// PMTiles v3 tile types
enum class TileType : uint8_t
{
  Unknown = 0,
  MVT = 1,
  PNG = 2,
  JPEG = 3,
  WebP = 4,
  AVIF = 5
};

// Tile coordinates in the WebMercatorQuad tiling
struct TileCoord
{
  uint8_t z = 0;
  uint32_t x = 0;
  uint32_t y = 0;
};

// What a source contains
struct SourceHeader
{
  TileType tileType = TileType::Unknown;
  Compression tileCompression = Compression::Unknown;
  uint8_t minZoom = 0;
  uint8_t maxZoom = 0;
  double west = -180;
  double south = -85.0511;
  double east = 180;
  double north = 85.0511;
};

class TileSource
{
 public:
  TileSource();
  virtual ~TileSource();

  TileSource(const TileSource&) = delete;
  TileSource& operator=(const TileSource&) = delete;
  TileSource(TileSource&&) = delete;
  TileSource& operator=(TileSource&&) = delete;

  // Unique id of this instance, used in cache keys
  std::uint64_t id() const { return itsId; }

  const SourceHeader& summary() const { return itsSummary; }

  // The decompressed tile, std::nullopt if the source does not have it
  virtual std::optional<std::string> getDecompressedTile(uint8_t z,
                                                         uint32_t x,
                                                         uint32_t y) const = 0;

  // Several tiles at once. Sources with a high latency per request override
  // this.
  virtual std::vector<std::optional<std::string>> getDecompressedTiles(
      const std::vector<TileCoord>& theTiles) const;

  // True if fetching several tiles at once is faster than one at a time
  virtual bool batched() const { return false; }

  // The JSON metadata, empty if there is none
  virtual std::string metadata() const = 0;

  // True if reading has failed in a way which requires reopening the source,
  // for example when a file on NFS has been replaced (ESTALE)
  virtual bool failed() const { return false; }

  // Convert tile coordinates to the PMTiles Hilbert-curve tile id
  static uint64_t tileToId(uint8_t z, uint32_t x, uint32_t y);

 protected:
  void setSummary(const SourceHeader& theSummary) { itsSummary = theSummary; }

 private:
  SourceHeader itsSummary;
  std::uint64_t itsId = 0;
};

// Decompress data compressed with the given PMTiles compression
std::vector<std::byte> decompress(const std::byte* theData,
                                  std::size_t theSize,
                                  Compression theCompression);

// Same into a string
std::string decompressToString(const std::byte* theData,
                               std::size_t theSize,
                               Compression theCompression);

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
