// ======================================================================
/*!
 * \brief PMTiles v3 memory-mapped reader implementation
 */
// ======================================================================

#include "PMTilesReader.h"
#include <macgyver/Exception.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

// ======================================================================
//  Header layout constants
// ======================================================================

namespace
{
constexpr std::size_t kHeaderSize = 127;
constexpr std::array<char, 7> kMagic = {'P', 'M', 'T', 'i', 'l', 'e', 's'};
constexpr uint8_t kSpecVersion = 3;

// Read a little-endian uint64 from a byte pointer
inline uint64_t readU64LE(const std::byte* p)
{
  uint64_t v = 0;
  std::memcpy(&v, p, 8);
  // The spec mandates little-endian; Linux/x86 is LE so this is a no-op
  return v;
}

inline int32_t readI32LE(const std::byte* p)
{
  int32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}

// Decompress with zlib (gzip stream)
std::vector<std::byte> decompressGzip(const std::byte* src, std::size_t srcLen)
{
  // Estimate: directories are rarely > 16 MB
  std::vector<std::byte> dst(srcLen * 4);
  for (int attempt = 0; attempt < 4; ++attempt)
  {
    z_stream zs{};
    // inflateInit2 with 32 + MAX_WBITS → auto-detect gzip/zlib header
    if (inflateInit2(&zs, 32 + MAX_WBITS) != Z_OK)
      throw Fmi::Exception(BCP, "zlib inflateInit2 failed");

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(src));
    zs.avail_in = static_cast<uInt>(srcLen);
    zs.next_out = reinterpret_cast<Bytef*>(dst.data());
    zs.avail_out = static_cast<uInt>(dst.size());

    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);

    if (ret == Z_STREAM_END)
    {
      dst.resize(zs.total_out);
      return dst;
    }
    if (ret == Z_BUF_ERROR || zs.avail_out == 0)
    {
      dst.resize(dst.size() * 4);  // grow and retry
      continue;
    }
    throw Fmi::Exception(BCP, "zlib inflate failed").addParameter("zlib_ret", std::to_string(ret));
  }
  throw Fmi::Exception(BCP, "PMTiles gzip decompress: output buffer exhausted");
}

// Decompress with zstd
std::vector<std::byte> decompressZstd(const std::byte* src, std::size_t srcLen)
{
  unsigned long long const frameSize =
      ZSTD_getFrameContentSize(reinterpret_cast<const void*>(src), srcLen);

  if (frameSize == ZSTD_CONTENTSIZE_ERROR)
    throw Fmi::Exception(BCP, "PMTiles zstd: not a valid zstd frame");

  std::size_t dstLen = (frameSize == ZSTD_CONTENTSIZE_UNKNOWN)
                           ? std::max(srcLen * 4, std::size_t{65536})
                           : static_cast<std::size_t>(frameSize);

  std::vector<std::byte> dst(dstLen);

  if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN)
  {
    // Known size — decompress directly
    std::size_t n = ZSTD_decompress(dst.data(), dstLen, src, srcLen);
    if (ZSTD_isError(n))
      throw Fmi::Exception(BCP, "PMTiles zstd decompress failed")
          .addParameter("zstd_error", ZSTD_getErrorName(n));
    dst.resize(n);
    return dst;
  }

  // Unknown decompressed size — use streaming decompression
  ZSTD_DStream* dstream = ZSTD_createDStream();
  if (!dstream)
    throw Fmi::Exception(BCP, "ZSTD_createDStream failed");

  ZSTD_initDStream(dstream);
  ZSTD_inBuffer in{reinterpret_cast<const void*>(src), srcLen, 0};
  std::size_t outPos = 0;

  while (in.pos < in.size)
  {
    if (outPos >= dst.size())
      dst.resize(dst.size() * 2);

    ZSTD_outBuffer out{dst.data() + outPos, dst.size() - outPos, 0};
    std::size_t ret = ZSTD_decompressStream(dstream, &out, &in);
    if (ZSTD_isError(ret))
    {
      ZSTD_freeDStream(dstream);
      throw Fmi::Exception(BCP, "PMTiles zstd stream decompress failed")
          .addParameter("zstd_error", ZSTD_getErrorName(ret));
    }
    outPos += out.pos;
  }

  ZSTD_freeDStream(dstream);
  dst.resize(outPos);
  return dst;
}

}  // anonymous namespace

