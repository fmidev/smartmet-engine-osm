// ======================================================================
/*!
 * \brief Lightweight Mapbox Vector Tile decoder and encoder
 */
// ======================================================================

#include "MvtTile.h"
#include "Clip.h"
#include <macgyver/Exception.h>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
namespace Mvt
{
namespace
{
// Protobuf wire types
constexpr int WireVarint = 0;
constexpr int WireFixed64 = 1;
constexpr int WireBytes = 2;
constexpr int WireFixed32 = 5;

// Geometry commands
constexpr uint32_t CmdMoveTo = 1;
constexpr uint32_t CmdLineTo = 2;
constexpr uint32_t CmdClosePath = 7;

// ----------------------------------------------------------------------
/*!
 * \brief Minimal protobuf wire format reader
 */
// ----------------------------------------------------------------------

class Reader
{
 public:
  explicit Reader(std::string_view data) : p(data.data()), end(data.data() + data.size()) {}

  bool next()
  {
    if (p >= end)
      return false;
    const auto key = varint();
    field = static_cast<uint32_t>(key >> 3);
    wiretype = static_cast<int>(key & 7);
    return true;
  }

  uint32_t tag() const { return field; }
  int type() const { return wiretype; }

  uint64_t varint()
  {
    uint64_t result = 0;
    int shift = 0;
    while (p < end)
    {
      const auto byte = static_cast<uint8_t>(*p++);
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0)
        return result;
      shift += 7;
      if (shift >= 64)
        break;
    }
    throw Fmi::Exception(BCP, "Invalid varint in vector tile");
  }

  std::string_view bytes()
  {
    const auto len = varint();
    if (len > static_cast<uint64_t>(end - p))
      throw Fmi::Exception(BCP, "Truncated vector tile");
    std::string_view ret(p, len);
    p += len;
    return ret;
  }

  uint32_t fixed32()
  {
    if (end - p < 4)
      throw Fmi::Exception(BCP, "Truncated vector tile");
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }

  uint64_t fixed64()
  {
    if (end - p < 8)
      throw Fmi::Exception(BCP, "Truncated vector tile");
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    p += 8;
    return v;
  }

  void skip()
  {
    switch (wiretype)
    {
      case WireVarint:
        varint();
        break;
      case WireFixed64:
        fixed64();
        break;
      case WireBytes:
        bytes();
        break;
      case WireFixed32:
        fixed32();
        break;
      default:
        throw Fmi::Exception(BCP, "Unsupported protobuf wire type in vector tile");
    }
  }

