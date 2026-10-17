// ======================================================================
/*!
 * \brief Fast rectangle clipping of rings and polylines
 */
// ======================================================================

#include "Clip.h"

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace
{
enum class Edge
{
  Left,
  Right,
  Bottom,
  Top
};

bool isInside(const Point& p, Edge edge, const Rect& r)
{
  switch (edge)
  {
    case Edge::Left:
      return p.x >= r.xmin;
    case Edge::Right:
      return p.x <= r.xmax;
    case Edge::Bottom:
      return p.y >= r.ymin;
    case Edge::Top:
      return p.y <= r.ymax;
  }
  return false;
}

Point intersect(const Point& a, const Point& b, Edge edge, const Rect& r)
{
  switch (edge)
  {
    case Edge::Left:
    {
      const double t = (r.xmin - a.x) / (b.x - a.x);
      return {r.xmin, a.y + t * (b.y - a.y)};
    }
    case Edge::Right:
    {
      const double t = (r.xmax - a.x) / (b.x - a.x);
      return {r.xmax, a.y + t * (b.y - a.y)};
    }
    case Edge::Bottom:
    {
      const double t = (r.ymin - a.y) / (b.y - a.y);
      return {a.x + t * (b.x - a.x), r.ymin};
    }
    case Edge::Top:
    {
      const double t = (r.ymax - a.y) / (b.y - a.y);
      return {a.x + t * (b.x - a.x), r.ymax};
    }
  }
  return a;
}

void clipEdge(const Part& input, Part& output, Edge edge, const Rect& r)
{
  output.clear();
  if (input.empty())
    return;
  Point prev = input.back();
  bool prevInside = isInside(prev, edge, r);
  for (const auto& cur : input)
  {
    const bool curInside = isInside(cur, edge, r);
    if (curInside)
    {
      if (!prevInside)
        output.push_back(intersect(prev, cur, edge, r));
      output.push_back(cur);
    }
    else if (prevInside)
      output.push_back(intersect(prev, cur, edge, r));
    prev = cur;
    prevInside = curInside;
  }
}

// Cohen-Sutherland outcodes
constexpr int CodeLeft = 1;
constexpr int CodeRight = 2;
constexpr int CodeBottom = 4;
constexpr int CodeTop = 8;

int outcode(const Point& p, const Rect& r)
{
  int code = 0;
  if (p.x < r.xmin)
    code |= CodeLeft;
  else if (p.x > r.xmax)
    code |= CodeRight;
  if (p.y < r.ymin)
    code |= CodeBottom;
  else if (p.y > r.ymax)
    code |= CodeTop;
  return code;
}

// Clip a segment, returns false if it is completely outside
bool clipSegment(Point& a, Point& b, const Rect& r)
{
  int ca = outcode(a, r);
  int cb = outcode(b, r);
  while (true)
  {
    if ((ca | cb) == 0)
      return true;
    if ((ca & cb) != 0)
      return false;
    const int code = (ca != 0 ? ca : cb);
    Point p{};
    if ((code & CodeTop) != 0)
      p = {a.x + (b.x - a.x) * (r.ymax - a.y) / (b.y - a.y), r.ymax};
    else if ((code & CodeBottom) != 0)
      p = {a.x + (b.x - a.x) * (r.ymin - a.y) / (b.y - a.y), r.ymin};
    else if ((code & CodeRight) != 0)
      p = {r.xmax, a.y + (b.y - a.y) * (r.xmax - a.x) / (b.x - a.x)};
    else
      p = {r.xmin, a.y + (b.y - a.y) * (r.xmin - a.x) / (b.x - a.x)};
    if (code == ca)
    {
      a = p;
      ca = outcode(a, r);
    }
    else
    {
      b = p;
      cb = outcode(b, r);
    }
  }
}

}  // namespace

// ----------------------------------------------------------------------

bool outside(const Part& part, const Rect& rect)
{
  if (part.empty())
    return true;
  double xmin = part[0].x;
  double xmax = xmin;
  double ymin = part[0].y;
  double ymax = ymin;
  for (const auto& p : part)
  {
    xmin = std::min(xmin, p.x);
    xmax = std::max(xmax, p.x);
    ymin = std::min(ymin, p.y);
    ymax = std::max(ymax, p.y);
  }
  return xmax < rect.xmin || xmin > rect.xmax || ymax < rect.ymin || ymin > rect.ymax;
}

bool inside(const Part& part, const Rect& rect)
{
  for (const auto& p : part)
    if (!rect.contains(p))
      return false;
  return true;
}

// ----------------------------------------------------------------------

Part clipRing(const Part& ring, const Rect& rect)
{
  if (inside(ring, rect))
    return ring;
  if (outside(ring, rect))
    return {};

  Part a = ring;
  Part b;
  b.reserve(ring.size() + 8);
  clipEdge(a, b, Edge::Left, rect);
  clipEdge(b, a, Edge::Right, rect);
  clipEdge(a, b, Edge::Bottom, rect);
  clipEdge(b, a, Edge::Top, rect);
  if (a.size() < 3)
    return {};
  return a;
}

// ----------------------------------------------------------------------

void clipLine(const Part& line, const Rect& rect, std::vector<Part>& output)
{
  if (line.size() < 2)
    return;
  if (inside(line, rect))
  {
    output.push_back(line);
    return;
  }
  if (outside(line, rect))
    return;

  Part current;
  for (std::size_t i = 1; i < line.size(); i++)
  {
    Point a = line[i - 1];
    Point b = line[i];
    const bool aInside = rect.contains(a);
    if (!clipSegment(a, b, rect))
    {
      if (current.size() >= 2)
        output.push_back(std::move(current));
      current.clear();
      continue;
    }
    if (current.empty() || !aInside)
    {
      if (current.size() >= 2)
        output.push_back(std::move(current));
      current.clear();
      current.push_back(a);
    }
    current.push_back(b);
    // Leaving the rectangle ends the current piece
    if (!rect.contains(line[i]))
    {
      if (current.size() >= 2)
        output.push_back(std::move(current));
      current.clear();
    }
  }
  if (current.size() >= 2)
    output.push_back(std::move(current));
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
