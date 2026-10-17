// ======================================================================
/*!
 * \brief Server side rendering of vector tile datasets with MapLibre styles
 *
 * Outline:
 *
 *  1. The style zoom level is chosen like MapLibre does: 512 pixel tiles,
 *     based on the resolution at the center of the image.
 *  2. The tiles covering the image are found by sampling the image in its
 *     own projection, which works for any projection.
 *  3. Each tile is taken from the most detailed file available. When a
 *     file only has an ancestor of the tile, the ancestor is clipped to
 *     the area of the requested tiles it replaces, so that detailed and
 *     coarse data never overlap.
 *  4. Features are clipped in tile coordinates, projected to pixels,
 *     clipped again to the image, and merged into one SVG path per style
 *     layer and paint combination.
 *  5. Point labels are placed last, in style priority order, avoiding
 *     collisions.
 */
// ======================================================================

#include "Renderer.h"
#include "Clip.h"
#include <boost/functional/hash.hpp>
#include <fmt/format.h>
#include <gis/Box.h>
#include <gis/CoordinateTransformation.h>
#include <gis/SpatialReference.h>
#include <macgyver/Exception.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace
{
constexpr double kHalf = 20037508.342789244;  // half the circumference in EPSG:3857
constexpr double kMaxLat = 85.0511287798066;
constexpr double kTileSize = 512;  // MapLibre renders vector tiles at 512 pixels

std::string escape(const std::string& s)
{
  std::string ret;
  ret.reserve(s.size());
  for (char c : s)
  {
    switch (c)
    {
      case '&':
        ret += "&amp;";
        break;
      case '<':
        ret += "&lt;";
        break;
      case '>':
        ret += "&gt;";
        break;
      case '"':
        ret += "&quot;";
        break;
      default:
        ret += c;
    }
  }
  return ret;
}

std::string number(double v, int precision = 2)
{
  auto s = fmt::format("{:.{}f}", v, precision);
  if (s.find('.') != std::string::npos)
  {
    while (s.back() == '0')
      s.pop_back();
    if (s.back() == '.')
      s.pop_back();
  }
  if (s == "-0")
    s = "0";
  return s;
}

bool matches(const std::string& id, const std::vector<std::string>& patterns)
{
  for (const auto& p : patterns)
  {
    if (!p.empty() && p.back() == '*')
    {
      if (id.compare(0, p.size() - 1, p, 0, p.size() - 1) == 0)
        return true;
    }
    else if (id == p)
      return true;
  }
  return false;
}

// Number of UTF-8 characters
std::size_t utf8_length(const std::string& s)
{
  std::size_t n = 0;
  for (char c : s)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  return n;
}

std::pair<uint32_t, uint32_t> lonlat_to_tile(double lon, double lat, int z)
{
  const double n = std::ldexp(1.0, z);
  lat = std::clamp(lat, -kMaxLat, kMaxLat);
  const double r = lat * M_PI / 180;
  auto x = static_cast<int64_t>(std::floor((lon + 180) / 360 * n));
  auto y = static_cast<int64_t>(std::floor((1 - std::asinh(std::tan(r)) / M_PI) / 2 * n));
  const auto top = static_cast<int64_t>(n) - 1;
  return {static_cast<uint32_t>(std::clamp<int64_t>(x, 0, top)),
          static_cast<uint32_t>(std::clamp<int64_t>(y, 0, top))};
}

double tile_lat(uint32_t y, int z)
{
  const double n = M_PI - 2.0 * M_PI * y / std::ldexp(1.0, z);
  return 180.0 / M_PI * std::atan(std::sinh(n));
}

// Does the tile intersect the bounds of a file?
bool intersects(const FileInfo& f, int z, uint32_t x, uint32_t y)
{
  const double n = std::ldexp(1.0, z);
  const double west = x / n * 360.0 - 180.0;
  const double east = (x + 1) / n * 360.0 - 180.0;
  return west < f.east && east > f.west && tile_lat(y + 1, z) < f.north && tile_lat(y, z) > f.south;
}

// Most detailed zoom level available for a tile, at most z
int available_zoom(const DatasetInfo& info, int z, uint32_t x, uint32_t y)
{
  int best = -1;
  for (const auto& f : info.files)
  {
    const int zz = std::min<int>(z, f.maxzoom);
    if (zz <= best || zz < f.minzoom)
      continue;
    if (intersects(f, zz, x >> (z - zz), y >> (z - zz)))
      best = zz;
  }
  return best;
}

// ----------------------------------------------------------------------
/*!
 * \brief Projection from tile coordinates to image pixels
 */
// ----------------------------------------------------------------------

class Projector
{
 public:
  Projector(const Fmi::SpatialReference& crs, const Fmi::Box& box) : itsBox(box)
  {
    auto epsg = crs.getEPSG();
    if (!epsg || *epsg != 3857)
      itsTransformation.emplace(Fmi::SpatialReference("EPSG:3857"), crs);
  }

  bool isWebMercator() const { return !itsTransformation; }

  // Project tile coordinates in place to pixel coordinates. Failed points become NaN.
  //
  // Calling PROJ for every point dominates the rendering time, hence each tile
  // gets a grid of exactly projected points and the points in between are
  // interpolated bilinearly. The grid is verified at the cell centres and
  // abandoned for exact projection if the error would be visible.
  void project(std::vector<double>& x,
               std::vector<double>& y,
               int z,
               uint32_t tx,
               uint32_t ty,
               uint32_t extent) const
  {
    if (!itsTransformation)
    {
      exact(x, y, z, tx, ty, extent);
      return;
    }

    const auto& grid = getGrid(z, tx, ty, extent);
    if (grid.n == 0)
    {
      exact(x, y, z, tx, ty, extent);
      return;
    }

    const int n = grid.n;
    const double scale = static_cast<double>(n) / extent;
    for (std::size_t k = 0; k < x.size(); k++)
    {
      const double u = x[k] * scale;
      const double v = y[k] * scale;
      const int i = std::clamp(static_cast<int>(std::floor(u)), 0, n - 1);
      const int j = std::clamp(static_cast<int>(std::floor(v)), 0, n - 1);
      const double fu = u - i;
      const double fv = v - j;
      const std::size_t p00 = static_cast<std::size_t>(j) * (n + 1) + i;
      const std::size_t p01 = p00 + n + 1;
      x[k] = (1 - fv) * ((1 - fu) * grid.x[p00] + fu * grid.x[p00 + 1]) +
             fv * ((1 - fu) * grid.x[p01] + fu * grid.x[p01 + 1]);
      y[k] = (1 - fv) * ((1 - fu) * grid.y[p00] + fu * grid.y[p00 + 1]) +
             fv * ((1 - fu) * grid.y[p01] + fu * grid.y[p01 + 1]);
    }
  }

 private:
  struct Grid
  {
    int n = 0;  // number of cells per side, 0 if exact projection must be used
    std::vector<double> x;
    std::vector<double> y;
  };

  void exact(std::vector<double>& x,
             std::vector<double>& y,
             int z,
             uint32_t tx,
             uint32_t ty,
             uint32_t extent) const
  {
    const double size = 2 * kHalf / std::ldexp(1.0, z);
    const double scale = size / extent;
    const double x0 = -kHalf + tx * size;
    const double y0 = kHalf - ty * size;
    for (std::size_t i = 0; i < x.size(); i++)
    {
      x[i] = x0 + x[i] * scale;
      y[i] = y0 - y[i] * scale;
    }
    // Tile buffers extend beyond the antimeridian, and PROJ would wrap such
    // longitudes to the other side of the world
    if (itsTransformation)
      for (auto& v : x)
        v = std::clamp(v, -kHalf, kHalf);
    if (itsTransformation && !x.empty())
      itsTransformation->transform(x, y);
    for (std::size_t i = 0; i < x.size(); i++)
      itsBox.transform(x[i], y[i]);
  }

  const Grid& getGrid(int z, uint32_t tx, uint32_t ty, uint32_t extent) const
  {
    const auto key = std::make_tuple(z, tx, ty, extent);
    auto it = itsGrids.find(key);
    if (it != itsGrids.end())
      return it->second;

    Grid grid;
    for (int n : {32, 128})
    {
      // Exactly projected grid nodes and cell centres in one call
      const std::size_t nodes = static_cast<std::size_t>(n + 1) * (n + 1);
      std::vector<double> x;
      std::vector<double> y;
      x.reserve(nodes + n * n);
      y.reserve(nodes + n * n);
      const double step = static_cast<double>(extent) / n;
      for (int j = 0; j <= n; j++)
        for (int i = 0; i <= n; i++)
        {
          x.push_back(i * step);
          y.push_back(j * step);
        }
      for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
        {
          x.push_back((i + 0.5) * step);
          y.push_back((j + 0.5) * step);
        }
      exact(x, y, z, tx, ty, extent);

      bool ok = std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); }) &&
                std::all_of(y.begin(), y.end(), [](double v) { return std::isfinite(v); });

      // Bilinear interpolation at a cell centre is the mean of its corners
      for (int j = 0; ok && j < n; j++)
        for (int i = 0; ok && i < n; i++)
        {
          const std::size_t p00 = static_cast<std::size_t>(j) * (n + 1) + i;
          const std::size_t p01 = p00 + n + 1;
          const std::size_t c = nodes + static_cast<std::size_t>(j) * n + i;
          const double ix = (x[p00] + x[p00 + 1] + x[p01] + x[p01 + 1]) / 4;
          const double iy = (y[p00] + y[p00 + 1] + y[p01] + y[p01 + 1]) / 4;
          if (std::hypot(ix - x[c], iy - y[c]) > 0.2)
            ok = false;
        }

      if (ok)
      {
        grid.n = n;
        x.resize(nodes);
        y.resize(nodes);
        grid.x = std::move(x);
        grid.y = std::move(y);
        break;
      }
    }
    return itsGrids.emplace(key, std::move(grid)).first->second;
  }

  const Fmi::Box& itsBox;
  std::optional<Fmi::CoordinateTransformation> itsTransformation;

  // Grids of the tiles used, the projector is used by one thread only
  mutable std::map<std::tuple<int, uint32_t, uint32_t, uint32_t>, Grid> itsGrids;
};

