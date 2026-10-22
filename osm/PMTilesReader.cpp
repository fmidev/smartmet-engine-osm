// ======================================================================
/*!
 * \brief PMTiles v3 reader implementation
 */
// ======================================================================

#include "PMTilesReader.h"
#include <macgyver/Exception.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

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

// Hardening limits against hostile PMTiles files.
//
// Maximum number of entries we are willing to allocate for a single directory.
// PMTiles directories are split so that no single directory grows unbounded; a
// few hundred thousand entries is already extreme. This cap prevents a crafted
// varint entry-count from triggering a huge std::vector allocation (DoS) before
// the (much smaller) input buffer is even consulted.
constexpr uint64_t kMaxDirectoryEntries = 10'000'000;

// Maximum depth of leaf-directory pointer chasing. The PMTiles spec uses at
// most a root directory plus one or two levels of leaf directories; a small
// bound stops a self-referential / cyclic leaf pointer from causing an
// unbounded loop.
constexpr int kMaxLeafDepth = 4;

// Overflow-safe range check: is the half-open range [offset, offset+length)
// fully contained in a buffer of the given size, without the offset+length
// addition wrapping around? Written so that no intermediate value can overflow.
inline bool rangeWithin(uint64_t offset, uint64_t length, uint64_t size)
{
  return offset <= size && length <= size - offset;
}

// True if the file is on a network file system, where a file replaced on the
// server becomes unreadable for the clients which have it open
bool on_network_fs(int fd)
{
  struct statfs fs{};
  if (::fstatfs(fd, &fs) != 0)
    return true;  // be safe

  switch (static_cast<unsigned long>(fs.f_type))
  {
    case NFS_SUPER_MAGIC:
    case SMB_SUPER_MAGIC:
    case 0xFF534D42UL:  // CIFS
    case 0xFE534D42UL:  // SMB2
    case 0x65735546UL:  // FUSE (sshfs, s3fs, ...)
    case 0x00C36400UL:  // Ceph
    case 0x0BD00BD0UL:  // Lustre
    case 0x47504653UL:  // GPFS
    case 0x6B414653UL:  // AFS
    case 0x013111A8UL:  // IBRIX
    case 0x5346414FUL:  // OpenAFS
      return true;
    default:
      return false;
  }
}

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

}  // anonymous namespace

// ======================================================================
//  Construction
// ======================================================================

FileAccess parseFileAccess(const std::string& theName)
{
  if (theName == "auto")
    return FileAccess::Auto;
  if (theName == "mmap")
    return FileAccess::Mmap;
  if (theName == "pread")
    return FileAccess::Pread;
  throw Fmi::Exception(BCP, "Unknown file access mode '" + theName + "'")
      .addParameter("allowed", "auto, mmap, pread");
}

PMTilesReader::PMTilesReader(const std::filesystem::path& path,
                             std::size_t leafCacheSize,
                             FileAccess access)
    : itsPath(path), itsLeafCache(std::max<std::size_t>(leafCacheSize, 16))
{
  try
  {
    itsFd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (itsFd < 0)
      throw Fmi::Exception(BCP, "Cannot open PMTiles file: " + path.string());

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

    if (access == FileAccess::Mmap || (access == FileAccess::Auto && !on_network_fs(itsFd)))
    {
      // MAP_SHARED lets the kernel manage pages via its page cache
      void* addr = ::mmap(nullptr, itsFileSize, PROT_READ, MAP_SHARED, itsFd, 0);
      if (addr == MAP_FAILED)
      {
        ::close(itsFd);
        throw Fmi::Exception(BCP, "mmap failed: " + path.string());
      }
      itsMapped = reinterpret_cast<const std::byte*>(addr);

      // Random access pattern, no read-ahead
      ::madvise(const_cast<std::byte*>(itsMapped), itsFileSize, MADV_RANDOM);
      // Exclude from core dumps to reduce dump sizes
      ::madvise(const_cast<std::byte*>(itsMapped), itsFileSize, MADV_DONTDUMP);
    }

    // Parse the 127-byte header
    const auto region = read(0, kHeaderSize);
    const std::byte* h = region.data();

    if (std::memcmp(h, kMagic.data(), 7) != 0)
      throw Fmi::Exception(BCP, "Not a PMTiles file: " + path.string());

    uint8_t version = static_cast<uint8_t>(h[7]);
    if (version != kSpecVersion)
      throw Fmi::Exception(BCP,
                           "Unsupported PMTiles version " + std::to_string(version) +
                               " in: " + path.string());

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
    itsHeader.minLon = readI32LE(h + 102) / 1e7F;
    itsHeader.minLat = readI32LE(h + 106) / 1e7F;
    itsHeader.maxLon = readI32LE(h + 110) / 1e7F;
    itsHeader.maxLat = readI32LE(h + 114) / 1e7F;
    itsHeader.centerZoom = static_cast<uint8_t>(h[118]);
    itsHeader.centerLon = readI32LE(h + 119) / 1e7F;
    itsHeader.centerLat = readI32LE(h + 123) / 1e7F;

    // Validate every file-derived section offset/length against the actual
    // file size, using overflow-safe arithmetic. This guarantees that
    // any offset later derived relative to these sections (leaf dirs, tile
    // data) cannot overflow when added to the section base.
    if (!rangeWithin(itsHeader.rootDirOffset, itsHeader.rootDirLength, itsFileSize) ||
        !rangeWithin(itsHeader.metadataOffset, itsHeader.metadataLength, itsFileSize) ||
        !rangeWithin(itsHeader.leafDirsOffset, itsHeader.leafDirsLength, itsFileSize) ||
        !rangeWithin(itsHeader.tileDataOffset, itsHeader.tileDataLength, itsFileSize))
      throw Fmi::Exception(BCP, "PMTiles header section out of file bounds: " + path.string());

    // Decompress and decode root directory into RAM (always kept resident)
    auto rootBytes = decompress(
        itsHeader.rootDirOffset, itsHeader.rootDirLength, itsHeader.internalCompression);
    itsRootDirectory = decodeDirectory(rootBytes);

    SourceHeader summary;
    summary.tileType = itsHeader.tileType;
    summary.tileCompression = itsHeader.tileCompression;
    summary.minZoom = itsHeader.minZoom;
    summary.maxZoom = itsHeader.maxZoom;
    summary.west = itsHeader.minLon;
    summary.south = itsHeader.minLat;
    summary.east = itsHeader.maxLon;
    summary.north = itsHeader.maxLat;
    setSummary(summary);
  }
  catch (...)
  {
    if (itsMapped)
      ::munmap(const_cast<std::byte*>(itsMapped), itsFileSize);
    if (itsFd >= 0)
      ::close(itsFd);
    throw Fmi::Exception::Trace(BCP, "PMTilesReader constructor failed");
  }
}

