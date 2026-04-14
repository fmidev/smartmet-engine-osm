# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

smartmet-engine-osm is a SmartMet Server engine that provides zero-copy, memory-mapped access to PMTiles v3 files containing pre-tiled OpenStreetMap vector data. It is used by the Dali WMS plugin for vector tile rendering of OSM map layers.

## Build commands

```bash
make                # Build osm.so shared library
make debug          # Build with debug flags
make test           # Run tests (delegates to test/Makefile)
make clean          # Remove build artifacts
make format         # Run clang-format on all source files
make install        # Install library to $(enginedir), headers to $(includedir)/smartmet/engines/osm/
make rpm            # Build RPM package
```

Requires `smartbuildcfg` to be installed (provides `makefile.inc`). pkg-config dependencies: `configpp`, `zlib`.

## Architecture

Three classes, all in `SmartMet::Engine::OSM`:

- **Engine** (`osm/Engine.h`) — SmartMetEngine plugin loaded by the server via `dlopen`. Opens all configured PMTiles sources at `init()`, dispatches `getTile(source, z, x, y)` to the correct reader. Entry points for plugin loading: `engine_class_creator` and `engine_name` (returns "OSM").

- **Config** (`osm/Config.h`) — Parses a libconfig file defining named tile sources (each maps a name to a `.pmtiles` file path) and a global `leaf_cache_size` setting.

- **PMTilesReader** (`osm/PMTilesReader.h`) — One instance per `.pmtiles` file. Memory-maps the entire file (`mmap MAP_SHARED, PROT_READ`). Tile lookup converts (z,x,y) to a Hilbert-curve tile ID, then walks the two-level directory structure (root + leaf) to locate tile bytes. Returns `TileData` — a raw pointer into the mmap'd region (zero-copy). Thread-safe via `std::shared_mutex` on the leaf directory cache.

### Key data flow

1. Dali plugin calls `Engine::getTile("scandinavia", z, x, y)`
2. Engine looks up the named `PMTilesReader` in `itsSources`
3. Reader converts (z,x,y) → Hilbert tile ID via `tileToId()`
4. Binary search in root directory; if entry points to a leaf, fetch/cache/decompress that leaf directory
5. Return `TileData{pointer, size}` directly into mmap'd region, or `std::nullopt`

### Compression

Directories may be gzip or zstd compressed. Tile data is returned as-is (still compressed per `tileCompression` in header) — decompression is the caller's responsibility. Brotli is explicitly unsupported.

## Configuration

libconfig format (see `test/cnf/osm.conf` for a full example):

```
leaf_cache_size = 1024;
sources: {
  scandinavia: { file = "/var/smartmet/osm/scandinavia.pmtiles"; };
  global:      { file = "/var/smartmet/osm/global.pmtiles"; };
};
```

## Dependencies

Linked libraries: `smartmet-spine`, `smartmet-macgyver`, `boost_thread`, `config++`, `zstd`, `z`, `pthread`.