// ======================================================================
//  Construction / mmap
// ======================================================================

PMTilesReader::PMTilesReader(const std::filesystem::path& path) : itsPath(path)
{
  try
  {
    // Open the file
    itsFd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (itsFd < 0)
      throw Fmi::Exception(BCP, "Cannot open PMTiles file: " + path.string());

    // Determine file size
    struct stat st{};
    if (::fstat(itsFd, &st) != 0)
    {
      ::close(itsFd);
      throw Fmi::Exception(BCP, "fstat failed: " + path.string());
    }
    itsFileSize = static_cast<std::size_t>(st.st_size);

    if (itsFileSize < kHeaderSize)
    {
      ::close(itsFd);
      throw Fmi::Exception(BCP, "PMTiles file too small: " + path.string());
    }

    // Memory-map the whole file read-only. MAP_SHARED lets the kernel
    // manage pages via its page cache. madvise(MADV_RANDOM) tells it
    // not to do sequential read-ahead since tile access is random.
    void* addr =
        ::mmap(nullptr, itsFileSize, PROT_READ, MAP_SHARED, itsFd, 0);
    if (addr == MAP_FAILED)
    {
      ::close(itsFd);
      throw Fmi::Exception(BCP, "mmap failed: " + path.string());
    }
    itsData = reinterpret_cast<const std::byte*>(addr);

    // Hint: random access pattern, no prefetch
    ::madvise(const_cast<std::byte*>(itsData), itsFileSize, MADV_RANDOM);
    // Exclude from core dumps to reduce dump sizes
    ::madvise(const_cast<std::byte*>(itsData), itsFileSize, MADV_DONTDUMP);

    // Parse the 127-byte header
    const std::byte* h = itsData;

    // Verify magic
    if (std::memcmp(h, kMagic.data(), 7) != 0)
    {
      ::munmap(const_cast<std::byte*>(itsData), itsFileSize);
      ::close(itsFd);
      throw Fmi::Exception(BCP, "Not a PMTiles file: " + path.string());
    }

    // Verify spec version
    uint8_t version = static_cast<uint8_t>(h[7]);
    if (version != kSpecVersion)
    {
      ::munmap(const_cast<std::byte*>(itsData), itsFileSize);
      ::close(itsFd);
      throw Fmi::Exception(BCP, "Unsupported PMTiles version " + std::to_string(version) +
                                     " in: " + path.string());
    }

    itsHeader.rootDirOffset = readU64LE(h + 8);
    itsHeader.rootDirLength = readU64LE(h + 16);
    itsHeader.metadataOffset = readU64LE(h + 24);
    itsHeader.metadataLength = readU64LE(h + 32);
    itsHeader.leafDirsOffset = readU64LE(h + 40);
    itsHeader.leafDirsLength = readU64LE(h + 48);
    itsHeader.tileDataOffset = readU64LE(h + 56);
    itsHeader.tileDataLength = readU64LE(h + 64);
    itsHeader.numAddressedTiles = readU64LE(h + 72);
    itsHeader.numTileEntries = readU64LE(h + 80);
    itsHeader.numTileContents = readU64LE(h + 88);
    itsHeader.clustered = static_cast<bool>(h[96]);
    itsHeader.internalCompression = static_cast<Compression>(h[97]);
    itsHeader.tileCompression = static_cast<Compression>(h[98]);
    itsHeader.tileType = static_cast<TileType>(h[99]);
    itsHeader.minZoom = static_cast<uint8_t>(h[100]);
    itsHeader.maxZoom = static_cast<uint8_t>(h[101]);
    itsHeader.minLon = readI32LE(h + 102) / 1e7f;
    itsHeader.minLat = readI32LE(h + 106) / 1e7f;
    itsHeader.maxLon = readI32LE(h + 110) / 1e7f;
    itsHeader.maxLat = readI32LE(h + 114) / 1e7f;
    itsHeader.centerZoom = static_cast<uint8_t>(h[118]);
    itsHeader.centerLon = readI32LE(h + 119) / 1e7f;
    itsHeader.centerLat = readI32LE(h + 123) / 1e7f;

    // Decompress and decode root directory into RAM (always kept resident)
    auto rootBytes = decompress(
        itsHeader.rootDirOffset, itsHeader.rootDirLength, itsHeader.internalCompression);
    itsRootDirectory = decodeDirectory(rootBytes);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader constructor failed");
  }
}