PMTilesReader::~PMTilesReader()
{
  if (itsMapped)
    ::munmap(const_cast<std::byte*>(itsMapped), itsFileSize);
  if (itsFd >= 0)
    ::close(itsFd);
}

// ======================================================================
//  File access
// ======================================================================

PMTilesReader::Region PMTilesReader::read(uint64_t offset, uint64_t length) const
{
  // Overflow-safe bounds check: a naive "offset + length > itsFileSize" can be
  // bypassed by a crafted length that makes the addition wrap around.
  if (!rangeWithin(offset, length, itsFileSize))
    throw Fmi::Exception(BCP, "PMTiles: region extends past end of file");

  Region region;
  region.itsSize = length;
  if (itsMapped)
  {
    region.itsView = itsMapped + offset;
    return region;
  }

  region.itsBuffer.resize(length);
  std::size_t done = 0;
  while (done < length)
  {
    const auto n = ::pread(itsFd,
                           region.itsBuffer.data() + done,
                           length - done,
                           static_cast<off_t>(offset + done));
    if (n > 0)
    {
      done += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;

    // A replaced file on NFS gives ESTALE, a truncated one a short read
    const int err = (n < 0 ? errno : 0);
    itsFailed = true;
    throw Fmi::Exception(BCP, "PMTiles: read failed")
        .addParameter("file", itsPath.string())
        .addParameter("error", err != 0 ? std::strerror(err) : "unexpected end of file");
  }
  return region;
}

// ----------------------------------------------------------------------

std::vector<std::byte> PMTilesReader::decompress(uint64_t offset,
                                                 uint64_t length,
                                                 Compression compression) const
{
  const auto region = read(offset, length);
  return OSM::decompress(region.data(), region.size(), compression);
}

// ======================================================================
//  Tile lookup
// ======================================================================

std::optional<Entry> PMTilesReader::findTile(uint8_t z, uint32_t x, uint32_t y) const
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

    // If run_length == 0, this entry is a pointer into the leaf-dirs section.
    // Bound the number of leaf levels we follow so that a self-referential or
    // cyclic leaf pointer in a hostile file cannot spin forever.
    int depth = 0;
    DirectoryPtr leaf;  // keeps the directory e points into alive
    while (e->runLength == 0)
    {
      if (++depth > kMaxLeafDepth)
        throw Fmi::Exception(BCP, "PMTiles: leaf directory recursion limit exceeded");
      leaf = getLeafDirectory(e->offset, e->length);
      e = findEntry(*leaf, tileId);
      if (!e)
        return {};
    }

    // Verify the tile falls within this run
    if (tileId >= e->tileId + e->runLength)
      return {};

    // Validate the tile range against the tile-data section extent with
    // overflow-safe arithmetic, so a crafted directory offset/length cannot
    // produce an out-of-bounds read.
    if (!rangeWithin(e->offset, e->length, itsHeader.tileDataLength))
      throw Fmi::Exception(BCP, "PMTiles: tile data region out of bounds");

    return *e;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::findTile failed");
  }
}

