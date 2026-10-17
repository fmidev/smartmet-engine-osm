// ======================================================================
/*!
 * \brief MapLibre style expressions
 *
 * Compiles the expressions, legacy filters and legacy property functions of
 * MapLibre/Mapbox GL styles into a tree which can be evaluated efficiently
 * for each feature. The supported subset covers what freely available
 * basemap styles use (Protomaps, OpenMapTiles, Shortbread/VersaTiles).
 * Unsupported operators evaluate to null and are reported once.
 *
 * Reference: https://maplibre.org/maplibre-style-spec/expressions/
 */
// ======================================================================

#pragma once

#include "MvtTile.h"
#include <json/json.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SmartMet
{
namespace Engine
{
namespace OSM
{
// RGBA colour with components in range 0...1
struct Color
{
  double r = 0;
  double g = 0;
  double b = 0;
  double a = 1;

  // "#rrggbb" without the alpha
  std::string hex() const;
};

std::optional<Color> parseColor(const std::string& theColor);

// Upper or lower case conversion of UTF-8 text for Latin, Greek and Cyrillic letters
std::string changeCase(const std::string& theText, bool theUpper);

// A dynamically typed value
struct Value
{
  enum class Type
  {
    Null,
    Bool,
    Number,
    String,
    Array
  };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;

  Value() = default;
  explicit Value(bool b) : type(Type::Bool), boolean(b) {}
  explicit Value(double d) : type(Type::Number), number(d) {}
  explicit Value(std::string s) : type(Type::String), string(std::move(s)) {}
  explicit Value(std::vector<Value> a) : type(Type::Array), array(std::move(a)) {}
  Value(const char*) = delete;  // would otherwise convert to bool

  bool isNull() const { return type == Type::Null; }
  bool truthy() const;
  std::string toString() const;
  std::optional<double> toNumber() const;

  bool operator==(const Value& other) const;
  bool operator!=(const Value& other) const { return !(*this == other); }

  static Value fromJson(const Json::Value& json);
  static Value fromProperty(const Mvt::PropertyValue& value);
};

// Evaluation context
struct Context
{
  double zoom = 0;
  const Mvt::Layer* layer = nullptr;
  const Mvt::Feature* feature = nullptr;
};

class Expression
{
 public:
  // Compile a property value: a constant, an expression or a legacy function
  static Expression compile(const Json::Value& theJson);

  // Compile a layer filter, handling the legacy filter syntax too
  static Expression compileFilter(const Json::Value& theJson);

  Value evaluate(const Context& theContext) const;

  // True if the result depends on feature properties or geometry
  bool isDataDependent() const { return itsDataDependent; }

  // True if the result depends on the zoom level
  bool isZoomDependent() const { return itsZoomDependent; }

  bool isConstant() const { return !itsDataDependent && !itsZoomDependent; }

  enum class Op;

  // Implementation details, public for the compiler helpers
  Op itsOp{};
  Value itsValue;                         // literal value
  std::string itsKey;                     // property name for get/has
  std::vector<Expression> itsArgs;        // sub-expressions
  std::vector<double> itsStops;           // interpolate/step inputs
  std::vector<std::vector<Value>> itsLabels;  // match labels
  double itsBase = 1;                     // interpolation base
  bool itsDataDependent = false;
  bool itsZoomDependent = false;
};

}  // namespace OSM
}  // namespace Engine
}  // namespace SmartMet