PMTilesReader::~PMTilesReader()
{
  if (itsData)
    ::munmap(const_cast<std::byte*>(itsData), itsFileSize);
  if (itsFd >= 0)
    ::close(itsFd);
}

// ======================================================================
//  Tile lookup
// ======================================================================

std::optional<TileData> PMTilesReader::getTile(
    uint8_t z, uint32_t x, uint32_t y) const
{
  try
  {
    if (z < itsHeader.minZoom || z > itsHeader.maxZoom)
      return {};

    const uint64_t tileId = tileToId(z, x, y);

    // Search the root directory first
    const Entry* e = findEntry(itsRootDirectory, tileId);
    if (!e)
      return {};

    // If run_length == 0, this entry is a pointer into the leaf-dirs section
    while (e->runLength == 0)
    {
      const Directory& leaf = getLeafDirectory(e->offset, e->length);
      e = findEntry(leaf, tileId);
      if (!e)
        return {};
    }

    // Verify the tile falls within this run
    if (tileId >= e->tileId + e->runLength)
      return {};

    // All tiles in a run share the same data (deduplication)
    const std::byte* tileBase =
        itsData + itsHeader.tileDataOffset + e->offset;
    return TileData{tileBase, e->length};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::getTile failed");
  }
}

// ======================================================================
//  File modification time
// ======================================================================

std::int64_t PMTilesReader::fileTimestamp() const
{
  struct stat st{};
  if (::fstat(itsFd, &st) != 0)
    return 0;
  return static_cast<std::int64_t>(st.st_mtime);
}

// ======================================================================
//  Hilbert curve: xy → 1D index
// ======================================================================

uint64_t PMTilesReader::xy2d(uint32_t n, uint32_t x, uint32_t y)
{
  uint64_t d = 0;
  for (uint32_t s = n / 2; s > 0; s /= 2)
  {
    uint32_t rx = (x & s) > 0 ? 1u : 0u;
    uint32_t ry = (y & s) > 0 ? 1u : 0u;
    d += static_cast<uint64_t>(s) * s * ((3u * rx) ^ ry);
    // Rotate the quadrant
    if (ry == 0)
    {
      if (rx == 1)
      {
        x = s - 1 - x;
        y = s - 1 - y;
      }
      std::swap(x, y);
    }
  }
  return d;
}

uint64_t PMTilesReader::tileToId(uint8_t z, uint32_t x, uint32_t y)
{
  // Accumulated tile count for all zoom levels below z:
  //   sum_{i=0}^{z-1} 4^i = (4^z - 1) / 3
  uint64_t acc = 0;
  uint32_t n = 1;
  for (uint8_t i = 0; i < z; ++i)
  {
    acc += static_cast<uint64_t>(n) * n;
    n <<= 1;  // n = 2^(i+1)
  }
  // n is now 2^z, the grid side length
  return acc + xy2d(n, x, y);
}

// ======================================================================
//  Decompress a region of the mmap'd file
// ======================================================================

std::vector<std::byte> PMTilesReader::decompress(uint64_t offset,
                                                  uint64_t length,
                                                  Compression compression) const
{
  if (offset + length > itsFileSize)
    throw Fmi::Exception(BCP, "PMTiles: compressed region extends past end of file");

  const std::byte* src = itsData + offset;

  switch (compression)
  {
    case Compression::None:
    case Compression::Unknown:
      return {src, src + length};

    case Compression::Gzip:
      return decompressGzip(src, length);

    case Compression::Zstd:
      return decompressZstd(src, length);

    case Compression::Brotli:
      throw Fmi::Exception(BCP, "PMTiles: brotli compression not supported");
  }
  throw Fmi::Exception(BCP, "PMTiles: unknown compression type");
}

