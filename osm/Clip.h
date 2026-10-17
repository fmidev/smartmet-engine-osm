// ======================================================================
/*!
 * \brief Fast rectangle clipping of rings and polylines
 *
 * Polygon rings are clipped with the Sutherland-Hodgman algorithm, which
 * keeps the ring orientation and may produce degenerate edges along the
 * rectangle. That is fine for filling but not for stroking the outline.
 */
// ======================================================================

#pragma once

#include "MvtTile.h"
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
using Mvt::Part;
using Mvt::Point;

struct Rect
{
  double xmin;
  double ymin;
  double xmax;
  double ymax;

  bool contains(const Point& p) const
  {
    return p.x >= xmin && p.x <= xmax && p.y >= ymin && p.y <= ymax;
  }
};

// Clip a closed ring, returns an empty part if nothing remains
Part clipRing(const Part& ring, const Rect& rect);

// Clip a polyline into zero or more polylines, appended to the output
void clipLine(const Part& line, const Rect& rect, std::vector<Part>& output);

// Bounding box test helpers
bool outside(const Part& part, const Rect& rect);
bool inside(const Part& part, const Rect& rect);

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