// A tile to be rendered and the areas of it to use
struct TileWork
{
  Mvt::TilePtr tile;
  int z = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  std::vector<Rect> clips;  // in tile units of a 1x1 tile, scaled by the layer extent later
};

// ----------------------------------------------------------------------
/*!
 * \brief Builds SVG path data from geometries
 */
// ----------------------------------------------------------------------

class PathBuilder
{
 public:
  PathBuilder(const Projector& projector, const Rect& pixels, int precision, bool densify)
      : itsProjector(projector), itsPixels(pixels), itsPrecision(precision), itsDensify(densify)
  {
  }

  // Add a geometry of a feature from a tile. Polygon rings are filled, lines
  // and polygon outlines are stroked.
  void add(const Mvt::Feature& feature,
           const TileWork& work,
           uint32_t extent,
           bool fill,
           std::string& path) const
  {
    const bool polygon = (feature.type == Mvt::GeomType::Polygon);
    if (fill && !polygon)
      return;
    if (feature.type == Mvt::GeomType::Point || feature.type == Mvt::GeomType::Unknown)
      return;

    // Fills overlap slightly at tile edges to avoid antialiasing seams
    const double eps = (fill ? extent / 1024.0 : 0.0);

    std::vector<Part> pieces;
    for (const auto& clip : work.clips)
    {
      const Rect rect{clip.xmin * extent - eps,
                      clip.ymin * extent - eps,
                      clip.xmax * extent + eps,
                      clip.ymax * extent + eps};
      for (const auto& part : feature.parts)
      {
        pieces.clear();
        if (fill)
        {
          auto ring = clipRing(part, rect);
          if (ring.size() >= 3)
            pieces.push_back(std::move(ring));
        }
        else if (polygon)
        {
          // Stroke polygon outlines, but not along the tile edges
          Part closed = part;
          closed.push_back(part.front());
          clipLine(closed, rect, pieces);
        }
        else
          clipLine(part, rect, pieces);

        for (auto& piece : pieces)
          emit(piece, work, extent, fill, path);
      }
    }
  }

