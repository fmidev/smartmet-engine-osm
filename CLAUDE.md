# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

smartmet-engine-osm is a SmartMet Server engine serving vector tile datasets (typically
OpenStreetMap basemaps such as the Protomaps builds) from PMTiles v3 files or PostgreSQL,
and rendering them server side with MapLibre styles into SVG elements. Used by the WMS/Dali
plugin's `OSMLayer` (raster maps, OGC API Tiles MVT); designed to serve an OGC API Maps
plugin too.

Full developer documentation: `docs/developer-guide.md`.

## Build commands

```bash
make                # Build osm.so
make test           # Unit tests (test/*Test.cpp)
make -C test render # Render tool for development and benchmarks
make install        # Library, headers, styles (/usr/share/smartmet/osm/styles), smartmet-osm-update, smartmet-osm-pgload
make rpm
```

Styles are generated: `cd styles && npm install && node generate.mjs .` (needs node >= 22).
Test fixtures: `cd test && python3 make_fixtures.py`.
PostgreSQL tests run only with `OSM_TEST_DB="<libpq connection string>"` (schema `osm_test`).

## Architecture (all in `SmartMet::Engine::OSM`)

- **Engine** (`Engine.h`) — SmartMetEngine. Virtual interface so plugins need not link against it (get it with `static_pointer_cast` from `Reactor::getSingleton("OSM")`). Owns the stores and a rescan thread.
- **Config** — data `directory`, `styles`, cache sizes, `rescan_interval`, `min_file_age`, `file_access`, explicit `datasets`, `postgresql`.
- **TileStore** — datasets = subdirectories (and single files) of the data directory plus published database datasets. Only the newest of versioned file names (`name-YYYYMMDD.pmtiles`) is used. `getTile` returns the most detailed decompressed tile at or above the request (ancestor fallback); sources are sorted by max zoom, then area. Rescans reuse readers of unchanged files (path+size+mtime+inode signature) unless they have failed; a failed read rescans and retries once. `prefetch` batches tiles for database sources. The tile cache also records missing tiles (null).
- **TileSource** — interface of a source (id for cache keys, summary, decompressed tiles, batch fetch, `failed()`), Hilbert tile id, decompression (gzip, zstd).
- **PMTilesReader** — one file, mmap on local disks or pread on network file systems (statfs); root directory + LRU of leaf directories; `forEachEntry` for the loader.
- **PgSource / PgDatabase** — tiles in PostgreSQL (`sources`, `datasets`, `tiles_<id>`, `tiledata_<id>`); one query per batch; lazily opened connection pool; keeps the previous sources if the database is down. Loaded with `tools/pgload.cpp` (`smartmet-osm-pgload`, libpqxx directly, COPY streams).
- **Mvt** (`MvtTile.h`) — protobuf wire decoder (only wanted layers), encoder, `overzoom`.
- **Expression** — MapLibre expressions, legacy filters and functions compiled to a tree; `Value`, `Color`, `changeCase`.
- **Style / StyleStore** — MapLibre style documents from the style directory, reloaded when modified.
- **Renderer** — zoom from resolution (512 px tiles), tile cover by sampling the image, grouping by available resolution (no overlap of coarse and fine data), clip/project (per-tile interpolation grids) /clip, one SVG path per layer and paint, label placement. Produces a `Scene`.
- **Clip** — Sutherland-Hodgman rings and Cohen-Sutherland lines.

## Gotchas

- Files must be replaced by rename, never overwritten (mmap → SIGBUS). On NFS use the default `file_access = auto` (pread); `mmap` there crashes when the server replaces a file.
- libpqxx differs between RHEL 8 (7.7), 9 (7.9) and 10 (7.10): `exec_params` is deprecated from 7.9, the loader has a wrapper. Public headers must not include pqxx (the WMS plugin includes them).
- Changing the order of virtual functions in `Engine` is an ABI break for the WMS plugin.
- Styles must match the tile schema of the dataset (Protomaps schema for the shipped styles).
