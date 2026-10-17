// ======================================================================
/*!
 * \brief MapLibre style expressions
 */
// ======================================================================

#include "Expression.h"
#include <fmt/format.h>
#include <macgyver/Exception.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
enum class Expression::Op
{
  Literal,
  Get,
  Has,
  NotHas,
  Not,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  All,
  Any,
  None,
  In,        // ["in", needle, haystack]
  LegacyIn,  // ["in", key, v1, v2, ...] with the values in itsLabels[0]
  LegacyNotIn,
  Case,
  Match,
  Coalesce,
  Interpolate,
  Step,
  Zoom,
  GeometryType,
  Concat,
  ToString,
  ToNumber,
  ToBoolean,
  Assert,  // number, string, boolean, array etc: first non-null argument
  Length,
  Plus,
  Minus,
  Times,
  Divide,
  Mod,
  Pow,
  Min,
  Max,
  Abs,
  Round,
  Floor,
  Ceil,
  Sqrt,
  Log10,
  Ln,
  Log2,
  Upcase,
  Downcase,
  Rgba
};

using Op = Expression::Op;

namespace
{
// Report each unsupported construct only once
void warn(const std::string& theMessage)
{
  static std::mutex mutex;
  static std::set<std::string> reported;
  std::lock_guard<std::mutex> lock(mutex);
  if (reported.insert(theMessage).second)
    std::cerr << fmt::format("OSM engine: {}\n", theMessage);
}

const std::map<std::string, std::string> named_colors = {
    {"black", "#000000"},  {"white", "#ffffff"},   {"red", "#ff0000"},
    {"green", "#008000"},  {"blue", "#0000ff"},    {"yellow", "#ffff00"},
    {"cyan", "#00ffff"},   {"magenta", "#ff00ff"}, {"gray", "#808080"},
    {"grey", "#808080"},   {"silver", "#c0c0c0"},  {"orange", "#ffa500"},
    {"brown", "#a52a2a"},  {"purple", "#800080"},  {"pink", "#ffc0cb"},
    {"navy", "#000080"},   {"lime", "#00ff00"},    {"olive", "#808000"},
    {"teal", "#008080"},   {"maroon", "#800000"},  {"aqua", "#00ffff"},
    {"fuchsia", "#ff00ff"}};

double hue2rgb(double p, double q, double t)
{
  if (t < 0)
    t += 1;
  if (t > 1)
    t -= 1;
  if (t < 1.0 / 6)
    return p + (q - p) * 6 * t;
  if (t < 1.0 / 2)
    return q;
  if (t < 2.0 / 3)
    return p + (q - p) * (2.0 / 3 - t) * 6;
  return p;
}

// Parse the comma separated numbers of rgb(), hsl() etc
std::vector<double> parse_numbers(const std::string& s, std::vector<bool>& percent)
{
  std::vector<double> ret;
  std::string token;
  auto flush = [&]()
  {
    if (token.empty())
      return;
    bool pct = (token.back() == '%');
    if (pct)
      token.pop_back();
    ret.push_back(std::stod(token));
    percent.push_back(pct);
    token.clear();
  };
  for (char c : s)
  {
    if (c == ',' || c == ' ' || c == '/')
      flush();
    else
      token += c;
  }
  flush();
  return ret;
}

std::string format_number(double d)
{
  if (std::floor(d) == d && std::abs(d) < 1e15)
    return fmt::format("{}", static_cast<long long>(d));
  return fmt::format("{}", d);
}

Value mix(const Value& a, const Value& b, double t)
{
  if (a.type == Value::Type::Number && b.type == Value::Type::Number)
    return Value(a.number + t * (b.number - a.number));

  if (a.type == Value::Type::String && b.type == Value::Type::String)
  {
    auto ca = parseColor(a.string);
    auto cb = parseColor(b.string);
    if (ca && cb)
    {
      Color c;
      c.r = ca->r + t * (cb->r - ca->r);
      c.g = ca->g + t * (cb->g - ca->g);
      c.b = ca->b + t * (cb->b - ca->b);
      c.a = ca->a + t * (cb->a - ca->a);
      return Value(fmt::format("rgba({},{},{},{:.4f})",
                               std::lround(c.r * 255),
                               std::lround(c.g * 255),
                               std::lround(c.b * 255),
                               c.a));
    }
  }

  if (a.type == Value::Type::Array && b.type == Value::Type::Array &&
      a.array.size() == b.array.size())
  {
    std::vector<Value> out;
    for (std::size_t i = 0; i < a.array.size(); i++)
      out.push_back(mix(a.array[i], b.array[i], t));
    return Value(std::move(out));
  }

  // Not interpolatable
  return t < 1 ? a : b;
}

bool is_interpolatable(const Json::Value& v)
{
  if (v.isNumeric())
    return true;
  if (v.isString())
    return parseColor(v.asString()).has_value();
  if (v.isArray())
  {
    for (const auto& e : v)
      if (!e.isNumeric())
        return false;
    return true;
  }
  return false;
}

// Expression constructors

Expression make(Op op, std::vector<Expression> args = {})
{
  Expression e;
  e.itsOp = op;
  e.itsArgs = std::move(args);
  for (const auto& a : e.itsArgs)
  {
    e.itsDataDependent |= a.itsDataDependent;
    e.itsZoomDependent |= a.itsZoomDependent;
  }
  return e;
}

Expression literal(Value v)
{
  Expression e;
  e.itsOp = Op::Literal;
  e.itsValue = std::move(v);
  return e;
}

Expression get(const std::string& key)
{
  Expression e;
  e.itsOp = Op::Get;
  e.itsKey = key;
  e.itsDataDependent = true;
  return e;
}

Expression has(const std::string& key, bool negate)
{
  Expression e;
  e.itsOp = (negate ? Op::NotHas : Op::Has);
  e.itsKey = key;
  e.itsDataDependent = true;
  return e;
}

Expression geometry_type()
{
  Expression e;
  e.itsOp = Op::GeometryType;
  e.itsDataDependent = true;
  return e;
}

Expression zoom()
{
  Expression e;
  e.itsOp = Op::Zoom;
  e.itsZoomDependent = true;
  return e;
}

const std::map<std::string, Op> simple_ops = {{"!", Op::Not},
                                              {"==", Op::Eq},
                                              {"!=", Op::Ne},
                                              {"<", Op::Lt},
                                              {"<=", Op::Le},
                                              {">", Op::Gt},
                                              {">=", Op::Ge},
                                              {"all", Op::All},
                                              {"any", Op::Any},
                                              {"none", Op::None},
                                              {"in", Op::In},
                                              {"case", Op::Case},
                                              {"coalesce", Op::Coalesce},
                                              {"concat", Op::Concat},
                                              {"to-string", Op::ToString},
                                              {"to-number", Op::ToNumber},
                                              {"to-boolean", Op::ToBoolean},
                                              {"to-color", Op::Assert},
                                              {"number", Op::Assert},
                                              {"string", Op::Assert},
                                              {"boolean", Op::Assert},
                                              {"array", Op::Assert},
                                              {"object", Op::Assert},
                                              {"length", Op::Length},
                                              {"+", Op::Plus},
                                              {"-", Op::Minus},
                                              {"*", Op::Times},
                                              {"/", Op::Divide},
                                              {"%", Op::Mod},
                                              {"^", Op::Pow},
                                              {"min", Op::Min},
                                              {"max", Op::Max},
                                              {"abs", Op::Abs},
                                              {"round", Op::Round},
                                              {"floor", Op::Floor},
                                              {"ceil", Op::Ceil},
                                              {"sqrt", Op::Sqrt},
                                              {"log10", Op::Log10},
                                              {"ln", Op::Ln},
                                              {"log2", Op::Log2},
                                              {"upcase", Op::Upcase},
                                              {"downcase", Op::Downcase},
                                              {"rgb", Op::Rgba},
                                              {"rgba", Op::Rgba}};

using Scope = std::map<std::string, Expression>;

Expression compile_expression(const Json::Value& json, const Scope& scope);

const std::set<std::string> special_ops = {"literal",
                                           "get",
                                           "has",
                                           "!has",
                                           "zoom",
                                           "geometry-type",
                                           "is-supported-script",
                                           "resolved-locale",
                                           "id",
                                           "feature-state",
                                           "collator",
                                           "line-progress",
                                           "var",
                                           "let",
                                           "format",
                                           "match",
                                           "interpolate",
                                           "interpolate-hcl",
                                           "interpolate-lab",
                                           "step",
                                           "image",
                                           "number-format",
                                           "within",
                                           "distance",
                                           "slice",
                                           "index-of",
                                           "at",
                                           "typeof",
                                           "properties",
                                           "accumulated",
                                           "heatmap-density",
                                           "global-state",
                                           "elevation"};

// Arrays starting with an operator name are expressions, others are literal
// arrays such as ["Noto Sans Regular"] in text-font
bool is_expression(const Json::Value& json)
{
  if (!json.isArray() || json.empty() || !json[0].isString())
    return false;
  const auto name = json[0].asString();
  return special_ops.count(name) > 0 || simple_ops.count(name) > 0;
}

// Compile a value which may be a literal, a literal array or an expression
Expression compile_value(const Json::Value& json, const Scope& scope)
{
  if (is_expression(json))
    return compile_expression(json, scope);
  return literal(Value::fromJson(json));
}

Expression compile_expression(const Json::Value& json, const Scope& scope)
{
  const std::string name = json[0].asString();
  const auto n = json.size();

  if (name == "literal")
    return literal(n > 1 ? Value::fromJson(json[1]) : Value());

  if (name == "get" || name == "has" || name == "!has")
  {
    if (n == 2 && json[1].isString())
    {
      if (name == "get")
        return get(json[1].asString());
      return has(json[1].asString(), name == "!has");
    }
    warn(fmt::format("unsupported form of '{}'", name));
    return literal(Value());
  }

  if (name == "zoom")
    return zoom();
  if (name == "geometry-type")
    return geometry_type();
  if (name == "is-supported-script")
    return literal(Value(true));
  if (name == "resolved-locale")
    return literal(Value(std::string("en")));
  if (name == "id" || name == "feature-state" || name == "collator")
    return literal(Value());
  if (name == "line-progress")
    return literal(Value(0.0));

  if (name == "var")
  {
    auto it = scope.find(json[1].asString());
    if (it != scope.end())
      return it->second;
    warn(fmt::format("unknown variable '{}'", json[1].asString()));
    return literal(Value());
  }

  if (name == "let")
  {
    Scope inner = scope;
    for (Json::ArrayIndex i = 1; i + 1 < n; i += 2)
      inner[json[i].asString()] = compile_value(json[i + 1], scope);
    return compile_value(json[n - 1], inner);
  }

  if (name == "format")
  {
    // Text sections and their formatting options alternate
    std::vector<Expression> args;
    for (Json::ArrayIndex i = 1; i < n; i++)
    {
      if (json[i].isObject())
        continue;
      args.push_back(compile_value(json[i], scope));
    }
    return make(Op::Concat, std::move(args));
  }

  if (name == "match")
  {
    // ["match", input, label1, output1, label2, output2, ..., fallback]
    std::vector<Expression> args{compile_value(json[1], scope)};
    Expression e;
    for (Json::ArrayIndex i = 2; i + 1 < n; i += 2)
    {
      std::vector<Value> labels;
      if (json[i].isArray())
        for (const auto& label : json[i])
          labels.push_back(Value::fromJson(label));
      else
        labels.push_back(Value::fromJson(json[i]));
      e.itsLabels.push_back(std::move(labels));
      args.push_back(compile_value(json[i + 1], scope));
    }
    args.push_back(compile_value(json[n - 1], scope));
    auto ret = make(Op::Match, std::move(args));
    ret.itsLabels = std::move(e.itsLabels);
    return ret;
  }

  if (name == "interpolate" || name == "interpolate-hcl" || name == "interpolate-lab")
  {
    // ["interpolate", ["linear"|"exponential", base|"cubic-bezier",...], input, stop, out, ...]
    double base = 1;
    const auto& type = json[1];
    if (type.isArray() && type.size() >= 2 && type[0].asString() == "exponential")
      base = type[1].asDouble();
    std::vector<Expression> args{compile_value(json[2], scope)};
    std::vector<double> stops;
    for (Json::ArrayIndex i = 3; i + 1 < n; i += 2)
    {
      stops.push_back(json[i].asDouble());
      args.push_back(compile_value(json[i + 1], scope));
    }
    auto ret = make(Op::Interpolate, std::move(args));
    ret.itsStops = std::move(stops);
    ret.itsBase = base;
    return ret;
  }

  if (name == "step")
  {
    // ["step", input, output0, stop1, output1, ...]
    std::vector<Expression> args{compile_value(json[1], scope), compile_value(json[2], scope)};
    std::vector<double> stops;
    for (Json::ArrayIndex i = 3; i + 1 < n; i += 2)
    {
      stops.push_back(json[i].asDouble());
      args.push_back(compile_value(json[i + 1], scope));
    }
    auto ret = make(Op::Step, std::move(args));
    ret.itsStops = std::move(stops);
    return ret;
  }

  auto it = simple_ops.find(name);
  if (it == simple_ops.end())
  {
    warn(fmt::format("unsupported style expression '{}'", name));
    return literal(Value());
  }

  std::vector<Expression> args;
  for (Json::ArrayIndex i = 1; i < n; i++)
    args.push_back(compile_value(json[i], scope));
  return make(it->second, std::move(args));
}

// ----------------------------------------------------------------------
/*!
 * \brief Is the filter an expression or in the legacy filter syntax?
 *
 * Follows isExpressionFilter of the MapLibre style specification.
 */
// ----------------------------------------------------------------------

bool is_expression_filter(const Json::Value& filter)
{
  if (filter.isBool())
    return true;
  if (!filter.isArray() || filter.empty() || !filter[0].isString())
    return false;

  const std::string op = filter[0].asString();
  const auto n = filter.size();

  if (op == "has")
    return n >= 2 && filter[1].asString() != "$id" && filter[1].asString() != "$type";
  if (op == "in")
    return n >= 3 && (!filter[1].isString() || filter[2].isArray());
  if (op == "!in" || op == "!has" || op == "none")
    return false;
  if (op == "==" || op == "!=" || op == ">" || op == ">=" || op == "<" || op == "<=")
    return n != 3 || filter[1].isArray() || filter[2].isArray();
  if (op == "any" || op == "all")
  {
    for (Json::ArrayIndex i = 1; i < n; i++)
      if (!is_expression_filter(filter[i]) && !filter[i].isBool())
        return false;
    return true;
  }
  return true;
}

Expression legacy_key(const std::string& key)
{
  if (key == "$type")
    return geometry_type();
  if (key == "$id")
    return literal(Value());
  return get(key);
}

Expression compile_legacy_filter(const Json::Value& filter)
{
  if (!filter.isArray() || filter.empty())
    return literal(Value(true));

  const std::string op = filter[0].asString();
  const auto n = filter.size();

  if (op == "all" || op == "any" || op == "none")
  {
    std::vector<Expression> args;
    for (Json::ArrayIndex i = 1; i < n; i++)
      args.push_back(compile_legacy_filter(filter[i]));
    return make(op == "all" ? Op::All : (op == "any" ? Op::Any : Op::None), std::move(args));
  }

  if (op == "has" || op == "!has")
  {
    const auto key = filter[1].asString();
    if (key == "$type" || key == "$id")
      return literal(Value(op == "has"));
    return has(key, op == "!has");
  }

  if (op == "in" || op == "!in")
  {
    auto e = make(op == "in" ? Op::LegacyIn : Op::LegacyNotIn, {legacy_key(filter[1].asString())});
    std::vector<Value> values;
    for (Json::ArrayIndex i = 2; i < n; i++)
      values.push_back(Value::fromJson(filter[i]));
    e.itsLabels.push_back(std::move(values));
    return e;
  }

  auto it = simple_ops.find(op);
  if (it != simple_ops.end() && n == 3)
    return make(it->second,
                {legacy_key(filter[1].asString()), literal(Value::fromJson(filter[2]))});

  warn(fmt::format("unsupported legacy filter '{}'", op));
  return literal(Value(false));
}

// ----------------------------------------------------------------------
/*!
 * \brief Compile a legacy function {"stops": [...], "base": ..., ...}
 */
// ----------------------------------------------------------------------

Expression compile_legacy_function(const Json::Value& json)
{
  const auto& stops = json["stops"];
  if (!stops.isArray() || stops.empty())
  {
    if (json.isMember("property") && json["type"].asString() == "identity")
      return get(json["property"].asString());
    warn("unsupported legacy property function");
    return literal(Value());
  }

  // Composite functions are approximated by their zoom part
  const bool composite = stops[0][0].isObject();

  const bool has_property = json.isMember("property") && !composite;
  Expression input = has_property ? get(json["property"].asString()) : zoom();

  std::string type = json.get("type", "").asString();
  if (type.empty())
    type = is_interpolatable(stops[0][1]) ? "exponential" : "interval";

  if (type == "identity")
    return input;

  if (type == "categorical")
  {
    std::vector<Expression> args{input};
    Expression e;
    for (const auto& stop : stops)
    {
      e.itsLabels.push_back({Value::fromJson(stop[0])});
      args.push_back(literal(Value::fromJson(stop[1])));
    }
    args.push_back(literal(Value::fromJson(json.get("default", Json::Value()))));
    auto ret = make(Op::Match, std::move(args));
    ret.itsLabels = std::move(e.itsLabels);
    return ret;
  }

  std::vector<double> inputs;
  std::vector<Expression> args{input};
  for (const auto& stop : stops)
  {
    const auto& in = composite ? stop[0]["zoom"] : stop[0];
    inputs.push_back(in.asDouble());
    args.push_back(literal(Value::fromJson(stop[1])));
  }

  if (type == "interval")
  {
    // Step: output0 applies below the first stop too
    std::vector<Expression> stepargs{args[0], args[1]};
    std::vector<double> stepstops;
    for (std::size_t i = 1; i < inputs.size(); i++)
    {
      stepstops.push_back(inputs[i]);
      stepargs.push_back(args[i + 1]);
    }
    auto ret = make(Op::Step, std::move(stepargs));
    ret.itsStops = std::move(stepstops);
    return ret;
  }

  auto ret = make(Op::Interpolate, std::move(args));
  ret.itsStops = std::move(inputs);
  ret.itsBase = json.get("base", 1.0).asDouble();
  return ret;
}

bool compare(Op op, const Value& a, const Value& b)
{
  if (op == Op::Eq)
    return a == b;
  if (op == Op::Ne)
    return a != b;

  if (a.type == Value::Type::Number && b.type == Value::Type::Number)
  {
    switch (op)
    {
      case Op::Lt:
        return a.number < b.number;
      case Op::Le:
        return a.number <= b.number;
      case Op::Gt:
        return a.number > b.number;
      case Op::Ge:
        return a.number >= b.number;
      default:
        return false;
    }
  }
  if (a.type == Value::Type::String && b.type == Value::Type::String)
  {
    switch (op)
    {
      case Op::Lt:
        return a.string < b.string;
      case Op::Le:
        return a.string <= b.string;
      case Op::Gt:
        return a.string > b.string;
      case Op::Ge:
        return a.string >= b.string;
      default:
        return false;
    }
  }
  return false;
}

std::optional<double> number_arg(const Expression& e, const Context& ctx)
{
  return e.evaluate(ctx).toNumber();
}

}  // namespace