  // Project a point, returns false if it fails
  std::optional<Point> point(const Point& p, const TileWork& work, uint32_t extent) const
  {
    std::vector<double> x{p.x};
    std::vector<double> y{p.y};
    itsProjector.project(x, y, work.z, work.x, work.y, extent);
    if (std::isnan(x[0]) || std::isnan(y[0]))
      return {};
    return Point{x[0], y[0]};
  }

 private:
  void emit(Part& piece, const TileWork& work, uint32_t extent, bool ring, std::string& path) const
  {
    if (itsDensify)
      densify(piece, extent / 16.0, ring);

    std::vector<double> x;
    std::vector<double> y;
    x.reserve(piece.size());
    y.reserve(piece.size());
    for (const auto& p : piece)
    {
      x.push_back(p.x);
      y.push_back(p.y);
    }
    itsProjector.project(x, y, work.z, work.x, work.y, extent);

    Part projected;
    projected.reserve(x.size());
    std::vector<Part> parts;
    for (std::size_t i = 0; i < x.size(); i++)
    {
      if (std::isnan(x[i]) || std::isnan(y[i]))
      {
        // Lines are split at failed points, rings just lose them
        if (!ring && projected.size() >= 2)
          parts.push_back(std::move(projected));
        if (!ring)
          projected.clear();
        continue;
      }
      projected.push_back(Point{x[i], y[i]});
    }
    if (!projected.empty())
      parts.push_back(std::move(projected));

    for (const auto& p : parts)
    {
      if (ring)
      {
        auto clipped = clipRing(p, itsPixels);
        if (clipped.size() >= 3)
          append(clipped, true, path);
      }
      else
      {
        std::vector<Part> lines;
        clipLine(p, itsPixels, lines);
        for (const auto& l : lines)
          append(l, false, path);
      }
    }
  }

  static void densify(Part& part, double maxlength, bool ring)
  {
    Part out;
    out.reserve(part.size());
    const std::size_t n = part.size();
    const std::size_t segments = (ring ? n : n - 1);
    for (std::size_t i = 0; i < segments; i++)
    {
      const auto& a = part[i];
      const auto& b = part[(i + 1) % n];
      out.push_back(a);
      const double len = std::hypot(b.x - a.x, b.y - a.y);
      if (len > maxlength)
      {
        const int steps = static_cast<int>(std::ceil(len / maxlength));
        for (int j = 1; j < steps; j++)
        {
          const double t = static_cast<double>(j) / steps;
          out.push_back(Point{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)});
        }
      }
    }
    if (!ring)
      out.push_back(part.back());
    part = std::move(out);
  }

  void append(const Part& part, bool ring, std::string& path) const
  {
    // Round and drop repeated points
    const double factor = std::pow(10.0, itsPrecision);
    std::vector<std::pair<long, long>> pts;
    pts.reserve(part.size());
    for (const auto& p : part)
    {
      std::pair<long, long> ip{std::lround(p.x * factor), std::lround(p.y * factor)};
      if (pts.empty() || pts.back() != ip)
        pts.push_back(ip);
    }
    if (ring && pts.size() > 1 && pts.front() == pts.back())
      pts.pop_back();
    if (pts.size() < (ring ? 3U : 2U))
      return;

    // Fast formatting of the rounded coordinates, which are integers scaled by
    // 10^precision: the integer part, then the fraction without trailing zeros
    const long divisor = std::lround(factor);
    auto coord = [&](long v)
    {
      if (v < 0)
      {
        path += '-';
        v = -v;
      }
      char buf[24];
      auto res = std::to_chars(buf, buf + sizeof(buf), v / divisor);
      path.append(buf, res.ptr);
      long frac = v % divisor;
      if (frac == 0)
        return;
      path += '.';
      for (long d = divisor / 10; d > 0 && frac > 0; d /= 10)
      {
        path += static_cast<char>('0' + frac / d);
        frac %= d;
      }
    };

    path += 'M';
    coord(pts[0].first);
    path += ' ';
    coord(pts[0].second);
    for (std::size_t i = 1; i < pts.size(); i++)
    {
      path += (i == 1 ? 'L' : ' ');
      coord(pts[i].first);
      path += ' ';
      coord(pts[i].second);
    }
    if (ring)
      path += 'Z';
  }

  const Projector& itsProjector;
  Rect itsPixels;
  int itsPrecision;
  bool itsDensify;
};

// ----------------------------------------------------------------------
/*!
 * \brief Evaluates a paint or layout property, once if it is not data dependent
 */
// ----------------------------------------------------------------------

class Property
{
 public:
  Property(const Expression* expr, const Context& zoomContext, Value defaultValue)
      : itsExpr(expr), itsDefault(std::move(defaultValue))
  {
    if (itsExpr == nullptr)
      itsConstant = itsDefault;
    else if (!itsExpr->isDataDependent())
    {
      itsConstant = itsExpr->evaluate(zoomContext);
      if (itsConstant->isNull())
        itsConstant = itsDefault;
    }
  }

  Value operator()(const Context& ctx) const
  {
    if (itsConstant)
      return *itsConstant;
    auto v = itsExpr->evaluate(ctx);
    return v.isNull() ? itsDefault : v;
  }

  double number(const Context& ctx) const { return (*this)(ctx).toNumber().value_or(0); }

 private:
  const Expression* itsExpr;
  Value itsDefault;
  std::optional<Value> itsConstant;
};

// Colour and opacity attributes, false if the result is invisible
bool add_color(std::vector<std::pair<std::string, std::string>>& attributes,
               const std::string& name,
               const Value& color,
               double opacity)
{
  auto c = parseColor(color.toString());
  if (!c)
    return false;
  const double alpha = c->a * opacity;
  if (alpha <= 0)
    return false;
  attributes.emplace_back(name, c->hex());
  if (alpha < 1)
    attributes.emplace_back(name + "-opacity", number(alpha, 3));
  return true;
}

