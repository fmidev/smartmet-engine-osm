// ======================================================================
/*!
 * \brief Render a map with the OSM engine renderer without a server
 *
 * Usage: render <datadir> <styledir> <dataset> <style> <crs> <x1,y1,x2,y2> <width> <height>
 *               [repeat] > map.svg
 *
 * Used for development and speed tests. The timing is printed to stderr.
 */
// ======================================================================

#include "Config.h"
#include "Renderer.h"
#include "Style.h"
#include "TileStore.h"
#include <boost/algorithm/string.hpp>
#include <fmt/format.h>
#include <gis/Box.h>
#include <gis/SpatialReference.h>
#include <macgyver/Exception.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace SmartMet::Engine::OSM;

int main(int argc, char* argv[])
{
  try
  {
    if (argc < 9)
    {
      std::cerr << "Usage: render datadir styledir dataset style crs x1,y1,x2,y2 width height "
                   "[repeat] [zoomoffset]\n";
      return 1;
    }

    auto conf = std::filesystem::temp_directory_path() /
                ("osm_render_" + std::to_string(::getpid()) + ".conf");
    {
      std::ofstream out(conf);
      out << "directory = \"" << argv[1] << "\";\nstyles = \"" << argv[2]
          << "\";\nmin_file_age = 0;\n";
      // Additional settings such as file_access or postgresql
      if (const char* extra = std::getenv("OSM_CONFIG_EXTRA"))
        out << extra << "\n";
    }
    Config config(conf.string());
    std::filesystem::remove(conf);

    TileStore tiles(config);
    {
      // Keep the scan messages out of the SVG output
      auto* buf = std::cout.rdbuf(std::cerr.rdbuf());
      tiles.rescan();
      std::cout.rdbuf(buf);
    }
    StyleStore styles(config.styleDirectory());
    Renderer renderer(tiles, styles, config.decodedCacheBytes());

    MapOptions options;
    options.dataset = argv[3];
    options.style = argv[4];
    if (argc > 10)
      options.zoomOffset = std::stod(argv[10]);

    Fmi::SpatialReference crs(argv[5]);
    std::vector<std::string> parts;
    boost::algorithm::split(parts, std::string(argv[6]), boost::is_any_of(","));
    const auto width = std::stoul(argv[7]);
    const auto height = std::stoul(argv[8]);
    Fmi::Box box(std::stod(parts[0]),
                 std::stod(parts[1]),
                 std::stod(parts[2]),
                 std::stod(parts[3]),
                 width,
                 height);

    const int repeat = (argc > 9 ? std::stoi(argv[9]) : 1);
    ScenePtr scene;
    for (int i = 0; i < repeat; i++)
    {
      auto t1 = std::chrono::steady_clock::now();
      scene = renderer.render(options, crs, box);
      auto t2 = std::chrono::steady_clock::now();
      std::cerr << fmt::format(
          "render {} ms, zoom {:.2f}, tile zoom {}, {} tiles, {} elements\n",
          std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1000.0,
          scene->zoom,
          scene->tileZoom,
          scene->tiles,
          scene->elements.size());
    }
    std::cout << scene->svg();
    return 0;
  }
  catch (...)
  {
    Fmi::Exception ex(BCP, "render failed", nullptr);
    ex.printError();
    return 1;
  }
}