// Upper or lower case conversion of UTF-8 text for Latin, Greek and Cyrillic letters
std::string changeCase(const std::string& s, bool upper)
{
  // Decode
  std::vector<uint32_t> cps;
  for (std::size_t i = 0; i < s.size();)
  {
    const auto c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int len = 1;
    if (c >= 0xF0)
    {
      cp = c & 0x07;
      len = 4;
    }
    else if (c >= 0xE0)
    {
      cp = c & 0x0F;
      len = 3;
    }
    else if (c >= 0xC0)
    {
      cp = c & 0x1F;
      len = 2;
    }
    for (int j = 1; j < len && i + j < s.size(); j++)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
    cps.push_back(cp);
    i += len;
  }

  for (auto& cp : cps)
  {
    if (upper)
    {
      if ((cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) ||
          (cp >= 0x3B1 && cp <= 0x3C9 && cp != 0x3C2) || (cp >= 0x430 && cp <= 0x44F))
        cp -= 0x20;
      else if (cp >= 0x450 && cp <= 0x45F)
        cp -= 0x50;
      else if (cp >= 0x100 && cp <= 0x17F && (cp & 1) == 1 && cp != 0x131 && cp != 0x138 &&
               cp != 0x149 && !(cp >= 0x139 && cp <= 0x148) && !(cp >= 0x179 && cp <= 0x17E))
        cp -= 1;
      else if (((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) && (cp & 1) == 0)
        cp -= 1;
    }
    else
    {
      if ((cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) ||
          (cp >= 0x391 && cp <= 0x3A9) || (cp >= 0x410 && cp <= 0x42F))
        cp += 0x20;
      else if (cp >= 0x400 && cp <= 0x40F)
        cp += 0x50;
      else if (cp >= 0x100 && cp <= 0x17F && (cp & 1) == 0 && cp != 0x130 && cp != 0x138 &&
               !(cp >= 0x139 && cp <= 0x148) && !(cp >= 0x179 && cp <= 0x17E))
        cp += 1;
      else if (((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) && (cp & 1) == 1)
        cp += 1;
    }
  }

  // Encode
  std::string out;
  out.reserve(s.size());
  for (auto cp : cps)
  {
    if (cp < 0x80)
      out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

// ----------------------------------------------------------------------
// Color
// ----------------------------------------------------------------------

std::string Color::hex() const
{
  auto clamp = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
  return fmt::format("#{:02x}{:02x}{:02x}", clamp(r), clamp(g), clamp(b));
}

std::optional<Color> parseColor(const std::string& theColor)
{
  try
  {
    std::string s;
    for (char c : theColor)
      if (c != ' ')
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (s.empty())
      return {};

    if (s == "transparent")
      return Color{0, 0, 0, 0};

    auto named = named_colors.find(s);
    if (named != named_colors.end())
      s = named->second;

    if (s[0] == '#')
    {
      auto hexval = [&](std::size_t pos, std::size_t len)
      { return std::stoi(s.substr(pos, len), nullptr, 16); };
      Color c;
      if (s.size() == 4 || s.size() == 5)
      {
        c.r = hexval(1, 1) * 17 / 255.0;
        c.g = hexval(2, 1) * 17 / 255.0;
        c.b = hexval(3, 1) * 17 / 255.0;
        if (s.size() == 5)
          c.a = hexval(4, 1) * 17 / 255.0;
        return c;
      }
      if (s.size() == 7 || s.size() == 9)
      {
        c.r = hexval(1, 2) / 255.0;
        c.g = hexval(3, 2) / 255.0;
        c.b = hexval(5, 2) / 255.0;
        if (s.size() == 9)
          c.a = hexval(7, 2) / 255.0;
        return c;
      }
      return {};
    }

    const auto open = s.find('(');
    const auto close = s.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open)
      return {};

    const auto func = s.substr(0, open);
    std::vector<bool> percent;
    auto nums = parse_numbers(s.substr(open + 1, close - open - 1), percent);
    if (nums.size() < 3)
      return {};

    Color c;
    if (nums.size() >= 4)
      c.a = percent[3] ? nums[3] / 100 : nums[3];

    if (func == "rgb" || func == "rgba")
    {
      c.r = percent[0] ? nums[0] / 100 : nums[0] / 255;
      c.g = percent[1] ? nums[1] / 100 : nums[1] / 255;
      c.b = percent[2] ? nums[2] / 100 : nums[2] / 255;
      return c;
    }
    if (func == "hsl" || func == "hsla")
    {
      const double h = std::fmod(std::fmod(nums[0], 360) + 360, 360) / 360;
      const double sat = nums[1] / 100;
      const double l = nums[2] / 100;
      if (sat == 0)
        c.r = c.g = c.b = l;
      else
      {
        const double q = (l < 0.5 ? l * (1 + sat) : l + sat - l * sat);
        const double p = 2 * l - q;
        c.r = hue2rgb(p, q, h + 1.0 / 3);
        c.g = hue2rgb(p, q, h);
        c.b = hue2rgb(p, q, h - 1.0 / 3);
      }
      return c;
    }
    return {};
  }
  catch (...)
  {
    return {};
  }
}

// ----------------------------------------------------------------------
// Value
// ----------------------------------------------------------------------

bool Value::truthy() const
{
  return type == Type::Bool && boolean;
}

std::string Value::toString() const
{
  switch (type)
  {
    case Type::Null:
      return "";
    case Type::Bool:
      return boolean ? "true" : "false";
    case Type::Number:
      return format_number(number);
    case Type::String:
      return string;
    case Type::Array:
    {
      std::string ret;
      for (const auto& v : array)
      {
        if (!ret.empty())
          ret += ',';
        ret += v.toString();
      }
      return ret;
    }
  }
  return "";
}

std::optional<double> Value::toNumber() const
{
  switch (type)
  {
    case Type::Number:
      return number;
    case Type::Bool:
      return boolean ? 1.0 : 0.0;
    case Type::String:
      try
      {
        std::size_t pos = 0;
        double d = std::stod(string, &pos);
        if (pos == string.size())
          return d;
      }
      catch (...)
      {
      }
      return {};
    default:
      return {};
  }
}

bool Value::operator==(const Value& other) const
{
  if (type != other.type)
    return false;
  switch (type)
  {
    case Type::Null:
      return true;
    case Type::Bool:
      return boolean == other.boolean;
    case Type::Number:
      return number == other.number;
    case Type::String:
      return string == other.string;
    case Type::Array:
      return array == other.array;
  }
  return false;
}

Value Value::fromJson(const Json::Value& json)
{
  if (json.isBool())
    return Value(json.asBool());
  if (json.isNumeric())
    return Value(json.asDouble());
  if (json.isString())
    return Value(json.asString());
  if (json.isArray())
  {
    std::vector<Value> values;
    for (const auto& v : json)
      values.push_back(fromJson(v));
    return Value(std::move(values));
  }
  return {};
}

Value Value::fromProperty(const Mvt::PropertyValue& value)
{
  if (const auto* s = std::get_if<std::string>(&value))
    return Value(*s);
  if (const auto* d = std::get_if<double>(&value))
    return Value(*d);
  if (const auto* b = std::get_if<bool>(&value))
    return Value(*b);
  return {};
}

// ----------------------------------------------------------------------
// Expression
// ----------------------------------------------------------------------

Expression Expression::compile(const Json::Value& theJson)
{
  try
  {
    if (theJson.isObject())
      return compile_legacy_function(theJson);
    return compile_value(theJson, Scope{});
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to compile style expression")
        .addParameter("expression", theJson.toStyledString());
  }
}

Expression Expression::compileFilter(const Json::Value& theJson)
{
  try
  {
    if (theJson.isNull())
      return literal(Value(true));
    if (is_expression_filter(theJson))
      return compile_value(theJson, Scope{});
    return compile_legacy_filter(theJson);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Failed to compile style filter")
        .addParameter("filter", theJson.toStyledString());
  }
}

Value Expression::evaluate(const Context& ctx) const
{
  const auto& args = itsArgs;
  const auto n = args.size();

  switch (itsOp)
  {
    case Op::Literal:
      return itsValue;

    case Op::Get:
    {
      if (ctx.layer == nullptr || ctx.feature == nullptr)
        return {};
      const auto* v = ctx.layer->get(*ctx.feature, itsKey);
      return v ? Value::fromProperty(*v) : Value();
    }

    case Op::Has:
    case Op::NotHas:
    {
      bool found = (ctx.layer && ctx.feature && ctx.layer->get(*ctx.feature, itsKey) != nullptr);
      return Value(itsOp == Op::Has ? found : !found);
    }

    case Op::Not:
      return Value(n > 0 ? !args[0].evaluate(ctx).truthy() : true);

    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
      if (n < 2)
        return Value(false);
      return Value(compare(itsOp, args[0].evaluate(ctx), args[1].evaluate(ctx)));

    case Op::All:
      for (const auto& a : args)
        if (!a.evaluate(ctx).truthy())
          return Value(false);
      return Value(true);

    case Op::Any:
      for (const auto& a : args)
        if (a.evaluate(ctx).truthy())
          return Value(true);
      return Value(false);

    case Op::None:
      for (const auto& a : args)
        if (a.evaluate(ctx).truthy())
          return Value(false);
      return Value(true);

    case Op::In:
    {
      if (n < 2)
        return Value(false);
      auto needle = args[0].evaluate(ctx);
      auto haystack = args[1].evaluate(ctx);
      if (haystack.type == Value::Type::Array)
        return Value(std::find(haystack.array.begin(), haystack.array.end(), needle) !=
                     haystack.array.end());
      if (haystack.type == Value::Type::String)
        return Value(haystack.string.find(needle.toString()) != std::string::npos);
      return Value(false);
    }

    case Op::LegacyIn:
    case Op::LegacyNotIn:
    {
      auto v = args[0].evaluate(ctx);
      const auto& values = itsLabels[0];
      const bool found = (std::find(values.begin(), values.end(), v) != values.end());
      return Value(itsOp == Op::LegacyIn ? found : !found);
    }

    case Op::Case:
    {
      for (std::size_t i = 0; i + 1 < n; i += 2)
        if (args[i].evaluate(ctx).truthy())
          return args[i + 1].evaluate(ctx);
      return n % 2 == 1 ? args[n - 1].evaluate(ctx) : Value();
    }

    case Op::Match:
    {
      auto input = args[0].evaluate(ctx);
      for (std::size_t i = 0; i < itsLabels.size(); i++)
        for (const auto& label : itsLabels[i])
          if (label == input)
            return args[i + 1].evaluate(ctx);
      return args[n - 1].evaluate(ctx);
    }

    case Op::Coalesce:
      for (const auto& a : args)
      {
        auto v = a.evaluate(ctx);
        if (!v.isNull())
          return v;
      }
      return {};

    case Op::Interpolate:
    {
      auto x = number_arg(args[0], ctx);
      if (!x || itsStops.empty())
        return {};
      const auto& stops = itsStops;
      if (*x <= stops.front())
        return args[1].evaluate(ctx);
      if (*x >= stops.back())
        return args[stops.size()].evaluate(ctx);
      std::size_t i = 0;
      while (i + 1 < stops.size() && *x >= stops[i + 1])
        ++i;
      const double x0 = stops[i];
      const double x1 = stops[i + 1];
      double t = 0;
      if (itsBase == 1)
        t = (*x - x0) / (x1 - x0);
      else
        t = (std::pow(itsBase, *x - x0) - 1) / (std::pow(itsBase, x1 - x0) - 1);
      return mix(args[i + 1].evaluate(ctx), args[i + 2].evaluate(ctx), t);
    }

    case Op::Step:
    {
      auto x = number_arg(args[0], ctx);
      if (!x)
        return args[1].evaluate(ctx);
      std::size_t i = 0;
      while (i < itsStops.size() && *x >= itsStops[i])
        ++i;
      return args[i + 1].evaluate(ctx);
    }

    case Op::Zoom:
      return Value(ctx.zoom);

    case Op::GeometryType:
    {
      if (ctx.feature == nullptr)
        return {};
      switch (ctx.feature->type)
      {
        case Mvt::GeomType::Point:
          return Value(std::string("Point"));
        case Mvt::GeomType::LineString:
          return Value(std::string("LineString"));
        case Mvt::GeomType::Polygon:
          return Value(std::string("Polygon"));
        default:
          return {};
      }
    }

    case Op::Concat:
    {
      std::string ret;
      for (const auto& a : args)
        ret += a.evaluate(ctx).toString();
      return Value(ret);
    }

    case Op::ToString:
      return Value(n > 0 ? args[0].evaluate(ctx).toString() : std::string());

    case Op::ToNumber:
      for (const auto& a : args)
      {
        auto v = a.evaluate(ctx).toNumber();
        if (v)
          return Value(*v);
      }
      return Value(0.0);

    case Op::ToBoolean:
    {
      if (n == 0)
        return Value(false);
      auto v = args[0].evaluate(ctx);
      switch (v.type)
      {
        case Value::Type::Null:
          return Value(false);
        case Value::Type::Bool:
          return v;
        case Value::Type::Number:
          return Value(v.number != 0 && !std::isnan(v.number));
        case Value::Type::String:
          return Value(!v.string.empty());
        case Value::Type::Array:
          return Value(true);
      }
      return Value(false);
    }

    case Op::Assert:
      for (const auto& a : args)
      {
        auto v = a.evaluate(ctx);
        if (!v.isNull())
          return v;
      }
      return {};

    case Op::Length:
    {
      if (n == 0)
        return {};
      auto v = args[0].evaluate(ctx);
      if (v.type == Value::Type::String)
        return Value(static_cast<double>(v.string.size()));
      if (v.type == Value::Type::Array)
        return Value(static_cast<double>(v.array.size()));
      return {};
    }

    case Op::Plus:
    case Op::Times:
    case Op::Min:
    case Op::Max:
    {
      std::optional<double> acc;
      for (const auto& a : args)
      {
        auto v = number_arg(a, ctx);
        if (!v)
          return {};
        if (!acc)
          acc = *v;
        else if (itsOp == Op::Plus)
          *acc += *v;
        else if (itsOp == Op::Times)
          *acc *= *v;
        else if (itsOp == Op::Min)
          *acc = std::min(*acc, *v);
        else
          *acc = std::max(*acc, *v);
      }
      return acc ? Value(*acc) : Value();
    }

    case Op::Minus:
    case Op::Divide:
    case Op::Mod:
    case Op::Pow:
    {
      if (n == 1 && itsOp == Op::Minus)
      {
        auto v = number_arg(args[0], ctx);
        return v ? Value(-*v) : Value();
      }
      if (n < 2)
        return {};
      auto a = number_arg(args[0], ctx);
      auto b = number_arg(args[1], ctx);
      if (!a || !b)
        return {};
      if (itsOp == Op::Minus)
        return Value(*a - *b);
      if (itsOp == Op::Divide)
        return Value(*a / *b);
      if (itsOp == Op::Mod)
        return Value(std::fmod(*a, *b));
      return Value(std::pow(*a, *b));
    }

    case Op::Abs:
    case Op::Round:
    case Op::Floor:
    case Op::Ceil:
    case Op::Sqrt:
    case Op::Log10:
    case Op::Ln:
    case Op::Log2:
    {
      if (n == 0)
        return {};
      auto v = number_arg(args[0], ctx);
      if (!v)
        return {};
      switch (itsOp)
      {
        case Op::Abs:
          return Value(std::abs(*v));
        case Op::Round:
          return Value(std::round(*v));
        case Op::Floor:
          return Value(std::floor(*v));
        case Op::Ceil:
          return Value(std::ceil(*v));
        case Op::Sqrt:
          return Value(std::sqrt(*v));
        case Op::Log10:
          return Value(std::log10(*v));
        case Op::Ln:
          return Value(std::log(*v));
        default:
          return Value(std::log2(*v));
      }
    }

    case Op::Upcase:
    case Op::Downcase:
    {
      if (n == 0)
        return {};
      return Value(changeCase(args[0].evaluate(ctx).toString(), itsOp == Op::Upcase));
    }

    case Op::Rgba:
    {
      std::vector<double> c;
      for (const auto& a : args)
        c.push_back(number_arg(a, ctx).value_or(0));
      while (c.size() < 4)
        c.push_back(1);
      return Value(fmt::format("rgba({},{},{},{})", c[0], c[1], c[2], c[3]));
    }
  }
  return {};
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
