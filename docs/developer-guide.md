# OSM engine developer guide

This guide is for developers who change `smartmet-engine-osm`, or use it from a plugin.
The engine serves vector tiles (Mapbox Vector Tiles) of OpenStreetMap data from
[PMTiles v3](https://github.com/protomaps/PMTiles) archives, without copying: a tile is
returned as a pointer into the memory-mapped file. The WMS/Dali plugin's `OSMLayer` uses
it to draw OSM map layers.

[CLAUDE.md](../CLAUDE.md) has the architecture summary.

## Contents

1. [Building and testing](#1-building-and-testing)
2. [The API](#2-the-api)
3. [How a tile is found](#3-how-a-tile-is-found)
4. [Configuration](#4-configuration)
5. [Known pitfalls](#5-known-pitfalls)

---

## 1. Building and testing

```bash
make
make test     # test/PMTilesReaderTest.cpp against the archives in test/cnf/osm.conf
```

## 2. The API

| Call | Returns |
|------|---------|
| `getTile(source, z, x, y)` | `std::optional<TileData>`: a pointer and size of the tile's bytes inside the mapped file, or `nullopt` if the source is unknown or the tile does not exist. |
| `getHeader(source)` | The PMTiles header: zoom range, bounds, tile type, tile compression, … |
| `getDataTimestamp(source)` | The modification time of the **mapped** file (`fstat` on the open descriptor, not the path), for ETags; it stays consistent with the data actually served. |
| `getSources()` | The configured source names. |

The tile bytes are returned **as stored**: still compressed according to the header's
`tileCompression` (typically gzip). Decompressing them is the caller's job.

## 3. How a tile is found

Each source has one `PMTilesReader`, which maps the whole file read-only
(`mmap(MAP_SHARED, PROT_READ)`) at `init()`. A lookup:

1. converts `(z, x, y)` to the PMTiles tile id (position on the Hilbert curve of level `z`);
2. binary-searches the root directory; if the entry points to a leaf directory, fetches it
   from the leaf cache or decompresses it (gzip or zstd) from the file and caches it
   (`leaf_cache_size` entries, guarded by a `shared_mutex`);
3. follows run-length entries to the tile's offset and length, and returns a pointer into
   the mapping.

Brotli-compressed directories are not supported.

## 4. Configuration

```
leaf_cache_size = 1024;
sources:
{
  scandinavia: { file = "/var/smartmet/osm/scandinavia.pmtiles"; };
  global:      { file = "/var/smartmet/osm/global.pmtiles"; };
};
```

## 5. Known pitfalls

* **Returned pointers live as long as the engine.** Do not keep `TileData` beyond the
  request, and do not use it after shutdown.
* **Archives are opened once.** The files are mapped at start and never reopened: a new
  archive is used only after a restart. Replace an archive by writing a new file and
  renaming it over the old one; the running server keeps the old (still mapped) file.
  Truncating or rewriting a mapped file in place makes the next access to the lost pages
  crash the process with `SIGBUS`.
* **Tiles come compressed** (§2).
