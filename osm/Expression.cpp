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
Expression compile_value(const Json::Value& json, const Scope& scope);

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

Expression compile_get(const Json::Value& json, const Scope& /* scope */)
{
  const std::string name = json[0].asString();
  if (json.size() == 2 && json[1].isString())
  {
    if (name == "get")
      return get(json[1].asString());
    return has(json[1].asString(), name == "!has");
  }
  warn(fmt::format("unsupported form of '{}'", name));
  return literal(Value());
}

Expression compile_var(const Json::Value& json, const Scope& scope)
{
  auto it = scope.find(json[1].asString());
  if (it != scope.end())
    return it->second;
  warn(fmt::format("unknown variable '{}'", json[1].asString()));
  return literal(Value());
}

Expression compile_let(const Json::Value& json, const Scope& scope)
{
  const auto n = json.size();
  Scope inner = scope;
  for (Json::ArrayIndex i = 1; i + 1 < n; i += 2)
    inner[json[i].asString()] = compile_value(json[i + 1], scope);
  return compile_value(json[n - 1], inner);
}

Expression compile_format(const Json::Value& json, const Scope& scope)
{
  // Text sections and their formatting options alternate
  std::vector<Expression> args;
  for (Json::ArrayIndex i = 1; i < json.size(); i++)
  {
    if (!json[i].isObject())
      args.push_back(compile_value(json[i], scope));
  }
  return make(Op::Concat, std::move(args));
}

// A label of match: a single value or an array of values
std::vector<Value> match_labels(const Json::Value& json)
{
  std::vector<Value> labels;
  if (json.isArray())
    for (const auto& label : json)
      labels.push_back(Value::fromJson(label));
  else
    labels.push_back(Value::fromJson(json));
  return labels;
}

Expression compile_match(const Json::Value& json, const Scope& scope)
{
  // ["match", input, label1, output1, label2, output2, ..., fallback]
  const auto n = json.size();
  std::vector<Expression> args{compile_value(json[1], scope)};
  std::vector<std::vector<Value>> labels;
  for (Json::ArrayIndex i = 2; i + 1 < n; i += 2)
  {
    labels.push_back(match_labels(json[i]));
    args.push_back(compile_value(json[i + 1], scope));
  }
  args.push_back(compile_value(json[n - 1], scope));
  auto ret = make(Op::Match, std::move(args));
  ret.itsLabels = std::move(labels);
  return ret;
}

Expression compile_interpolate(const Json::Value& json, const Scope& scope)
{
  // ["interpolate", ["linear"|"exponential", base|"cubic-bezier",...], input, stop, out, ...]
  double base = 1;
  const auto& type = json[1];
  if (type.isArray() && type.size() >= 2 && type[0].asString() == "exponential")
    base = type[1].asDouble();
  std::vector<Expression> args{compile_value(json[2], scope)};
  std::vector<double> stops;
  for (Json::ArrayIndex i = 3; i + 1 < json.size(); i += 2)
  {
    stops.push_back(json[i].asDouble());
    args.push_back(compile_value(json[i + 1], scope));
  }
  auto ret = make(Op::Interpolate, std::move(args));
  ret.itsStops = std::move(stops);
  ret.itsBase = base;
  return ret;
}

Expression compile_step(const Json::Value& json, const Scope& scope)
{
  // ["step", input, output0, stop1, output1, ...]
  std::vector<Expression> args{compile_value(json[1], scope), compile_value(json[2], scope)};
  std::vector<double> stops;
  for (Json::ArrayIndex i = 3; i + 1 < json.size(); i += 2)
  {
    stops.push_back(json[i].asDouble());
    args.push_back(compile_value(json[i + 1], scope));
  }
  auto ret = make(Op::Step, std::move(args));
  ret.itsStops = std::move(stops);
  return ret;
}

using Compiler = Expression (*)(const Json::Value& json, const Scope& scope);

