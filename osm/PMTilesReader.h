// ======================================================================
/*!
 * \brief PMTiles v3 reader
 *
 * Files on local disks are memory-mapped so that the kernel's page cache
 * manages the resident pages and reads need no system calls. Files on
 * network file systems are read with pread instead: a file replaced on an
 * NFS server disappears from under its readers (ESTALE), which would crash
 * a process using a mapping (SIGBUS), while pread just returns an error.
 * The reader then reports itself as failed and the tile store reopens the
 * file.
 *
 * Directory decompression (gzip / zstd) results are cached per file.
 *
 * Reference: https://github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
 */
// ======================================================================

#pragma once

#include "TileSource.h"
#include <macgyver/Cache.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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

// How files are read
enum class FileAccess
{
  Auto,  // mmap on local file systems, pread on network file systems
  Mmap,
  Pread
};

// Parse "auto", "mmap" or "pread"
FileAccess parseFileAccess(const std::string& theName);

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
//  PMTilesReader
//
//  One instance per .pmtiles file. Thread-safe for concurrent reads.
// ======================================================================

class PMTilesReader : public TileSource
{
 public:
  // The leaf cache size is the number of decoded leaf directories kept in memory.
  // A planet file has a few thousand leaves of roughly 1.5 MB each when decoded.
  explicit PMTilesReader(const std::filesystem::path& path,
                         std::size_t leafCacheSize = 256,
                         FileAccess access = FileAccess::Auto);
  ~PMTilesReader() override;

  PMTilesReader(const PMTilesReader&) = delete;
  PMTilesReader& operator=(const PMTilesReader&) = delete;
  PMTilesReader(PMTilesReader&&) = delete;
  PMTilesReader& operator=(PMTilesReader&&) = delete;

  // The tile bytes as stored (possibly compressed), std::nullopt if the tile
  // does not exist in this file
  std::optional<std::string> getTile(uint8_t z, uint32_t x, uint32_t y) const;

  // Return the tile bytes decompressed according to the tile compression of the
  // file. Returns std::nullopt if the tile does not exist in this file.
  std::optional<std::string> getDecompressedTile(uint8_t z,
                                                 uint32_t x,
                                                 uint32_t y) const override;

  // The JSON metadata section (decompressed), empty if there is none
  std::string metadata() const override;

  // True after a read error, the file must be reopened
  bool failed() const override { return itsFailed; }

  const PMTilesHeader& header() const { return itsHeader; }

  // True if the file is memory-mapped, false if it is read with pread
  bool mapped() const { return itsMapped != nullptr; }

  // Visit all tile entries (not leaf pointers) in tile id order
  void forEachEntry(const std::function<void(const Entry&)>& theCallback) const;

  // The raw bytes of a tile entry
  std::string readTileData(const Entry& theEntry) const;

  // Modification time of the source file (seconds since epoch)
  std::int64_t fileTimestamp() const;

  const std::filesystem::path& path() const { return itsPath; }

 private:
  // Bytes read from the file: a view into the mapping, or a pread buffer
  class Region
  {
   public:
    const std::byte* data() const { return itsBuffer.empty() ? itsView : itsBuffer.data(); }
    std::size_t size() const { return itsSize; }

   private:
    friend class PMTilesReader;
    const std::byte* itsView = nullptr;
    std::vector<std::byte> itsBuffer;
    std::size_t itsSize = 0;
  };

  // Read a region of the file, throws on I/O errors and marks the reader failed
  Region read(uint64_t offset, uint64_t length) const;

  // Read and decompress a region of the file
  std::vector<std::byte> decompress(uint64_t offset,
                                    uint64_t length,
                                    Compression compression) const;

  // The entry of the tile, nullptr if there is none
  std::optional<Entry> findTile(uint8_t z, uint32_t x, uint32_t y) const;

  // Visit the entries of a directory, following leaf pointers
  void visit(const Directory& theDirectory,
             int theDepth,
             const std::function<void(const Entry&)>& theCallback) const;

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

  // ---- file access ----
  std::filesystem::path itsPath;
  int itsFd = -1;
  const std::byte* itsMapped = nullptr;  // base of the mapping, nullptr when using pread
  std::size_t itsFileSize = 0;
  mutable std::atomic<bool> itsFailed{false};

  // ---- parsed state ----
  PMTilesHeader itsHeader{};
  Directory itsRootDirectory;  // always resident (small, max ~512 KB)

  // ---- leaf directory cache, keyed by the offset in the leaf-dirs section ----
  mutable Fmi::Cache::Cache<uint64_t, DirectoryPtr> itsLeafCache;
};

// ======================================================================

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