// ======================================================================
//  Decode a directory from decompressed bytes
//
//  PMTiles v3 columnar directory format:
//    [N varints: tile_id deltas]
//    [N varints: run_lengths]
//    [N varints: data lengths]
//    [N varints: data offsets (0 means clustered continuation)]
//  where N is itself a varint at the start.
// ======================================================================

Directory PMTilesReader::decodeDirectory(const std::vector<std::byte>& data)
{
  try
  {
    const std::byte* p = data.data();
    const std::byte* end = p + data.size();

    const uint64_t numEntries = readVarint(p, end);
    Directory dir;
    dir.resize(numEntries);

    // Column 1: tile_id (delta-encoded)
    uint64_t lastId = 0;
    for (uint64_t i = 0; i < numEntries; ++i)
    {
      lastId += readVarint(p, end);
      dir[i].tileId = lastId;
    }

    // Column 2: run_length
    for (uint64_t i = 0; i < numEntries; ++i)
      dir[i].runLength = static_cast<uint32_t>(readVarint(p, end));

    // Column 3: data length
    for (uint64_t i = 0; i < numEntries; ++i)
      dir[i].length = static_cast<uint32_t>(readVarint(p, end));

    // Column 4: data offset
    // Encoding: 0 → clustered continuation (offset = prev.offset + prev.length)
    //           v → actual offset = v - 1
    for (uint64_t i = 0; i < numEntries; ++i)
    {
      uint64_t v = readVarint(p, end);
      if (v == 0 && i > 0)
        dir[i].offset = dir[i - 1].offset + dir[i - 1].length;
      else
        dir[i].offset = v - 1;
    }

    return dir;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::decodeDirectory failed");
  }
}

// ======================================================================
//  Varint decode (protobuf-style, 7 bits per byte, LSB first)
// ======================================================================

uint64_t PMTilesReader::readVarint(const std::byte*& p, const std::byte* end)
{
  uint64_t result = 0;
  int shift = 0;
  while (p < end)
  {
    uint8_t byte = static_cast<uint8_t>(*p++);
    result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
    if (!(byte & 0x80u))
      return result;
    shift += 7;
    if (shift >= 64)
      throw Fmi::Exception(BCP, "PMTiles: varint overflow");
  }
  throw Fmi::Exception(BCP, "PMTiles: truncated varint");
}

// ======================================================================
//  Leaf directory cache — thread-safe shared_mutex
// ======================================================================

const Directory& PMTilesReader::getLeafDirectory(uint64_t offset, uint32_t length) const
{
  try
  {
    // Check cache with shared (read) lock first
    {
      std::shared_lock lock(itsLeafMutex);
      auto it = itsLeafCache.find(offset);
      if (it != itsLeafCache.end())
        return it->second;
    }

    // Not in cache — decompress and insert with exclusive lock
    auto leafBytes =
        decompress(itsHeader.leafDirsOffset + offset, length, itsHeader.internalCompression);
    Directory leafDir = decodeDirectory(leafBytes);

    std::unique_lock lock(itsLeafMutex);
    // Double-check after acquiring exclusive lock
    auto [it, inserted] = itsLeafCache.emplace(offset, std::move(leafDir));
    return it->second;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::getLeafDirectory failed");
  }
}

// ======================================================================
//  Binary search for tileId in a sorted directory
// ======================================================================

const Entry* PMTilesReader::findEntry(const Directory& dir, uint64_t tileId)
{
  if (dir.empty())
    return nullptr;

  // Upper-bound search: find first entry with tileId > target, then look
  // one back.
  auto it = std::upper_bound(dir.begin(), dir.end(), tileId,
                             [](uint64_t id, const Entry& e) { return id < e.tileId; });
  if (it == dir.begin())
    return nullptr;

  --it;

  // For a run (runLength > 0): tile must fall within [tileId, tileId+runLength)
  // For a leaf pointer (runLength == 0): check if tileId matches exactly
  const Entry& e = *it;
  if (e.runLength == 0)
    return (e.tileId == tileId) ? &e : nullptr;

  return (tileId < e.tileId + e.runLength) ? &e : nullptr;
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