// The special operators with compilers of their own. Feature and rendering state
// unknown to a static renderer are given constant values.
const std::map<std::string, Compiler> special_compilers = {
    {"literal",
     [](const Json::Value& json, const Scope& /* scope */)
     { return literal(json.size() > 1 ? Value::fromJson(json[1]) : Value()); }},
    {"get", compile_get},
    {"has", compile_get},
    {"!has", compile_get},
    {"zoom", [](const Json::Value& /* json */, const Scope& /* scope */) { return zoom(); }},
    {"geometry-type",
     [](const Json::Value& /* json */, const Scope& /* scope */) { return geometry_type(); }},
    {"is-supported-script",
     [](const Json::Value& /* json */, const Scope& /* scope */) { return literal(Value(true)); }},
    {"resolved-locale",
     [](const Json::Value& /* json */, const Scope& /* scope */)
     { return literal(Value(std::string("en"))); }},
    {"id",
     [](const Json::Value& /* json */, const Scope& /* scope */) { return literal(Value()); }},
    {"feature-state",
     [](const Json::Value& /* json */, const Scope& /* scope */) { return literal(Value()); }},
    {"collator",
     [](const Json::Value& /* json */, const Scope& /* scope */) { return literal(Value()); }},
    {"line-progress",
     [](const Json::Value& /* json */, const Scope& /* scope */) { return literal(Value(0.0)); }},
    {"var", compile_var},
    {"let", compile_let},
    {"format", compile_format},
    {"match", compile_match},
    {"interpolate", compile_interpolate},
    {"interpolate-hcl", compile_interpolate},
    {"interpolate-lab", compile_interpolate},
    {"step", compile_step}};

