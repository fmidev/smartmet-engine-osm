// ======================================================================
/*!
 * \brief Vector tiles stored in PostgreSQL
 *
 * The database holds loaded versions of datasets, and one published version
 * of each dataset. Publishing a new version is a single transaction, so the
 * servers switch from one complete version to the next one atomically, and
 * queries in progress finish using the version they started with. Read-only
 * replicas can serve the tiles.
 *
 * Schema (created by smartmet-osm-pgload):
 *
 *   <schema>.sources     one row per loaded PMTiles file: dataset, version,
 *                        source name, zoom levels, bounds, compression,
 * metadata <schema>.datasets    the published version of each dataset
 *   <schema>.tiles_<id>  tile_id, run_length, data_id for source <id>
 *   <schema>.tiledata_<id> data_id, data (the tile as stored in the PMTiles
 * file)
 *
 * The tile ids and runs are those of the PMTiles format, so identical tiles
 * (oceans, plain land) are stored only once.
 */
// ======================================================================

#pragma once

#include "Config.h"
#include "TileSource.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Fmi
{
namespace Database
{
class PostgreSQLConnection;
}  // namespace Database
}  // namespace Fmi

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
class PgConnectionPool;

// A published source as listed in the database
struct PgSourceRecord
{
  std::int64_t id = 0;
  std::string dataset;
  std::string version;
  std::string source;
  SourceHeader header;
  std::string metadata;
  std::int64_t loaded = 0;  // seconds since epoch
};

class PgSource : public TileSource
{
 public:
  PgSource(std::shared_ptr<PgConnectionPool> thePool,
           std::string theSchema,
           PgSourceRecord theRecord);

  std::optional<std::string> getDecompressedTile(uint8_t z, uint32_t x, uint32_t y) const override;

  std::vector<std::optional<std::string>> getDecompressedTiles(
      const std::vector<TileCoord>& theTiles) const override;

  bool batched() const override { return true; }

  std::string metadata() const override { return itsRecord.metadata; }

  const PgSourceRecord& record() const { return itsRecord; }

 private:
  std::shared_ptr<PgConnectionPool> itsPool;
  PgSourceRecord itsRecord;
  std::string itsQuery;
};

// The tile database: connections and the list of published sources
class PgDatabase
{
 public:
  explicit PgDatabase(const PgConfig& theConfig);
  ~PgDatabase();

  PgDatabase(const PgDatabase&) = delete;
  PgDatabase& operator=(const PgDatabase&) = delete;
  PgDatabase(PgDatabase&&) = delete;
  PgDatabase& operator=(PgDatabase&&) = delete;

  // The published sources, throws if the database cannot be read
  std::vector<PgSourceRecord> published() const;

  // A source for a published record
  std::shared_ptr<PgSource> open(const PgSourceRecord& theRecord) const;

  // Description of the database for messages, without the password
  std::string name() const;

 private:
  PgConfig itsConfig;
  std::shared_ptr<PgConnectionPool> itsPool;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
