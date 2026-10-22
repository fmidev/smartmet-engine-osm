// ======================================================================
/*!
 * \brief Tile source base class and decompression
 */
// ======================================================================

#include "TileSource.h"
#include <macgyver/Exception.h>
#include <algorithm>
#include <atomic>
#include <utility>
#include <zlib.h>
#include <zstd.h>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace
{
// Source of unique source ids
std::atomic<std::uint64_t> gSourceCounter{0};

// Decompress with zlib (gzip stream)
std::vector<std::byte> decompressGzip(const std::byte* src, std::size_t srcLen)
{
  // Estimate: directories are rarely > 16 MB
  std::vector<std::byte> dst(std::max<std::size_t>(srcLen * 4, 1024));
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

// Hilbert curve: xy → 1D index
uint64_t xy2d(uint32_t n, uint32_t x, uint32_t y)
{
  uint64_t d = 0;
  for (uint32_t s = n / 2; s > 0; s /= 2)
  {
    uint32_t rx = (x & s) > 0 ? 1U : 0U;
    uint32_t ry = (y & s) > 0 ? 1U : 0U;
    d += static_cast<uint64_t>(s) * s * ((3U * rx) ^ ry);
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

}  // namespace

// ----------------------------------------------------------------------

TileSource::TileSource() : itsId(++gSourceCounter) {}

TileSource::~TileSource() = default;

// ----------------------------------------------------------------------

std::vector<std::optional<std::string>> TileSource::getDecompressedTiles(
    const std::vector<TileCoord>& theTiles) const
{
  std::vector<std::optional<std::string>> result;
  result.reserve(theTiles.size());
  for (const auto& t : theTiles)
    result.push_back(getDecompressedTile(t.z, t.x, t.y));
  return result;
}

// ----------------------------------------------------------------------

uint64_t TileSource::tileToId(uint8_t z, uint32_t x, uint32_t y)
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

// ----------------------------------------------------------------------

std::vector<std::byte> decompress(const std::byte* theData,
                                  std::size_t theSize,
                                  Compression theCompression)
{
  switch (theCompression)
  {
    case Compression::None:
    case Compression::Unknown:
      return {theData, theData + theSize};
    case Compression::Gzip:
      return decompressGzip(theData, theSize);
    case Compression::Zstd:
      return decompressZstd(theData, theSize);
    case Compression::Brotli:
      throw Fmi::Exception(BCP, "PMTiles: brotli compression not supported");
  }
  throw Fmi::Exception(BCP, "PMTiles: unknown compression type");
}

std::string decompressToString(const std::byte* theData,
                               std::size_t theSize,
                               Compression theCompression)
{
  if (theCompression == Compression::None || theCompression == Compression::Unknown)
    return {reinterpret_cast<const char*>(theData), theSize};
  auto bytes = decompress(theData, theSize, theCompression);
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