Expression compile_expression(const Json::Value& json, const Scope& scope)
{
  const std::string name = json[0].asString();

  auto special = special_compilers.find(name);
  if (special != special_compilers.end())
    return special->second(json, scope);

  auto it = simple_ops.find(name);
  if (it == simple_ops.end())
  {
    warn(fmt::format("unsupported style expression '{}'", name));
    return literal(Value());
  }

  std::vector<Expression> args;
  for (Json::ArrayIndex i = 1; i < json.size(); i++)
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

namespace
{
std::vector<uint32_t> utf8_decode(const std::string& s)
{
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
  return cps;
}

std::string utf8_encode(const std::vector<uint32_t>& cps)
{
  std::string out;
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

bool in_range(uint32_t cp, uint32_t first, uint32_t last)
{
  return cp >= first && cp <= last;
}

// Latin Extended-A letters come in upper/lower case pairs starting at even code
// points, except in these two ranges where the pairs start at odd code points
bool odd_case_pairs(uint32_t cp)
{
  return in_range(cp, 0x139, 0x148) || in_range(cp, 0x179, 0x17E);
}

uint32_t to_upper(uint32_t cp)
{
  if (in_range(cp, 'a', 'z') || (in_range(cp, 0xE0, 0xFE) && cp != 0xF7) ||
      (in_range(cp, 0x3B1, 0x3C9) && cp != 0x3C2) || in_range(cp, 0x430, 0x44F))
    return cp - 0x20;
  if (in_range(cp, 0x450, 0x45F))
    return cp - 0x50;
  if (!in_range(cp, 0x100, 0x17F) || cp == 0x131 || cp == 0x138 || cp == 0x149)
    return cp;
  const bool lower = (odd_case_pairs(cp) ? (cp & 1) == 0 : (cp & 1) == 1);
  return lower ? cp - 1 : cp;
}

uint32_t to_lower(uint32_t cp)
{
  if (in_range(cp, 'A', 'Z') || (in_range(cp, 0xC0, 0xDE) && cp != 0xD7) ||
      in_range(cp, 0x391, 0x3A9) || in_range(cp, 0x410, 0x42F))
    return cp + 0x20;
  if (in_range(cp, 0x400, 0x40F))
    return cp + 0x50;
  if (!in_range(cp, 0x100, 0x17F) || cp == 0x130 || cp == 0x138)
    return cp;
  const bool upper = (odd_case_pairs(cp) ? (cp & 1) == 1 : (cp & 1) == 0);
  return upper ? cp + 1 : cp;
}

}  // namespace

// Upper or lower case conversion of UTF-8 text for Latin, Greek and Cyrillic letters
std::string changeCase(const std::string& s, bool upper)
{
  auto cps = utf8_decode(s);
  for (auto& cp : cps)
    cp = (upper ? to_upper(cp) : to_lower(cp));
  return utf8_encode(cps);
}

// ----------------------------------------------------------------------
// Color
// ----------------------------------------------------------------------

std::string Color::hex() const
{
  auto clamp = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
  return fmt::format("#{:02x}{:02x}{:02x}", clamp(r), clamp(g), clamp(b));
}

namespace
{
// #rgb, #rgba, #rrggbb or #rrggbbaa
std::optional<Color> parse_hex_color(const std::string& s)
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

void set_hsl(Color& c, double hue, double saturation, double lightness)
{
  const double h = std::fmod(std::fmod(hue, 360) + 360, 360) / 360;
  const double sat = saturation / 100;
  const double l = lightness / 100;
  if (sat == 0)
  {
    c.r = c.g = c.b = l;
    return;
  }
  const double q = (l < 0.5 ? l * (1 + sat) : l + sat - l * sat);
  const double p = 2 * l - q;
  c.r = hue2rgb(p, q, h + 1.0 / 3);
  c.g = hue2rgb(p, q, h);
  c.b = hue2rgb(p, q, h - 1.0 / 3);
}

double rgb_component(double value, bool percent)
{
  return percent ? value / 100 : value / 255;
}

// rgb(), rgba(), hsl() or hsla()
std::optional<Color> parse_color_function(const std::string& s)
{
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
    c.r = rgb_component(nums[0], percent[0]);
    c.g = rgb_component(nums[1], percent[1]);
    c.b = rgb_component(nums[2], percent[2]);
    return c;
  }
  if (func == "hsl" || func == "hsla")
  {
    set_hsl(c, nums[0], nums[1], nums[2]);
    return c;
  }
  return {};
}
}  // namespace

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
      return parse_hex_color(s);
    return parse_color_function(s);
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

namespace
{
Value eval_get(const Expression& e, const Context& ctx)
{
  if (ctx.layer == nullptr || ctx.feature == nullptr)
    return {};
  const auto* v = ctx.layer->get(*ctx.feature, e.itsKey);
  return v ? Value::fromProperty(*v) : Value();
}

Value eval_has(const Expression& e, const Context& ctx)
{
  bool found = (ctx.layer && ctx.feature && ctx.layer->get(*ctx.feature, e.itsKey) != nullptr);
  return Value(e.itsOp == Op::Has ? found : !found);
}

Value eval_compare(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  if (args.size() < 2)
    return Value(false);
  return Value(compare(e.itsOp, args[0].evaluate(ctx), args[1].evaluate(ctx)));
}

// all, any and none
Value eval_logical(const Expression& e, const Context& ctx)
{
  // all stops at the first false, any and none at the first true argument
  const bool stop_at = (e.itsOp != Op::All);
  for (const auto& a : e.itsArgs)
    if (a.evaluate(ctx).truthy() == stop_at)
      return Value(e.itsOp == Op::Any);
  return Value(e.itsOp != Op::Any);
}

Value eval_in(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  if (args.size() < 2)
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

Value eval_legacy_in(const Expression& e, const Context& ctx)
{
  auto v = e.itsArgs[0].evaluate(ctx);
  const auto& values = e.itsLabels[0];
  const bool found = (std::find(values.begin(), values.end(), v) != values.end());
  return Value(e.itsOp == Op::LegacyIn ? found : !found);
}

Value eval_case(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  const auto n = args.size();
  for (std::size_t i = 0; i + 1 < n; i += 2)
    if (args[i].evaluate(ctx).truthy())
      return args[i + 1].evaluate(ctx);
  return n % 2 == 1 ? args[n - 1].evaluate(ctx) : Value();
}

Value eval_match(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  auto input = args[0].evaluate(ctx);
  for (std::size_t i = 0; i < e.itsLabels.size(); i++)
  {
    const auto& labels = e.itsLabels[i];
    if (std::find(labels.begin(), labels.end(), input) != labels.end())
      return args[i + 1].evaluate(ctx);
  }
  return args.back().evaluate(ctx);
}

// The first non-null argument
Value eval_coalesce(const Expression& e, const Context& ctx)
{
  for (const auto& a : e.itsArgs)
  {
    auto v = a.evaluate(ctx);
    if (!v.isNull())
      return v;
  }
  return {};
}

Value eval_interpolate(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  const auto& stops = e.itsStops;
  auto x = number_arg(args[0], ctx);
  if (!x || stops.empty())
    return {};
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
  if (e.itsBase == 1)
    t = (*x - x0) / (x1 - x0);
  else
    t = (std::pow(e.itsBase, *x - x0) - 1) / (std::pow(e.itsBase, x1 - x0) - 1);
  return mix(args[i + 1].evaluate(ctx), args[i + 2].evaluate(ctx), t);
}

Value eval_step(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  auto x = number_arg(args[0], ctx);
  if (!x)
    return args[1].evaluate(ctx);
  std::size_t i = 0;
  while (i < e.itsStops.size() && *x >= e.itsStops[i])
    ++i;
  return args[i + 1].evaluate(ctx);
}

Value eval_geometry_type(const Context& ctx)
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

Value eval_concat(const Expression& e, const Context& ctx)
{
  std::string ret;
  for (const auto& a : e.itsArgs)
    ret += a.evaluate(ctx).toString();
  return Value(ret);
}

Value eval_to_number(const Expression& e, const Context& ctx)
{
  for (const auto& a : e.itsArgs)
  {
    auto v = a.evaluate(ctx).toNumber();
    if (v)
      return Value(*v);
  }
  return Value(0.0);
}

Value eval_to_boolean(const Expression& e, const Context& ctx)
{
  if (e.itsArgs.empty())
    return Value(false);
  auto v = e.itsArgs[0].evaluate(ctx);
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

Value eval_length(const Expression& e, const Context& ctx)
{
  if (e.itsArgs.empty())
    return {};
  auto v = e.itsArgs[0].evaluate(ctx);
  if (v.type == Value::Type::String)
    return Value(static_cast<double>(v.string.size()));
  if (v.type == Value::Type::Array)
    return Value(static_cast<double>(v.array.size()));
  return {};
}

double accumulate(Op op, double acc, double v)
{
  switch (op)
  {
    case Op::Plus:
      return acc + v;
    case Op::Times:
      return acc * v;
    case Op::Min:
      return std::min(acc, v);
    default:
      return std::max(acc, v);
  }
}

// +, *, min and max of any number of arguments
Value eval_fold(const Expression& e, const Context& ctx)
{
  std::optional<double> acc;
  for (const auto& a : e.itsArgs)
  {
    auto v = number_arg(a, ctx);
    if (!v)
      return {};
    acc = (acc ? accumulate(e.itsOp, *acc, *v) : *v);
  }
  return acc ? Value(*acc) : Value();
}

// -, /, % and ^, and unary minus
Value eval_binary(const Expression& e, const Context& ctx)
{
  const auto& args = e.itsArgs;
  if (args.size() == 1 && e.itsOp == Op::Minus)
  {
    auto v = number_arg(args[0], ctx);
    return v ? Value(-*v) : Value();
  }
  if (args.size() < 2)
    return {};
  auto a = number_arg(args[0], ctx);
  auto b = number_arg(args[1], ctx);
  if (!a || !b)
    return {};
  switch (e.itsOp)
  {
    case Op::Minus:
      return Value(*a - *b);
    case Op::Divide:
      return Value(*a / *b);
    case Op::Mod:
      return Value(std::fmod(*a, *b));
    default:
      return Value(std::pow(*a, *b));
  }
}

double math_function(Op op, double v)
{
  switch (op)
  {
    case Op::Abs:
      return std::abs(v);
    case Op::Round:
      return std::round(v);
    case Op::Floor:
      return std::floor(v);
    case Op::Ceil:
      return std::ceil(v);
    case Op::Sqrt:
      return std::sqrt(v);
    case Op::Log10:
      return std::log10(v);
    case Op::Ln:
      return std::log(v);
    default:
      return std::log2(v);
  }
}

Value eval_math(const Expression& e, const Context& ctx)
{
  if (e.itsArgs.empty())
    return {};
  auto v = number_arg(e.itsArgs[0], ctx);
  if (!v)
    return {};
  return Value(math_function(e.itsOp, *v));
}

Value eval_change_case(const Expression& e, const Context& ctx)
{
  if (e.itsArgs.empty())
    return {};
  return Value(changeCase(e.itsArgs[0].evaluate(ctx).toString(), e.itsOp == Op::Upcase));
}

Value eval_rgba(const Expression& e, const Context& ctx)
{
  std::vector<double> c;
  for (const auto& a : e.itsArgs)
    c.push_back(number_arg(a, ctx).value_or(0));
  while (c.size() < 4)
    c.push_back(1);
  return Value(fmt::format("rgba({},{},{},{})", c[0], c[1], c[2], c[3]));
}

}  // namespace

Value Expression::evaluate(const Context& ctx) const
{
  switch (itsOp)
  {
    case Op::Literal:
      return itsValue;
    case Op::Get:
      return eval_get(*this, ctx);
    case Op::Has:
    case Op::NotHas:
      return eval_has(*this, ctx);
    case Op::Not:
      return Value(itsArgs.empty() || !itsArgs[0].evaluate(ctx).truthy());
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
      return eval_compare(*this, ctx);
    case Op::All:
    case Op::Any:
    case Op::None:
      return eval_logical(*this, ctx);
    case Op::In:
      return eval_in(*this, ctx);
    case Op::LegacyIn:
    case Op::LegacyNotIn:
      return eval_legacy_in(*this, ctx);
    case Op::Case:
      return eval_case(*this, ctx);
    case Op::Match:
      return eval_match(*this, ctx);
    case Op::Coalesce:
    case Op::Assert:
      return eval_coalesce(*this, ctx);
    case Op::Interpolate:
      return eval_interpolate(*this, ctx);
    case Op::Step:
      return eval_step(*this, ctx);
    case Op::Zoom:
      return Value(ctx.zoom);
    case Op::GeometryType:
      return eval_geometry_type(ctx);
    case Op::Concat:
      return eval_concat(*this, ctx);
    case Op::ToString:
      return Value(itsArgs.empty() ? std::string() : itsArgs[0].evaluate(ctx).toString());
    case Op::ToNumber:
      return eval_to_number(*this, ctx);
    case Op::ToBoolean:
      return eval_to_boolean(*this, ctx);
    case Op::Length:
      return eval_length(*this, ctx);
    case Op::Plus:
    case Op::Times:
    case Op::Min:
    case Op::Max:
      return eval_fold(*this, ctx);
    case Op::Minus:
    case Op::Divide:
    case Op::Mod:
    case Op::Pow:
      return eval_binary(*this, ctx);
    case Op::Abs:
    case Op::Round:
    case Op::Floor:
    case Op::Ceil:
    case Op::Sqrt:
    case Op::Log10:
    case Op::Ln:
    case Op::Log2:
      return eval_math(*this, ctx);
    case Op::Upcase:
    case Op::Downcase:
      return eval_change_case(*this, ctx);
    case Op::Rgba:
      return eval_rgba(*this, ctx);
  }
  return {};
}

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