// ----------------------------------------------------------------------

std::optional<std::string> PMTilesReader::getTile(uint8_t z, uint32_t x, uint32_t y) const
{
  auto entry = findTile(z, x, y);
  if (!entry)
    return {};
  return readTileData(*entry);
}

// ----------------------------------------------------------------------

std::string PMTilesReader::readTileData(const Entry& theEntry) const
{
  if (!rangeWithin(theEntry.offset, theEntry.length, itsHeader.tileDataLength))
    throw Fmi::Exception(BCP, "PMTiles: tile data region out of bounds");
  const auto region = read(itsHeader.tileDataOffset + theEntry.offset, theEntry.length);
  return {reinterpret_cast<const char*>(region.data()), region.size()};
}

// ----------------------------------------------------------------------

std::optional<std::string> PMTilesReader::getDecompressedTile(uint8_t z,
                                                              uint32_t x,
                                                              uint32_t y) const
{
  try
  {
    auto entry = findTile(z, x, y);
    if (!entry)
      return {};
    // All tiles in a run share the same data (deduplication)
    const auto region = read(itsHeader.tileDataOffset + entry->offset, entry->length);
    return decompressToString(region.data(), region.size(), itsHeader.tileCompression);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::getDecompressedTile failed")
        .addParameter("file", itsPath.string());
  }
}

// ----------------------------------------------------------------------

void PMTilesReader::forEachEntry(const std::function<void(const Entry&)>& theCallback) const
{
  try
  {
    visit(itsRootDirectory, 0, theCallback);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::forEachEntry failed")
        .addParameter("file", itsPath.string());
  }
}

void PMTilesReader::visit(const Directory& theDirectory,
                          int theDepth,
                          const std::function<void(const Entry&)>& theCallback) const
{
  if (theDepth > kMaxLeafDepth)
    throw Fmi::Exception(BCP, "PMTiles: leaf directory recursion limit exceeded");

  for (const auto& e : theDirectory)
  {
    if (e.runLength > 0)
      theCallback(e);
    else
      visit(*getLeafDirectory(e.offset, e.length), theDepth + 1, theCallback);
  }
}

// ----------------------------------------------------------------------

std::string PMTilesReader::metadata() const
{
  try
  {
    if (itsHeader.metadataLength == 0)
      return {};
    auto bytes = decompress(
        itsHeader.metadataOffset, itsHeader.metadataLength, itsHeader.internalCompression);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "PMTilesReader::metadata failed")
        .addParameter("file", itsPath.string());
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

    // Guard against a hostile entry count triggering a huge allocation before
    // the (much smaller) input buffer is even consulted. Two independent caps:
    //  1) an absolute sanity ceiling, and
    //  2) the physical limit that each entry needs at least one varint byte in
    //     the first column, so numEntries can never exceed the bytes remaining.
    if (numEntries > kMaxDirectoryEntries)
      throw Fmi::Exception(BCP, "PMTiles: directory entry count exceeds limit")
          .addParameter("num_entries", std::to_string(numEntries));
    if (numEntries > static_cast<uint64_t>(end - p))
      throw Fmi::Exception(BCP, "PMTiles: directory entry count exceeds available data");

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
    result |= static_cast<uint64_t>(byte & 0x7FU) << shift;
    if (!(byte & 0x80U))
      return result;
    shift += 7;
    if (shift >= 64)
      throw Fmi::Exception(BCP, "PMTiles: varint overflow");
  }
  throw Fmi::Exception(BCP, "PMTiles: truncated varint");
}

// ======================================================================
//  Leaf directory cache
// ======================================================================

DirectoryPtr PMTilesReader::getLeafDirectory(uint64_t offset, uint32_t length) const
{
  try
  {
    if (auto cached = itsLeafCache.find(offset))
      return *cached;

    // Validate the leaf-directory region against the leaf-dirs section extent
    // using overflow-safe arithmetic BEFORE computing the absolute file offset.
    // Since the leaf-dirs section was itself bounds-checked at construction,
    // this guarantees leafDirsOffset + offset cannot overflow and the resulting
    // absolute region stays inside the mapped file.
    if (!rangeWithin(offset, length, itsHeader.leafDirsLength))
      throw Fmi::Exception(BCP, "PMTiles: leaf directory region out of bounds");

    // Not in cache. Two threads may decode the same directory simultaneously,
    // which is harmless.
    auto leafBytes =
        decompress(itsHeader.leafDirsOffset + offset, length, itsHeader.internalCompression);
    auto leafDir = std::make_shared<const Directory>(decodeDirectory(leafBytes));
    itsLeafCache.insert(offset, leafDir);
    return leafDir;
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
  // For a leaf pointer (runLength == 0): the leaf covers all ids from its first
  // tile id up to the first id of the next entry, which upper_bound guarantees.
  const Entry& e = *it;
  if (e.runLength == 0)
    return &e;

  return (tileId < e.tileId + e.runLength) ? &e : nullptr;
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
