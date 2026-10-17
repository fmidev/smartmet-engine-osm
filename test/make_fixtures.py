#!/usr/bin/env python3
"""
Generate the PMTiles fixtures of the engine tests into test/data.

The tiles are not real vector tiles, each tile contains the text
"<file> <z>/<x>/<y>" so that the tests can tell which file and tile
answered a request.

    data/osm/basemap/world.pmtiles   zoom 0-2, whole world, leaf directories
    data/osm/basemap/region.pmtiles  zoom 0-5, lon 20...30, lat 60...65
    data/osm/single.pmtiles          zoom 0-1, whole world
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pmtiles_writer  # noqa: E402


def lon2x(lon, z):
    return int((lon + 180.0) / 360.0 * (1 << z))


def lat2y(lat, z):
    r = math.radians(lat)
    return int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * (1 << z))


def make(path, name, maxzoom, bounds, leaf_size=4096):
    w, s, e, n = bounds
    tiles = {}
    for z in range(maxzoom + 1):
        top = (1 << z) - 1
        for x in range(lon2x(w, z), min(lon2x(e, z), top) + 1):
            for y in range(lat2y(n, z), min(lat2y(s, z), top) + 1):
                tiles[(z, x, y)] = f"{name} {z}/{x}/{y}".encode()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pmtiles_writer.write(path, tiles, {"name": name}, bounds, leaf_size=leaf_size)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "osm")
    world = (-180.0, -85.0511, 180.0, 85.0511)
    make(os.path.join(root, "basemap", "world.pmtiles"), "world", 2, world, leaf_size=8)
    make(os.path.join(root, "basemap", "region.pmtiles"), "region", 5, (20.0, 60.0, 30.0, 65.0))
    make(os.path.join(root, "single.pmtiles"), "single", 1, world)


if __name__ == "__main__":
    main()
