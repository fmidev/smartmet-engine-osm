// ======================================================================
/*!
 * \brief Regression tests for PMTilesReader bounds / overflow hardening
 *
 * These tests construct minimal (and deliberately malformed) PMTiles v3
 * files on disk and verify that the reader parses valid files correctly
 * and rejects hostile ones by throwing rather than reading out of bounds
 * or looping forever.
 *
 * All directories use internalCompression = None so that the on-disk
 * directory bytes are the raw columnar varint encoding (no gzip/zstd
 * needed to build a fixture).
 */
// ======================================================================

#include "PMTilesReader.h"
#define BOOST_TEST_MODULE PMTilesReaderTest
#include <boost/test/included/unit_test.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace SmartMet::Engine::OSM;

namespace
{
// ---- little-endian / varint writers -------------------------------------

void putU64(std::vector<uint8_t>& b, std::size_t pos, uint64_t v)
{
  for (int i = 0; i < 8; ++i)
    b[pos + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

void appendVarint(std::vector<uint8_t>& b, uint64_t v)
{
  while (v >= 0x80)
  {
    b.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  b.push_back(static_cast<uint8_t>(v));
}

// Encode a single-entry columnar directory.
//   tileId (delta from 0), runLength, length, offset (actual offset)
std::vector<uint8_t> makeDir(uint64_t tileId, uint32_t runLength, uint32_t length,
                             uint64_t offset)
{
  std::vector<uint8_t> d;
  appendVarint(d, 1);          // numEntries
  appendVarint(d, tileId);     // col1: tile_id delta
  appendVarint(d, runLength);  // col2: run_length
  appendVarint(d, length);     // col3: length
  appendVarint(d, offset + 1); // col4: offset encoding (v-1 = offset)
  return d;
}

// Build a 127-byte header with the given section offsets/lengths.
std::vector<uint8_t> makeHeader(uint64_t rootOff, uint64_t rootLen, uint64_t leafOff,
                                uint64_t leafLen, uint64_t tileOff, uint64_t tileLen,
                                uint8_t minZoom = 0, uint8_t maxZoom = 0)
{
  std::vector<uint8_t> h(127, 0);
  std::memcpy(h.data(), "PMTiles", 7);
  h[7] = 3;  // spec version
  putU64(h, 8, rootOff);
  putU64(h, 16, rootLen);
  putU64(h, 24, 0);  // metadataOffset
  putU64(h, 32, 0);  // metadataLength
  putU64(h, 40, leafOff);
  putU64(h, 48, leafLen);
  putU64(h, 56, tileOff);
  putU64(h, 64, tileLen);
  putU64(h, 72, 1);  // numAddressedTiles
  putU64(h, 80, 1);  // numTileEntries
  putU64(h, 88, 1);  // numTileContents
  h[96] = 0;         // clustered
  h[97] = 1;         // internalCompression = None
  h[98] = 1;         // tileCompression   = None
  h[99] = 1;         // tileType = MVT
  h[100] = minZoom;
  h[101] = maxZoom;
  return h;
}

std::filesystem::path writeTemp(const std::vector<uint8_t>& bytes, const std::string& tag)
{
  auto p = std::filesystem::temp_directory_path() /
           ("pmtiles_test_" + tag + "_" + std::to_string(::getpid()) + ".pmtiles");
  std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
  ofs.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  ofs.close();
  return p;
}

std::vector<uint8_t> concat(const std::vector<std::vector<uint8_t>>& parts)
{
  std::vector<uint8_t> out;
  for (const auto& p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}
}  // namespace

// ----------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(valid_file_parses_and_returns_tile)
{
  BOOST_TEST_MESSAGE("PMTilesReader: valid minimal file returns the tile");

  // Layout: header(127) + rootDir + tileData(4)
  auto root = makeDir(/*tileId*/ 0, /*runLength*/ 1, /*length*/ 4, /*offset*/ 0);
  const uint64_t rootOff = 127;
  const uint64_t rootLen = root.size();
  const uint64_t tileOff = rootOff + rootLen;
  const std::vector<uint8_t> tile = {'M', 'V', 'T', '!'};

  auto file =
      concat({makeHeader(rootOff, rootLen, tileOff, 0, tileOff, tile.size()), root, tile});
  auto path = writeTemp(file, "valid");

  PMTilesReader reader(path);
  auto td = reader.getTile(0, 0, 0);
  BOOST_REQUIRE(td.has_value());
  BOOST_CHECK_EQUAL(td->size, 4u);
  BOOST_CHECK_EQUAL(std::memcmp(td->data, tile.data(), 4), 0);

  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(overflowing_root_dir_length_is_rejected)
{
  BOOST_TEST_MESSAGE("PMTilesReader: overflow-safe header bounds (H-16/H-17)");

  auto root = makeDir(0, 1, 4, 0);
  const uint64_t rootOff = 127;
  // A length that, added to rootOff, wraps around uint64. The old
  // "offset + length > fileSize" check would be bypassed.
  const uint64_t evilLen = 0xFFFFFFFFFFFFFFF0ULL;
  auto file = concat({makeHeader(rootOff, evilLen, 127, 0, 127, 0), root});
  auto path = writeTemp(file, "ovf");

  BOOST_CHECK_THROW(PMTilesReader reader(path), std::exception);
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(out_of_bounds_tile_offset_is_rejected)
{
  BOOST_TEST_MESSAGE("PMTilesReader: out-of-range tile offset/length (H-16)");

  // Valid header/root, but the tile entry claims a length far beyond the
  // 4-byte tile-data section.
  auto root = makeDir(/*tileId*/ 0, /*runLength*/ 1, /*length*/ 1000000, /*offset*/ 0);
  const uint64_t rootOff = 127;
  const uint64_t rootLen = root.size();
  const uint64_t tileOff = rootOff + rootLen;
  const std::vector<uint8_t> tile = {'a', 'b', 'c', 'd'};

  auto file =
      concat({makeHeader(rootOff, rootLen, tileOff, 0, tileOff, tile.size()), root, tile});
  auto path = writeTemp(file, "oobtile");

  PMTilesReader reader(path);
  BOOST_CHECK_THROW(reader.getTile(0, 0, 0), std::exception);
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(huge_directory_entry_count_is_rejected)
{
  BOOST_TEST_MESSAGE("PMTilesReader: unbounded directory-entry allocation is capped");

  // Root directory whose entry count is absurd relative to the tiny buffer.
  std::vector<uint8_t> root;
  appendVarint(root, 0xFFFFFFFFULL);  // ~4.29 billion entries claimed
  // (no actual column data follows)

  const uint64_t rootOff = 127;
  const uint64_t rootLen = root.size();
  auto file = concat({makeHeader(rootOff, rootLen, 127, 0, 127, 0), root});
  auto path = writeTemp(file, "hugedir");

  // Rejected during construction (root directory is decoded eagerly) without
  // attempting the multi-gigabyte allocation.
  BOOST_CHECK_THROW(PMTilesReader reader(path), std::exception);
  std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(cyclic_leaf_directory_is_bounded)
{
  BOOST_TEST_MESSAGE("PMTilesReader: self-referential leaf directory is bounded");

  // Root entry is a leaf pointer (runLength == 0) to leaf-dir offset 0.
  // The leaf directory at that offset is itself a leaf pointer back to
  // offset 0, forming a cycle.
  auto leaf = makeDir(/*tileId*/ 0, /*runLength*/ 0, /*length*/ 0, /*offset*/ 0);
  // Fix up the leaf's declared "length" to its own size so it re-reads itself.
  leaf = makeDir(0, 0, static_cast<uint32_t>(leaf.size()), 0);
  auto root = makeDir(0, 0, static_cast<uint32_t>(leaf.size()), 0);

  const uint64_t rootOff = 127;
  const uint64_t rootLen = root.size();
  const uint64_t leafOff = rootOff + rootLen;
  const uint64_t leafLen = leaf.size();

  auto file = concat({makeHeader(rootOff, rootLen, leafOff, leafLen, leafOff + leafLen, 0),
                      root, leaf});
  auto path = writeTemp(file, "cyclic");

  PMTilesReader reader(path);
  // Must terminate (throw) rather than loop forever.
  BOOST_CHECK_THROW(reader.getTile(0, 0, 0), std::exception);
  std::filesystem::remove(path);
}