// Paint groups of a layer in the order of appearance
struct Groups
{
  std::vector<std::pair<std::vector<std::pair<std::string, std::string>>, std::string>> groups;
  std::map<std::string, std::size_t> index;

  std::string& path(const std::string& key,
                    const std::vector<std::pair<std::string, std::string>>& attributes)
  {
    auto it = index.find(key);
    if (it != index.end())
      return groups[it->second].second;
    index[key] = groups.size();
    groups.emplace_back(attributes, std::string());
    return groups.back().second;
  }
};

// ----------------------------------------------------------------------
/*!
 * \brief Label candidate
 */
// ----------------------------------------------------------------------

struct Label
{
  int layer = 0;  // index of the style layer
  double sortKey = 0;
  std::size_t sequence = 0;
  std::string layerId;
  double x = 0;
  double y = 0;
  std::vector<std::string> lines;
  double size = 16;
  std::vector<std::string> anchors;
  double dx = 0;  // text-offset in pixels
  double dy = 0;
  double radial = 0;  // text-radial-offset in pixels
  double letterSpacing = 0;
  double padding = 2;
  bool icon = false;
  double iconRadius = 0;
  std::vector<std::pair<std::string, std::string>> attributes;  // presentation
  std::string iconColor;
  std::string iconStroke;
};

struct Box2
{
  double x1;
  double y1;
  double x2;
  double y2;
  bool overlaps(const Box2& o) const { return x1 < o.x2 && o.x1 < x2 && y1 < o.y2 && o.y1 < y2; }
};

