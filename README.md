# smartmet-engine-osm

Part of [SmartMet Server](https://github.com/fmidev/smartmet-server). See the [SmartMet Server documentation](https://github.com/fmidev/smartmet-server) for a full overview of the ecosystem.

## Overview

The OSM engine serves vector tile datasets such as OpenStreetMap basemaps from PMTiles
files in a configured data directory (also on NFS) or from PostgreSQL, and renders them
server side with MapLibre styles.
Global data with few zoom levels and detailed regional files can be combined, so a
server can be localized anywhere in the world. The WMS plugin uses the engine for map
backgrounds and vector tiles.

The engine package ships MapLibre styles for the Protomaps basemap schema, among them
backgrounds designed for meteorological data, the `smartmet-osm-update` tool for
installing the [Protomaps](https://docs.protomaps.com/basemaps/downloads) builds, and the
`smartmet-osm-pgload` tool for loading and publishing versions in PostgreSQL. The map
data itself is not packaged.

## Documentation

- [Developer guide](docs/developer-guide.md) — data, configuration, API, rendering, styles, performance, pitfalls

## License

MIT — see [LICENSE](LICENSE). The shipped styles: Protomaps map design CC0, VersaTiles
styles MIT. OpenStreetMap data is © OpenStreetMap contributors under the ODbL.

## Contributing

Bug reports and pull requests are welcome on [GitHub](../../issues).
