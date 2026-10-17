# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

smartmet-engine-osm is a SmartMet Server engine serving vector tile datasets (typically
OpenStreetMap basemaps such as the Protomaps builds) from memory-mapped PMTiles v3 files,
and rendering them server side with MapLibre styles into SVG elements. Used by the WMS/Dali
plugin's `OSMLayer` (raster maps, OGC API Tiles MVT); designed to serve an OGC API Maps
plugin too.

Full developer documentation: `docs/developer-guide.md`.

## Build commands

```bash
make                # Build osm.so
make test           # Unit tests (test/*Test.cpp)
make -C test render # Render tool for development and benchmarks
make install        # Library, headers, styles (/usr/share/smartmet/osm/styles), smartmet-osm-update
make rpm
```

Styles are generated: `cd styles && npm install && node generate.mjs .` (needs node >= 22).
Test fixtures: `cd test && python3 make_fixtures.py`.

## Architecture (all in `SmartMet::Engine::OSM`)

- **Engine** (`Engine.h`) — SmartMetEngine. Virtual interface so plugins need not link against it (get it with `static_pointer_cast` from `Reactor::getSingleton("OSM")`). Owns the stores and a rescan thread.
- **Config** — data `directory`, `styles`, cache sizes, `rescan_interval`, `min_file_age`, explicit `datasets`.
- **TileStore** — datasets = subdirectories (and single files) of the data directory. `getTile` returns the most detailed decompressed tile at or above the request (ancestor fallback); files are sorted by max zoom, then area. Rescans reuse readers of unchanged files (path+size+mtime+inode signature).
- **PMTilesReader** — one mmap'd file; Hilbert tile id; root directory + LRU of leaf directories; decompression (gzip, zstd).
- **Mvt** (`MvtTile.h`) — protobuf wire decoder (only wanted layers), encoder, `overzoom`.
- **Expression** — MapLibre expressions, legacy filters and functions compiled to a tree; `Value`, `Color`, `changeCase`.
- **Style / StyleStore** — MapLibre style documents from the style directory, reloaded when modified.
- **Renderer** — zoom from resolution (512 px tiles), tile cover by sampling the image, grouping by available resolution (no overlap of coarse and fine data), clip/project (per-tile interpolation grids) /clip, one SVG path per layer and paint, label placement. Produces a `Scene`.
- **Clip** — Sutherland-Hodgman rings and Cohen-Sutherland lines.

## Gotchas

- Files must be replaced by rename, never overwritten (mmap → SIGBUS); not on NFS.
- Changing the order of virtual functions in `Engine` is an ABI break for the WMS plugin.
- Styles must match the tile schema of the dataset (Protomaps schema for the shipped styles).