// Font family, weight and style from a MapLibre font stack name such as "Noto Sans Bold Italic"
std::tuple<std::string, std::string, std::string> font(const std::string& name)
{
  static const std::vector<std::pair<std::string, std::string>> weights = {
      {" Regular", "normal"},
      {" Medium", "500"},
      {" Semibold", "600"},
      {" SemiBold", "600"},
      {" Bold", "bold"},
      {" Light", "300"},
      {" Black", "900"}};
  // VersaTiles uses names like "noto_sans_bold"
  std::string family;
  bool capitalize = true;
  for (char c : name)
  {
    if (c == '_' || c == ' ')
    {
      family += ' ';
      capitalize = true;
    }
    else
    {
      family += (capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
      capitalize = false;
    }
  }
  std::string style = "normal";
  std::string weight = "normal";
  auto italic = family.find(" Italic");
  if (italic != std::string::npos)
  {
    style = "italic";
    family.erase(italic, 7);
  }
  for (const auto& [suffix, w] : weights)
  {
    auto pos = family.find(suffix);
    if (pos != std::string::npos)
    {
      weight = w;
      family.erase(pos, suffix.size());
    }
  }
  // Remove version suffixes like " v1"
  auto v = family.rfind(" v");
  if (v != std::string::npos && v + 2 < family.size() &&
      std::isdigit(static_cast<unsigned char>(family[v + 2])))
    family.erase(v);
  return {family, weight, style};
}

// Word wrap at spaces to the given maximum number of characters
std::vector<std::string> wrap(const std::string& text, std::size_t maxchars)
{
  std::vector<std::string> lines;
  std::string paragraph;
  auto flush_paragraph = [&]()
  {
    std::string line;
    std::size_t pos = 0;
    while (pos <= paragraph.size())
    {
      auto next = paragraph.find(' ', pos);
      if (next == std::string::npos)
        next = paragraph.size();
      auto word = paragraph.substr(pos, next - pos);
      if (!line.empty() && utf8_length(line) + 1 + utf8_length(word) > maxchars)
      {
        lines.push_back(line);
        line = word;
      }
      else
        line += (line.empty() ? "" : " ") + word;
      pos = next + 1;
    }
    if (!line.empty())
      lines.push_back(line);
  };
  for (char c : text)
  {
    if (c == '\n')
    {
      flush_paragraph();
      paragraph.clear();
    }
    else
      paragraph += c;
  }
  flush_paragraph();
  return lines;
}

// Horizontal and vertical position of the anchor within the text box (0, 0.5 or 1)
std::pair<double, double> anchor_position(const std::string& anchor)
{
  double h = 0.5;
  double v = 0.5;
  if (anchor.find("left") != std::string::npos)
    h = 0;
  else if (anchor.find("right") != std::string::npos)
    h = 1;
  if (anchor.find("top") != std::string::npos)
    v = 0;
  else if (anchor.find("bottom") != std::string::npos)
    v = 1;
  return {h, v};
}

// ----------------------------------------------------------------------
/*!
 * \brief Collect the label candidates of a symbol layer
 */
// ----------------------------------------------------------------------

void collect_labels(const StyleLayer& layer,
                    int layerIndex,
                    const std::vector<TileWork>& works,
                    double zoom,
                    const PathBuilder& builder,
                    const MapOptions& options,
                    std::vector<Label>& labels)
{
  const Context zctx{zoom, nullptr, nullptr};
  Property textField(layer.layoutProperty("text-field"), zctx, Value());
  Property textSize(layer.layoutProperty("text-size"), zctx, Value(16.0));
  Property textFont(layer.layoutProperty("text-font"), zctx, Value());
  Property textTransform(layer.layoutProperty("text-transform"), zctx, Value(std::string("none")));
  Property textMaxWidth(layer.layoutProperty("text-max-width"), zctx, Value(10.0));
  Property letterSpacing(layer.layoutProperty("text-letter-spacing"), zctx, Value(0.0));
  Property textAnchor(layer.layoutProperty("text-anchor"), zctx, Value(std::string("center")));
  Property variableAnchor(layer.layoutProperty("text-variable-anchor"), zctx, Value());
  Property textOffset(layer.layoutProperty("text-offset"), zctx, Value());
  Property radialOffset(layer.layoutProperty("text-radial-offset"), zctx, Value(0.0));
  Property textPadding(layer.layoutProperty("text-padding"), zctx, Value(2.0));
  Property sortKey(layer.layoutProperty("symbol-sort-key"), zctx, Value(0.0));
  Property placement(layer.layoutProperty("symbol-placement"), zctx, Value(std::string("point")));
  Property iconImage(layer.layoutProperty("icon-image"), zctx, Value());
  Property iconSize(layer.layoutProperty("icon-size"), zctx, Value(1.0));
  Property textColor(layer.paintProperty("text-color"), zctx, Value(std::string("#000000")));
  Property textOpacity(layer.paintProperty("text-opacity"), zctx, Value(1.0));
  Property haloColor(layer.paintProperty("text-halo-color"), zctx, Value(std::string("rgba(0,0,0,0)")));
  Property haloWidth(layer.paintProperty("text-halo-width"), zctx, Value(0.0));

  for (const auto& work : works)
  {
    const auto* mvtlayer = work.tile->layer(layer.sourceLayer);
    if (mvtlayer == nullptr)
      continue;
    const auto extent = mvtlayer->extent;

    for (const auto& feature : mvtlayer->features)
    {
      if (feature.type != Mvt::GeomType::Point)
        continue;

      const Context ctx{zoom, mvtlayer, &feature};
      if (layer.filter && !layer.filter->evaluate(ctx).truthy())
        continue;
      if (placement(ctx).toString() != "point")
        continue;

      std::string text = textField(ctx).toString();
      const std::string icon = (options.icons ? iconImage(ctx).toString() : std::string());
      if (text.empty() && icon.empty())
        continue;

      const auto transform = textTransform(ctx).toString();
      if (transform == "uppercase" || transform == "lowercase")
        text = changeCase(text, transform == "uppercase");

      for (const auto& part : feature.parts)
      {
        const auto& p = part.front();
        // Each point belongs to exactly one clip area
        bool owned = false;
        for (const auto& clip : work.clips)
          if (p.x >= clip.xmin * extent && p.x < clip.xmax * extent && p.y >= clip.ymin * extent &&
              p.y < clip.ymax * extent)
            owned = true;
        if (!owned)
          continue;

        auto pixel = builder.point(p, work, extent);
        if (!pixel)
          continue;

        Label label;
        label.layer = layerIndex;
        label.layerId = layer.id;
        label.sortKey = sortKey.number(ctx);
        label.x = pixel->x;
        label.y = pixel->y;
        label.size = textSize.number(ctx);
        label.letterSpacing = letterSpacing.number(ctx) * label.size;
        label.padding = textPadding.number(ctx);
        const double maxwidth = textMaxWidth.number(ctx);
        label.lines = wrap(text, static_cast<std::size_t>(std::max(1.0, maxwidth / 0.55)));

        auto anchors = variableAnchor(ctx);
        if (anchors.type == Value::Type::Array && !anchors.array.empty())
        {
          for (const auto& a : anchors.array)
            label.anchors.push_back(a.toString());
          label.radial = radialOffset.number(ctx) * label.size;
        }
        else
          label.anchors.push_back(textAnchor(ctx).toString());

        auto offset = textOffset(ctx);
        if (offset.type == Value::Type::Array && offset.array.size() == 2)
        {
          label.dx = offset.array[0].toNumber().value_or(0) * label.size;
          label.dy = offset.array[1].toNumber().value_or(0) * label.size;
        }

        // Presentation attributes
        auto& attrs = label.attributes;
        std::string family = options.fontFamily;
        auto fonts = textFont(ctx);
        if (fonts.type == Value::Type::Array && !fonts.array.empty())
        {
          auto [fam, weight, style] = font(fonts.array[0].toString());
          if (options.fontFamily.compare(0, fam.size(), fam) != 0)
            family = fam + ", " + options.fontFamily;
          if (weight != "normal")
            attrs.emplace_back("font-weight", weight);
          if (style != "normal")
            attrs.emplace_back("font-style", style);
        }
        attrs.emplace(attrs.begin(), "font-family", escape(family));
        attrs.emplace(attrs.begin() + 1, "font-size", number(label.size, 1));
        const auto color = textColor(ctx);
        if (!add_color(attrs, "fill", color, textOpacity.number(ctx)))
          continue;
        const double halo = haloWidth.number(ctx);
        if (halo > 0)
        {
          if (add_color(attrs, "stroke", haloColor(ctx), textOpacity.number(ctx)))
          {
            attrs.emplace_back("stroke-width", number(2 * halo, 2));
            attrs.emplace_back("stroke-linejoin", "round");
            attrs.emplace_back("paint-order", "stroke");
          }
        }
        if (label.letterSpacing != 0)
          attrs.emplace_back("letter-spacing", number(label.letterSpacing, 2));

        if (!icon.empty())
        {
          label.icon = true;
          label.iconRadius = std::max(1.5, 3.5 * iconSize.number(ctx));
          auto c = parseColor(color.toString());
          label.iconColor = c ? c->hex() : "#000000";
          auto h = parseColor(haloColor(ctx).toString());
          label.iconStroke = (h && h->a > 0 ? h->hex() : "none");
        }
        if (text.empty())
          label.lines.clear();

        label.sequence = labels.size();
        labels.push_back(std::move(label));
      }
    }
  }
}

// ----------------------------------------------------------------------
/*!
 * \brief Place labels avoiding collisions and append them to the scene
 */
// ----------------------------------------------------------------------

void place_labels(std::vector<Label>& labels, Scene& scene)
{
  // Layers higher in the style are placed first, then by sort key
  std::stable_sort(labels.begin(),
                   labels.end(),
                   [](const Label& a, const Label& b)
                   {
                     if (a.layer != b.layer)
                       return a.layer > b.layer;
                     if (a.sortKey != b.sortKey)
                       return a.sortKey < b.sortKey;
                     return a.sequence < b.sequence;
                   });

  const Box2 image{0, 0, static_cast<double>(scene.width), static_cast<double>(scene.height)};
  std::vector<Box2> placed;

  auto free = [&](const Box2& b)
  {
    if (b.x1 < image.x1 || b.y1 < image.y1 || b.x2 > image.x2 || b.y2 > image.y2)
      return false;
    return std::none_of(placed.begin(), placed.end(), [&](const Box2& p) { return p.overlaps(b); });
  };

  for (const auto& label : labels)
  {
    std::size_t maxchars = 0;
    for (const auto& line : label.lines)
      maxchars = std::max(maxchars, utf8_length(line));
    const double width = maxchars * (0.55 * label.size + label.letterSpacing);
    const double lineheight = 1.2 * label.size;
    const double height = label.lines.size() * lineheight;

    std::optional<Box2> iconbox;
    if (label.icon)
    {
      const double r = label.iconRadius + 1;
      iconbox = Box2{label.x - r, label.y - r, label.x + r, label.y + r};
      if (!free(*iconbox))
        continue;
    }

    if (label.lines.empty())
    {
      if (iconbox)
      {
        placed.push_back(*iconbox);
        scene.elements.push_back(SceneElement{"circle",
                                              label.layerId,
                                              {{"cx", number(label.x, 1)},
                                               {"cy", number(label.y, 1)},
                                               {"r", number(label.iconRadius, 1)},
                                               {"fill", label.iconColor},
                                               {"stroke", label.iconStroke}},
                                              ""});
      }
      continue;
    }

    for (const auto& anchor : label.anchors)
    {
      auto [h, v] = anchor_position(anchor);

      // Offset away from the anchor point
      double ox = label.dx;
      double oy = label.dy;
      if (label.radial != 0 && anchor != "center")
      {
        ox = (h == 0 ? label.radial : (h == 1 ? -label.radial : 0));
        oy = (v == 0 ? label.radial : (v == 1 ? -label.radial : 0));
      }
      else if (label.icon && anchor == "center" && label.dx == 0 && label.dy == 0)
        oy = label.iconRadius + 0.6 * label.size;  // keep the dot visible

      const double left = label.x + ox - h * width;
      const double top = label.y + oy - v * height;
      const Box2 box{left - label.padding,
                     top - label.padding,
                     left + width + label.padding,
                     top + height + label.padding};
      if (!free(box))
        continue;

      placed.push_back(box);
      if (iconbox)
      {
        placed.push_back(*iconbox);
        scene.elements.push_back(SceneElement{"circle",
                                              label.layerId,
                                              {{"cx", number(label.x, 1)},
                                               {"cy", number(label.y, 1)},
                                               {"r", number(label.iconRadius, 1)},
                                               {"fill", label.iconColor},
                                               {"stroke", label.iconStroke}},
                                              ""});
      }

      // SVG text anchored horizontally by text-anchor, vertically by the baseline
      const double tx = left + h * width;
      const double baseline = top + 0.8 * label.size;
      SceneElement text;
      text.tag = "text";
      text.layer = label.layerId;
      text.attributes.emplace_back("x", number(tx, 1));
      text.attributes.emplace_back("y", number(baseline, 1));
      text.attributes.emplace_back("text-anchor", h == 0 ? "start" : (h == 1 ? "end" : "middle"));
      text.attributes.insert(text.attributes.end(), label.attributes.begin(), label.attributes.end());
      if (label.lines.size() == 1)
        text.content = escape(label.lines[0]);
      else
      {
        for (std::size_t i = 0; i < label.lines.size(); i++)
          text.content += fmt::format("<tspan x=\"{}\" dy=\"{}\">{}</tspan>",
                                      number(tx, 1),
                                      i == 0 ? "0" : number(lineheight, 1),
                                      escape(label.lines[i]));
      }
      scene.elements.push_back(std::move(text));
      break;
    }
  }
}

std::string strip_tags(const std::string& html)
{
  std::string ret;
  bool intag = false;
  for (char c : html)
  {
    if (c == '<')
      intag = true;
    else if (c == '>')
      intag = false;
    else if (!intag)
      ret += c;
  }
  // Decode the most common entity
  auto pos = ret.find("&copy;");
  if (pos != std::string::npos)
    ret.replace(pos, 6, "©");
  return ret;
}

}  // namespace

