// ======================================================================
/*!
 * \brief PMTiles v3 reader
 *
 * Provides zero-copy, memory-mapped access to PMTiles v3 files.
 * The entire file is mapped with mmap(MAP_SHARED, PROT_READ) so that
 * the kernel's page cache manages resident pages — no explicit memory
 * management is done by the application.
 *
 * Directory decompression (gzip / zstd) is the only place where heap
 * allocations occur, and those are cached per-file.
 *
 * Reference: https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
 */
// ======================================================================

#pragma once

#include <macgyver/Cache.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

// ======================================================================
//  PMTiles v3 compression codes
// ======================================================================

enum class Compression : uint8_t
{
  Unknown = 0,
  None = 1,
  Gzip = 2,
  Brotli = 3,
  Zstd = 4
};

// ======================================================================
//  PMTiles v3 tile types
// ======================================================================

enum class TileType : uint8_t
{
  Unknown = 0,
  MVT = 1,
  PNG = 2,
  JPEG = 3,
  WebP = 4,
  AVIF = 5
};

// ======================================================================
//  PMTiles v3 header (127 bytes, little-endian)
// ======================================================================

struct PMTilesHeader
{
  uint64_t rootDirOffset;
  uint64_t rootDirLength;
  uint64_t metadataOffset;
  uint64_t metadataLength;
  uint64_t leafDirsOffset;
  uint64_t leafDirsLength;
  uint64_t tileDataOffset;
  uint64_t tileDataLength;
  uint64_t numAddressedTiles;
  uint64_t numTileEntries;
  uint64_t numTileContents;
  bool clustered;
  Compression internalCompression;
  Compression tileCompression;
  TileType tileType;
  uint8_t minZoom;
  uint8_t maxZoom;
  float minLon;
  float minLat;
  float maxLon;
  float maxLat;
  uint8_t centerZoom;
  float centerLon;
  float centerLat;
};

// ======================================================================
//  Directory entry
// ======================================================================

struct Entry
{
  uint64_t tileId;
  uint64_t offset;
  uint32_t length;
  uint32_t runLength;  // 0 = pointer to leaf directory
};

using Directory = std::vector<Entry>;
using DirectoryPtr = std::shared_ptr<const Directory>;

// ======================================================================
//  Zero-copy view into the mmap'd tile data (C++17 compatible span substitute)
// ======================================================================

struct TileData
{
  const std::byte* data = nullptr;
  std::size_t size = 0;
};

// ======================================================================
//  PMTilesReader
//
//  One instance per .pmtiles file. Thread-safe for concurrent reads.
// ======================================================================

class PMTilesReader
{
 public:
  // The leaf cache size is the number of decoded leaf directories kept in memory.
  // A planet file has a few thousand leaves of roughly 1.5 MB each when decoded.
  explicit PMTilesReader(const std::filesystem::path& path, std::size_t leafCacheSize = 256);
  ~PMTilesReader();

  PMTilesReader(const PMTilesReader&) = delete;
  PMTilesReader& operator=(const PMTilesReader&) = delete;
  PMTilesReader(PMTilesReader&&) = delete;
  PMTilesReader& operator=(PMTilesReader&&) = delete;

  // Return a zero-copy view of the (possibly compressed) tile bytes,
  // pointing directly into the mmap'd region. Returns std::nullopt if
  // the tile does not exist in this file.
  std::optional<TileData> getTile(uint8_t z, uint32_t x, uint32_t y) const;

  // Return the tile bytes decompressed according to the tile compression of the
  // file. Returns std::nullopt if the tile does not exist in this file.
  std::optional<std::string> getDecompressedTile(uint8_t z, uint32_t x, uint32_t y) const;

  // Decompress tile bytes obtained from getTile()
  std::string decompressTile(const TileData& tile) const;

  // The JSON metadata section (decompressed), empty if there is none
  std::string metadata() const;

  const PMTilesHeader& header() const { return itsHeader; }

  // Unique id of this reader instance, used in cache keys. A file replaced on
  // disk is opened by a new reader with a new id.
  std::uint64_t id() const { return itsId; }

  // Convert tile coordinates to the PMTiles Hilbert-curve tile_id
  static uint64_t tileToId(uint8_t z, uint32_t x, uint32_t y);

  // Modification time of the source file (seconds since epoch)
  std::int64_t fileTimestamp() const;

  const std::filesystem::path& path() const { return itsPath; }

 private:
  // Hilbert-curve d = xy2d(n, x, y)
  static uint64_t xy2d(uint32_t n, uint32_t x, uint32_t y);

  // Decompress a region of the file into a byte vector
  std::vector<std::byte> decompress(uint64_t offset,
                                    uint64_t length,
                                    Compression compression) const;

  // Decode a columnar-encoded directory from raw bytes
  static Directory decodeDirectory(const std::vector<std::byte>& data);

  // Varint decode helper (protobuf-style, little-endian)
  static uint64_t readVarint(const std::byte*& p, const std::byte* end);

  // Fetch (and cache) the leaf directory at the given offset within the
  // leaf-dirs section. Thread-safe. The returned pointer stays valid even if
  // the directory is evicted from the cache meanwhile.
  DirectoryPtr getLeafDirectory(uint64_t offset, uint32_t length) const;

  // Binary search for tileId in a sorted directory
  static const Entry* findEntry(const Directory& dir, uint64_t tileId);

  // ---- file mapping ----
  std::filesystem::path itsPath;
  int itsFd = -1;
  const std::byte* itsData = nullptr;  // base of mmap'd region
  std::size_t itsFileSize = 0;

  // ---- parsed state ----
  PMTilesHeader itsHeader{};
  Directory itsRootDirectory;  // always resident (small, max ~512 KB)

  std::uint64_t itsId = 0;

  // ---- leaf directory cache, keyed by the offset in the leaf-dirs section ----
  mutable Fmi::Cache::Cache<uint64_t, DirectoryPtr> itsLeafCache;
};

// ======================================================================

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
