#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace nemo {

struct ChoiceValue {
    std::string value;
    friend bool operator==(const ChoiceValue&, const ChoiceValue&) = default;
};

struct Vector2Value {
    std::array<float, 2> value{};
    friend bool operator==(const Vector2Value&, const Vector2Value&) = default;
};

struct Vector3Value {
    std::array<float, 3> value{};
    friend bool operator==(const Vector3Value&, const Vector3Value&) = default;
};

struct ColorValue {
    std::array<float, 4> value{};
    friend bool operator==(const ColorValue&, const ColorValue&) = default;
};

using ParameterValue =
    std::variant<bool, std::int64_t, double, std::string, ChoiceValue, Vector2Value, Vector3Value, ColorValue>;
using ParameterValues = std::map<std::string, ParameterValue>;

// Checks that a value is a valid, finite representation independent of schema.
[[nodiscard]] std::optional<std::string> validateParameterValueRepresentation(const ParameterValue& value);
// Textual display boundary. This is not a parser and is not used by evaluators.
[[nodiscard]] std::string parameterValueText(const ParameterValue& value);
// Injective, type-tagged representation for hashes and other identity keys.
[[nodiscard]] std::string canonicalParameterValue(const ParameterValue& value);

}  // namespace nemo