// ----------------------------------------------------------------------
// Scene
// ----------------------------------------------------------------------

std::string Scene::svgElements() const
{
  std::string out;
  for (const auto& e : elements)
  {
    out += '<';
    out += e.tag;
    for (const auto& [name, value] : e.attributes)
      out += fmt::format(" {}=\"{}\"", name, value);
    if (e.content.empty())
      out += "/>\n";
    else
      out += fmt::format(">{}</{}>\n", e.content, e.tag);
  }
  return out;
}

std::string Scene::svg() const
{
  return fmt::format(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" viewBox=\"0 0 {} "
      "{}\">\n{}</svg>\n",
      width,
      height,
      width,
      height,
      svgElements());
}

// ----------------------------------------------------------------------
// Renderer
// ----------------------------------------------------------------------

Renderer::Renderer(const TileStore& theTiles, const StyleStore& theStyles, std::size_t theCacheBytes)
    : itsTiles(theTiles), itsStyles(theStyles), itsCache(theCacheBytes)
{
}

Mvt::TilePtr Renderer::decode(const Tile& theTile,
                              const std::set<std::string>* theLayers,
                              std::size_t theLayerHash) const
{
  const Key key{{theTile.source, PMTilesReader::tileToId(theTile.z, theTile.x, theTile.y)},
                theLayerHash};
  if (auto cached = itsCache.find(key))
    return *cached;
  auto tile = std::make_shared<const Mvt::Tile>(Mvt::decode(*theTile.data, theLayers));
  itsCache.insert(key, tile);
  return tile;
}

// ----------------------------------------------------------------------

std::optional<std::string> Renderer::getVectorTile(const std::string& theDataset,
                                                   uint8_t z,
                                                   uint32_t x,
                                                   uint32_t y) const
{
  try
  {
    auto tile = itsTiles.getTile(theDataset, z, x, y);
    if (!tile)
      return {};
    if (tile->z == z)
      return *tile->data;

    // Cut the requested tile from the ancestor
    const int dz = z - tile->z;
    const uint32_t mask = (1U << dz) - 1;
    auto decoded = decode(*tile, nullptr, 0);
    return Mvt::encode(Mvt::overzoom(*decoded, dz, x & mask, y & mask));
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to get vector tile")
        .addParameter("dataset", theDataset)
        .addParameter("z", std::to_string(z))
        .addParameter("x", std::to_string(x))
        .addParameter("y", std::to_string(y));
  }
}

