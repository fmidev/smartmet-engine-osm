// ======================================================================
/*!
 * \brief OSM engine configuration implementation
 */
// ======================================================================

#include "Config.h"
#include <macgyver/Exception.h>
#include <libconfig.h++>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{

Config::Config(const std::string& configFile)
{
  try
  {
    libconfig::Config cfg;
    cfg.setIncludeDir(std::filesystem::path(configFile).parent_path().c_str());
    cfg.readFile(configFile.c_str());

    std::string dir;
    if (cfg.lookupValue("directory", dir))
      itsDirectory = dir;

    if (cfg.lookupValue("styles", dir))
      itsStyleDirectory = dir;

    cfg.lookupValue("leaf_cache_size", itsLeafCacheSize);
    cfg.lookupValue("decoded_cache_mb", itsDecodedCacheMB);
    cfg.lookupValue("tile_cache_mb", itsTileCacheMB);
    cfg.lookupValue("rescan_interval", itsRescanInterval);
    cfg.lookupValue("min_file_age", itsMinFileAge);

    std::string access;
    if (cfg.lookupValue("file_access", access))
      itsFileAccess = parseFileAccess(access);

    // postgresql:
    // {
    //   host = "localhost"; port = 5432; database = "osm";
    //   username = "osm"; password = "secret"; schema = "osm";
    // };
    if (cfg.exists("postgresql"))
    {
      const auto& pg = cfg.lookup("postgresql");
      if (!pg.isGroup())
        throw Fmi::Exception(BCP, "OSM engine config: 'postgresql' must be a group");

      PgConfig db;
      pg.lookupValue("host", db.host);
      pg.lookupValue("port", db.port);
      pg.lookupValue("database", db.database);
      pg.lookupValue("username", db.username);
      pg.lookupValue("password", db.password);
      pg.lookupValue("connect_timeout", db.connectTimeout);
      pg.lookupValue("schema", db.schema);
      pg.lookupValue("max_idle_connections", db.maxIdleConnections);

      if (db.database.empty())
        throw Fmi::Exception(BCP, "OSM engine config: postgresql.database is required");
      if (db.schema.empty() ||
          db.schema.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") !=
              std::string::npos)
        throw Fmi::Exception(BCP,
                             "OSM engine config: postgresql.schema must consist of lower case "
                             "letters, digits and underscores");
      itsDatabase = std::move(db);
    }

    if (itsRescanInterval < 0)
      throw Fmi::Exception(BCP, "OSM engine config: rescan_interval must be >= 0");

    // datasets:
    // {
    //   protomaps: { directory = "/smartmet/data/osm/protomaps"; };
    //   test:      { files = ["/path/a.pmtiles", "/path/b.pmtiles"]; };
    // };
    if (cfg.exists("datasets"))
    {
      const auto& datasets = cfg.lookup("datasets");
      if (!datasets.isGroup())
        throw Fmi::Exception(BCP, "OSM engine config: 'datasets' must be a group");

      for (int i = 0; i < datasets.getLength(); ++i)
      {
        const auto& ds = datasets[i];
        std::string name = ds.getName();

        DatasetConfig dc;
        std::string path;
        if (ds.lookupValue("directory", path))
          dc.directory = path;
        if (ds.exists("files"))
        {
          const auto& files = ds.lookup("files");
          for (int j = 0; j < files.getLength(); ++j)
            dc.files.emplace_back(static_cast<const char*>(files[j]));
        }
        if (dc.directory.empty() && dc.files.empty())
          throw Fmi::Exception(BCP,
                               "OSM engine: dataset '" + name + "' needs 'directory' or 'files'");

        itsDatasets.emplace(std::move(name), std::move(dc));
      }
    }

    // Missing data is not an error: the data may be installed later, in which
    // case the rescans will pick it up.
  }
  catch (const libconfig::ParseException& e)
  {
    throw Fmi::Exception(BCP, "OSM engine config parse error")
        .addParameter("file", e.getFile() ? e.getFile() : configFile)
        .addParameter("line", std::to_string(e.getLine()))
        .addParameter("error", e.getError());
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "OSM engine Config constructor failed")
        .addParameter("file", configFile);
  }
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