 private:
  const char* p;
  const char* end;
  uint32_t field = 0;
  int wiretype = 0;
};

int64_t zigzag(uint64_t v)
{
  return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

uint32_t unzigzag(int64_t v)
{
  return static_cast<uint32_t>((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
}

// Read a packed repeated uint32 field
void packed(std::string_view data, std::vector<uint32_t>& out)
{
  const char* p = data.data();
  const char* end = p + data.size();
  while (p < end)
  {
    uint64_t result = 0;
    int shift = 0;
    while (true)
    {
      if (p >= end)
        throw Fmi::Exception(BCP, "Truncated packed field in vector tile");
      const auto byte = static_cast<uint8_t>(*p++);
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0)
        break;
      shift += 7;
      if (shift >= 64)
        throw Fmi::Exception(BCP, "Invalid varint in vector tile");
    }
    out.push_back(static_cast<uint32_t>(result));
  }
}

PropertyValue decodeValue(std::string_view data)
{
  Reader r(data);
  PropertyValue value;
  while (r.next())
  {
    switch (r.tag())
    {
      case 1:
        value = std::string(r.bytes());
        break;
      case 2:
      {
        auto bits = r.fixed32();
        float f = 0;
        std::memcpy(&f, &bits, 4);
        value = static_cast<double>(f);
        break;
      }
      case 3:
      {
        auto bits = r.fixed64();
        double d = 0;
        std::memcpy(&d, &bits, 8);
        value = d;
        break;
      }
      case 4:
        value = static_cast<double>(static_cast<int64_t>(r.varint()));
        break;
      case 5:
        value = static_cast<double>(r.varint());
        break;
      case 6:
        value = static_cast<double>(zigzag(r.varint()));
        break;
      case 7:
        value = (r.varint() != 0);
        break;
      default:
        r.skip();
    }
  }
  return value;
}

void decodeGeometry(const std::vector<uint32_t>& cmds, Feature& feature)
{
  int64_t x = 0;
  int64_t y = 0;
  std::size_t i = 0;
  const std::size_t n = cmds.size();

  while (i < n)
  {
    const uint32_t cmd = cmds[i] & 7;
    const uint32_t count = cmds[i] >> 3;
    ++i;

    if (cmd == CmdMoveTo || cmd == CmdLineTo)
    {
      if (count > (n - i) / 2)
        throw Fmi::Exception(BCP, "Truncated geometry in vector tile");
      for (uint32_t j = 0; j < count; j++)
      {
        x += zigzag(cmds[i++]);
        y += zigzag(cmds[i++]);
        if (cmd == CmdMoveTo)
          feature.parts.emplace_back();
        else if (feature.parts.empty())
          throw Fmi::Exception(BCP, "LineTo without MoveTo in vector tile");
        feature.parts.back().push_back(Point{static_cast<double>(x), static_cast<double>(y)});
      }
    }
    else if (cmd == CmdClosePath)
    {
      // Rings are implicitly closed
    }
    else
      throw Fmi::Exception(BCP, "Unknown geometry command in vector tile");
  }
}

Feature decodeFeature(std::string_view data)
{
  Feature feature;
  std::vector<uint32_t> geometry;
  Reader r(data);
  while (r.next())
  {
    switch (r.tag())
    {
      case 2:
        if (r.type() == WireBytes)
          packed(r.bytes(), feature.tags);
        else
          feature.tags.push_back(static_cast<uint32_t>(r.varint()));
        break;
      case 3:
        feature.type = static_cast<GeomType>(r.varint());
        break;
      case 4:
        if (r.type() == WireBytes)
          packed(r.bytes(), geometry);
        else
          geometry.push_back(static_cast<uint32_t>(r.varint()));
        break;
      default:
        r.skip();
    }
  }
  decodeGeometry(geometry, feature);
  return feature;
}

std::optional<Layer> decodeLayer(std::string_view data, const std::set<std::string>* wanted)
{
  // The name may come after the features, so collect the fields first
  Layer layer;
  std::vector<std::string_view> features;
  std::vector<std::string_view> values;

  Reader r(data);
  while (r.next())
  {
    switch (r.tag())
    {
      case 1:
        layer.name = std::string(r.bytes());
        break;
      case 2:
        features.push_back(r.bytes());
        break;
      case 3:
        layer.keys.emplace_back(r.bytes());
        break;
      case 4:
        values.push_back(r.bytes());
        break;
      case 5:
        layer.extent = static_cast<uint32_t>(r.varint());
        break;
      default:
        r.skip();
    }
  }

  if (wanted && wanted->count(layer.name) == 0)
    return {};

  if (layer.extent == 0)
    layer.extent = 4096;

  layer.values.reserve(values.size());
  for (const auto& v : values)
    layer.values.push_back(decodeValue(v));

  layer.features.reserve(features.size());
  for (const auto& f : features)
  {
    auto feature = decodeFeature(f);
    if (!feature.parts.empty())
      layer.features.push_back(std::move(feature));
  }
  return layer;
}

// ----------------------------------------------------------------------
/*!
 * \brief Minimal protobuf wire format writer
 */
// ----------------------------------------------------------------------

class Writer
{
 public:
  void varint(uint64_t v)
  {
    while (v >= 0x80)
    {
      out.push_back(static_cast<char>((v & 0x7F) | 0x80));
      v >>= 7;
    }
    out.push_back(static_cast<char>(v));
  }

  void key(uint32_t field, int wiretype) { varint((static_cast<uint64_t>(field) << 3) | wiretype); }

  void bytes(uint32_t field, std::string_view data)
  {
    key(field, WireBytes);
    varint(data.size());
    out.append(data);
  }

  void uint(uint32_t field, uint64_t v)
  {
    key(field, WireVarint);
    varint(v);
  }

  void fixed64(uint32_t field, uint64_t v)
  {
    key(field, WireFixed64);
    char buf[8];
    std::memcpy(buf, &v, 8);
    out.append(buf, 8);
  }

  void packed(uint32_t field, const std::vector<uint32_t>& values)
  {
    Writer w;
    for (auto v : values)
      w.varint(v);
    bytes(field, w.out);
  }

  std::string out;
};

std::string encodeValue(const PropertyValue& value)
{
  Writer w;
  if (const auto* s = std::get_if<std::string>(&value))
    w.bytes(1, *s);
  else if (const auto* d = std::get_if<double>(&value))
  {
    if (std::floor(*d) == *d && std::abs(*d) < 9e15)
    {
      const auto i = static_cast<int64_t>(*d);
      w.key(6, WireVarint);
      w.varint((static_cast<uint64_t>(i) << 1) ^ static_cast<uint64_t>(i >> 63));
    }
    else
    {
      uint64_t bits = 0;
      std::memcpy(&bits, d, 8);
      w.fixed64(3, bits);
    }
  }
  else if (const auto* b = std::get_if<bool>(&value))
    w.uint(7, *b ? 1 : 0);
  return w.out;
}

std::vector<uint32_t> encodeGeometry(const Feature& feature)
{
  std::vector<uint32_t> cmds;
  int64_t cx = 0;
  int64_t cy = 0;
  const bool polygon = (feature.type == GeomType::Polygon);

  if (feature.type == GeomType::Point)
  {
    cmds.push_back(CmdMoveTo | (static_cast<uint32_t>(feature.parts.size()) << 3));
    for (const auto& part : feature.parts)
    {
      const auto x = std::llround(part.front().x);
      const auto y = std::llround(part.front().y);
      cmds.push_back(unzigzag(x - cx));
      cmds.push_back(unzigzag(y - cy));
      cx = x;
      cy = y;
    }
    return cmds;
  }

  for (const auto& part : feature.parts)
  {
    // Round and remove repeated points
    std::vector<std::pair<int64_t, int64_t>> pts;
    pts.reserve(part.size());
    for (const auto& p : part)
    {
      std::pair<int64_t, int64_t> ip{std::llround(p.x), std::llround(p.y)};
      if (pts.empty() || pts.back() != ip)
        pts.push_back(ip);
    }
    if (polygon && pts.size() > 1 && pts.front() == pts.back())
      pts.pop_back();
    if (pts.size() < (polygon ? 3U : 2U))
      continue;

    cmds.push_back(CmdMoveTo | (1 << 3));
    cmds.push_back(unzigzag(pts[0].first - cx));
    cmds.push_back(unzigzag(pts[0].second - cy));
    cx = pts[0].first;
    cy = pts[0].second;
    cmds.push_back(CmdLineTo | (static_cast<uint32_t>(pts.size() - 1) << 3));
    for (std::size_t i = 1; i < pts.size(); i++)
    {
      cmds.push_back(unzigzag(pts[i].first - cx));
      cmds.push_back(unzigzag(pts[i].second - cy));
      cx = pts[i].first;
      cy = pts[i].second;
    }
    if (polygon)
      cmds.push_back(CmdClosePath | (1 << 3));
  }
  return cmds;
}

}  // namespace

// ----------------------------------------------------------------------

const PropertyValue* Layer::get(const Feature& feature, std::string_view key) const
{
  const auto n = feature.tags.size() & ~std::size_t{1};
  for (std::size_t i = 0; i < n; i += 2)
  {
    const auto k = feature.tags[i];
    const auto v = feature.tags[i + 1];
    if (k < keys.size() && v < values.size() && keys[k] == key)
      return &values[v];
  }
  return nullptr;
}

// ----------------------------------------------------------------------

const Layer* Tile::layer(std::string_view name) const
{
  for (const auto& l : layers)
    if (l.name == name)
      return &l;
  return nullptr;
}

// ----------------------------------------------------------------------

std::size_t Tile::memoryUse() const
{
  std::size_t total = sizeof(Tile);
  for (const auto& l : layers)
  {
    total += sizeof(Layer) + l.name.size();
    for (const auto& k : l.keys)
      total += sizeof(std::string) + k.size();
    total += l.values.size() * (sizeof(PropertyValue) + 8);
    for (const auto& f : l.features)
    {
      total += sizeof(Feature) + f.tags.size() * sizeof(uint32_t);
      for (const auto& p : f.parts)
        total += sizeof(Part) + p.size() * sizeof(Point);
    }
  }
  return total;
}

// ----------------------------------------------------------------------

Tile decode(std::string_view data, const std::set<std::string>* wanted)
{
  try
  {
    Tile tile;
    Reader r(data);
    while (r.next())
    {
      if (r.tag() == 3 && r.type() == WireBytes)
      {
        auto layer = decodeLayer(r.bytes(), wanted);
        if (layer)
          tile.layers.push_back(std::move(*layer));
      }
      else
        r.skip();
    }
    return tile;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to decode vector tile");
  }
}

// ----------------------------------------------------------------------

std::string encode(const Tile& tile)
{
  try
  {
    Writer out;
    for (const auto& layer : tile.layers)
    {
      Writer w;
      w.uint(15, 2);
      w.bytes(1, layer.name);
      for (const auto& feature : layer.features)
      {
        auto geometry = encodeGeometry(feature);
        if (geometry.empty())
          continue;
        Writer f;
        if (!feature.tags.empty())
          f.packed(2, feature.tags);
        f.uint(3, static_cast<uint64_t>(feature.type));
        f.packed(4, geometry);
        w.bytes(2, f.out);
      }
      for (const auto& k : layer.keys)
        w.bytes(3, k);
      for (const auto& v : layer.values)
        w.bytes(4, encodeValue(v));
      w.uint(5, layer.extent);
      out.bytes(3, w.out);
    }
    return out.out;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to encode vector tile");
  }
}

// ----------------------------------------------------------------------

Tile overzoom(const Tile& tile, int dz, uint32_t dx, uint32_t dy, double buffer)
{
  try
  {
    if (dz <= 0)
      return tile;

    const double scale = std::ldexp(1.0, dz);
    Tile result;
    for (const auto& layer : tile.layers)
    {
      Layer out;
      out.name = layer.name;
      out.extent = layer.extent;
      out.keys = layer.keys;
      out.values = layer.values;

      const double ox = static_cast<double>(dx) * layer.extent;
      const double oy = static_cast<double>(dy) * layer.extent;
      const Rect rect{-buffer, -buffer, layer.extent + buffer, layer.extent + buffer};

      for (const auto& feature : layer.features)
      {
        Feature f;
        f.type = feature.type;
        f.tags = feature.tags;

        for (const auto& part : feature.parts)
        {
          Part scaled;
          scaled.reserve(part.size());
          for (const auto& p : part)
            scaled.push_back(Point{p.x * scale - ox, p.y * scale - oy});

          if (feature.type == GeomType::Point)
          {
            if (rect.contains(scaled.front()))
              f.parts.push_back(std::move(scaled));
          }
          else if (feature.type == GeomType::LineString)
            clipLine(scaled, rect, f.parts);
          else if (feature.type == GeomType::Polygon)
          {
            auto ring = clipRing(scaled, rect);
            if (ring.size() >= 3)
              f.parts.push_back(std::move(ring));
          }
        }
        if (!f.parts.empty())
          out.features.push_back(std::move(f));
      }
      if (!out.features.empty())
        result.layers.push_back(std::move(out));
    }
    return result;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to overzoom vector tile");
  }
}

}  // namespace Mvt
}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