// ----------------------------------------------------------------------

ScenePtr Renderer::render(const MapOptions& theOptions,
                          const Fmi::SpatialReference& theCRS,
                          const Fmi::Box& theBox) const
{
  try
  {
    auto style = itsStyles.get(theOptions.style);
    auto info = itsTiles.info(theOptions.dataset);
    if (!info)
      throw Fmi::Exception(BCP, "OSM dataset is not available")
          .addParameter("dataset", theOptions.dataset);

    auto scene = std::make_shared<Scene>();
    scene->width = theBox.width();
    scene->height = theBox.height();

    // Attribution from the metadata
    {
      Json::Value meta;
      Json::CharReaderBuilder builder;
      std::string errors;
      std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
      const auto& m = info->metadata;
      if (!m.empty() && reader->parse(m.data(), m.data() + m.size(), &meta, &errors))
        scene->attribution = strip_tags(meta.get("attribution", "").asString());
      if (scene->attribution.empty())
        scene->attribution = "© OpenStreetMap";
    }

    const double W = static_cast<double>(scene->width);
    const double H = static_cast<double>(scene->height);
    if (W < 1 || H < 1)
      return scene;

    // Transformations from the image CRS

    const Fmi::CoordinateTransformation toLonLat(theCRS, Fmi::SpatialReference("WGS84"));
    std::optional<Fmi::CoordinateTransformation> toMercator;
    auto epsg = theCRS.getEPSG();
    if (!epsg || *epsg != 3857)
      toMercator.emplace(theCRS, Fmi::SpatialReference("EPSG:3857"));

    auto pixel_to_crs = [&](double& x, double& y) { theBox.itransform(x, y); };

    // 1. Style zoom from the resolution near the center of the image

    double zoom = 0;
    if (theOptions.zoom)
      zoom = *theOptions.zoom;
    else
    {
      std::optional<double> resolution;
      for (double f : {0.5, 0.25, 0.75, 0.1, 0.9})
      {
        for (double g : {0.5, 0.25, 0.75, 0.1, 0.9})
        {
          std::vector<double> x{W * f, W * f + 1, W * f};
          std::vector<double> y{H * g, H * g, H * g + 1};
          for (std::size_t i = 0; i < 3; i++)
            pixel_to_crs(x[i], y[i]);
          if (toMercator)
            toMercator->transform(x, y);
          const double d1 = std::hypot(x[1] - x[0], y[1] - y[0]);
          const double d2 = std::hypot(x[2] - x[0], y[2] - y[0]);
          if (std::isfinite(d1) && std::isfinite(d2) && d1 > 0 && d2 > 0)
          {
            resolution = std::sqrt(d1 * d2);
            break;
          }
        }
        if (resolution)
          break;
      }
      if (!resolution)
        return scene;
      zoom = std::log2(2 * kHalf / (kTileSize * *resolution)) + theOptions.zoomOffset;
    }
    zoom = std::clamp(zoom, 0.0, 24.0);
    scene->zoom = zoom;

    // 2. Tiles covering the image

    int tilez = std::clamp(static_cast<int>(std::floor(zoom)), 0, static_cast<int>(info->maxzoom));

    std::vector<double> lon;
    std::vector<double> lat;
    {
      const double step = 16;
      const double m = theOptions.margin;
      auto add = [&](double px, double py)
      {
        double x = px;
        double y = py;
        pixel_to_crs(x, y);
        lon.push_back(x);
        lat.push_back(y);
      };
      for (double py = -m; py < H + m + step; py += step)
        for (double px = -m; px < W + m + step; px += step)
          add(std::min(px, W + m), std::min(py, H + m));
      // Denser along the edges
      for (double p = -m; p <= W + m; p += 4)
      {
        add(p, -m);
        add(p, H + m);
      }
      for (double p = -m; p <= H + m; p += 4)
      {
        add(-m, p);
        add(W + m, p);
      }
      toLonLat.transform(lon, lat);
    }

    std::set<std::pair<uint32_t, uint32_t>> wanted;
    while (true)
    {
      wanted.clear();
      for (std::size_t i = 0; i < lon.size(); i++)
        if (std::isfinite(lon[i]) && std::isfinite(lat[i]))
          wanted.insert(lonlat_to_tile(lon[i], lat[i], tilez));
      if (static_cast<int>(wanted.size()) <= theOptions.maxTiles || tilez == 0)
        break;
      --tilez;
    }
    scene->tileZoom = tilez;

    // 3. Group the tiles by the most detailed data available

    using TileId = std::tuple<int, uint32_t, uint32_t>;
    std::map<TileId, std::vector<std::pair<uint32_t, uint32_t>>> groups;
    std::set<int> levels;
    for (const auto& [x, y] : wanted)
    {
      const int a = available_zoom(*info, tilez, x, y);
      if (a < 0)
        continue;
      const int d = tilez - a;
      groups[TileId{a, x >> d, y >> d}].emplace_back(x, y);
      levels.insert(a);
    }

    // Number of wanted tiles under each group tile, to detect mixed resolutions
    std::map<TileId, std::size_t> footprint;
    for (const auto& [x, y] : wanted)
      for (int a : levels)
        ++footprint[TileId{a, x >> (tilez - a), y >> (tilez - a)}];

    const auto& sourceLayers = style->sourceLayers();
    const std::size_t layerHash = boost::hash_range(sourceLayers.begin(), sourceLayers.end());

    std::vector<TileWork> works;
    for (const auto& [id, children] : groups)
    {
      const auto [a, gx, gy] = id;
      auto tile = itsTiles.getTile(theOptions.dataset, static_cast<uint8_t>(a), gx, gy);
      if (!tile)
        continue;

      TileWork work;
      work.tile = decode(*tile, &sourceLayers, layerHash);
      work.z = tile->z;
      work.x = tile->x;
      work.y = tile->y;

      // Clip areas in units of the returned tile
      auto add_clip = [&](int z, uint32_t x, uint32_t y)
      {
        const int d = z - work.z;
        const double n = std::ldexp(1.0, d);
        const double x0 = x - (static_cast<double>(work.x) * n);
        const double y0 = y - (static_cast<double>(work.y) * n);
        work.clips.push_back(Rect{x0 / n, y0 / n, (x0 + 1) / n, (y0 + 1) / n});
      };

      if (footprint[id] == children.size())
        add_clip(a, gx, gy);
      else
        for (const auto& [cx, cy] : children)
          add_clip(tilez, cx, cy);

      works.push_back(std::move(work));
    }
    scene->tiles = works.size();

    // 4. Render the layers

    const Projector projector(theCRS, theBox);
    const double pm = theOptions.margin + 4;
    const PathBuilder builder(
        projector, Rect{-pm, -pm, W + pm, H + pm}, theOptions.precision, !projector.isWebMercator());
    const Context zctx{zoom, nullptr, nullptr};

    std::vector<Label> labels;
    const auto& layers = style->layers();
    for (std::size_t li = 0; li < layers.size(); li++)
    {
      const auto& layer = layers[li];
      if (!layer.isVisibleAt(zoom))
        continue;
      if (!theOptions.include.empty() && !matches(layer.id, theOptions.include))
        continue;
      if (matches(layer.id, theOptions.exclude))
        continue;

      if (layer.type == LayerType::Background)
      {
        Property color(layer.paintProperty("background-color"), zctx, Value(std::string("#000000")));
        Property opacity(layer.paintProperty("background-opacity"), zctx, Value(1.0));
        SceneElement rect{"rect",
                          layer.id,
                          {{"x", "0"}, {"y", "0"}, {"width", number(W)}, {"height", number(H)}},
                          ""};
        if (add_color(rect.attributes, "fill", color(zctx), opacity.number(zctx)))
          scene->elements.push_back(std::move(rect));
        continue;
      }

      if (layer.type == LayerType::Symbol)
      {
        if (theOptions.labels)
          collect_labels(layer, static_cast<int>(li), works, zoom, builder, theOptions, labels);
        continue;
      }

      const bool fill = (layer.type == LayerType::Fill);
      Groups groups_out;

      if (fill)
      {
        Property color(layer.paintProperty("fill-color"), zctx, Value(std::string("#000000")));
        Property opacity(layer.paintProperty("fill-opacity"), zctx, Value(1.0));
        if (layer.paintProperty("fill-pattern") != nullptr)
          continue;

        for (const auto& work : works)
        {
          const auto* mvtlayer = work.tile->layer(layer.sourceLayer);
          if (mvtlayer == nullptr)
            continue;
          for (const auto& feature : mvtlayer->features)
          {
            if (feature.type != Mvt::GeomType::Polygon)
              continue;
            const Context ctx{zoom, mvtlayer, &feature};
            if (layer.filter && !layer.filter->evaluate(ctx).truthy())
              continue;
            std::vector<std::pair<std::string, std::string>> attrs;
            if (!add_color(attrs, "fill", color(ctx), opacity.number(ctx)))
              continue;
            std::string key;
            for (const auto& [n, v] : attrs)
              key += n + '=' + v + ';';
            auto& path = groups_out.path(key, attrs);
            builder.add(feature, work, mvtlayer->extent, true, path);
          }
        }
      }
      else
      {
        Property color(layer.paintProperty("line-color"), zctx, Value(std::string("#000000")));
        Property width(layer.paintProperty("line-width"), zctx, Value(1.0));
        Property gap(layer.paintProperty("line-gap-width"), zctx, Value(0.0));
        Property opacity(layer.paintProperty("line-opacity"), zctx, Value(1.0));
        Property dash(layer.paintProperty("line-dasharray"), zctx, Value());
        Property cap(layer.layoutProperty("line-cap"), zctx, Value(std::string("butt")));
        Property join(layer.layoutProperty("line-join"), zctx, Value(std::string("miter")));
        if (layer.paintProperty("line-pattern") != nullptr)
          continue;

        for (const auto& work : works)
        {
          const auto* mvtlayer = work.tile->layer(layer.sourceLayer);
          if (mvtlayer == nullptr)
            continue;
          for (const auto& feature : mvtlayer->features)
          {
            if (feature.type != Mvt::GeomType::LineString &&
                feature.type != Mvt::GeomType::Polygon)
              continue;
            const Context ctx{zoom, mvtlayer, &feature};
            if (layer.filter && !layer.filter->evaluate(ctx).truthy())
              continue;

            const double w = width.number(ctx);
            const double g = gap.number(ctx);
            const double total = (g > 0 ? g + 2 * w : w);
            if (total <= 0)
              continue;
            std::vector<std::pair<std::string, std::string>> attrs{{"fill", "none"}};
            if (!add_color(attrs, "stroke", color(ctx), opacity.number(ctx)))
              continue;
            attrs.emplace_back("stroke-width", number(total, 2));
            const auto c = cap(ctx).toString();
            if (c != "butt")
              attrs.emplace_back("stroke-linecap", c);
            const auto j = join(ctx).toString();
            if (j != "miter")
              attrs.emplace_back("stroke-linejoin", j == "bevel" ? "bevel" : "round");
            auto d = dash(ctx);
            if (d.type == Value::Type::Array && !d.array.empty())
            {
              std::string s;
              for (const auto& v : d.array)
                s += (s.empty() ? "" : ",") + number(v.toNumber().value_or(0) * std::max(w, 1.0), 2);
              attrs.emplace_back("stroke-dasharray", s);
            }

            std::string key;
            for (const auto& [n, v] : attrs)
              key += n + '=' + v + ';';
            auto& path = groups_out.path(key, attrs);
            builder.add(feature, work, mvtlayer->extent, false, path);
          }
        }
      }

      for (auto& [attrs, path] : groups_out.groups)
      {
        if (path.empty())
          continue;
        SceneElement e{"path", layer.id, {{"d", std::move(path)}}, ""};
        e.attributes.insert(e.attributes.end(), attrs.begin(), attrs.end());
        scene->elements.push_back(std::move(e));
      }
    }

    // 5. Labels on top

    place_labels(labels, *scene);

    return scene;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to render OSM map")
        .addParameter("dataset", theOptions.dataset)
        .addParameter("style", theOptions.style);
  }
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
