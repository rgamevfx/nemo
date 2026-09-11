#include "nemo/core/document/ParameterValueJson.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace nemo {
namespace {

[[noreturn]] void malformed(std::string message) {
    throw std::invalid_argument("malformed parameter value: " + std::move(message));
}

float finiteFloat(double value, const char* context) {
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
        malformed(std::string(context) + " must be finite and float-representable");
    return static_cast<float>(value);
}

double finiteDouble(const nlohmann::json& value) {
    if (!value.is_number())
        malformed("float value must be numeric");
    const double number = value.get<double>();
    if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
        malformed("float value must be finite and float-representable");
    return number;
}

std::int64_t integerValue(const nlohmann::json& value) {
    try {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                malformed("integer value is outside int64 range");
            return static_cast<std::int64_t>(number);
        }
        if (value.is_number_integer())
            return value.get<std::int64_t>();
    } catch (const nlohmann::json::exception&) {
        malformed("integer value is outside int64 range");
    }
    malformed("integer value must be a signed 64-bit integer");
}

template <std::size_t N>
std::array<float, N> vectorValue(const nlohmann::json& value, const char* context) {
    if (!value.is_array() || value.size() != N)
        malformed(std::string(context) + " value must be an array of exactly " + std::to_string(N) + " numbers");
    std::array<float, N> result{};
    for (std::size_t index = 0; index < N; ++index) {
        if (!value.at(index).is_number())
            malformed(std::string(context) + " channels must be numeric");
        result[index] = finiteFloat(value.at(index).get<double>(), context);
    }
    return result;
}

}  // namespace

nlohmann::json parameterValueToJson(const ParameterValue& value) {
    if (const auto problem = validateParameterValueRepresentation(value))
        malformed(*problem);
    return std::visit(
        [](const auto& typed) -> nlohmann::json {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, bool>)
                return {{"type", "boolean"}, {"value", typed}};
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return {{"type", "integer"}, {"value", typed}};
            else if constexpr (std::is_same_v<T, double>)
                return {{"type", "float"}, {"value", typed}};
            else if constexpr (std::is_same_v<T, std::string>)
                return {{"type", "string"}, {"value", typed}};
            else if constexpr (std::is_same_v<T, ChoiceValue>)
                return {{"type", "choice"}, {"value", typed.value}};
            else if constexpr (std::is_same_v<T, Vector2Value>)
                return {{"type", "vector2"}, {"value", typed.value}};
            else if constexpr (std::is_same_v<T, Vector3Value>)
                return {{"type", "vector3"}, {"value", typed.value}};
            else
                return {{"type", "color"}, {"value", typed.value}};
        },
        value);
}

ParameterValue parameterValueFromJson(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("type") || !value.contains("value") ||
        !value.at("type").is_string())
        malformed("value must be a tagged object containing only type and value");
    const std::string type = value.at("type").get<std::string>();
    const auto& payload = value.at("value");
    if (type == "boolean") {
        if (!payload.is_boolean())
            malformed("boolean value must be boolean");
        return payload.get<bool>();
    }
    if (type == "integer")
        return integerValue(payload);
    if (type == "float")
        return finiteDouble(payload);
    if (type == "string") {
        if (!payload.is_string())
            malformed("string value must be string");
        return payload.get<std::string>();
    }
    if (type == "choice") {
        if (!payload.is_string())
            malformed("choice value must be string");
        return ChoiceValue{payload.get<std::string>()};
    }
    if (type == "vector2")
        return Vector2Value{vectorValue<2>(payload, "vector2")};
    if (type == "vector3")
        return Vector3Value{vectorValue<3>(payload, "vector3")};
    if (type == "color")
        return ColorValue{vectorValue<4>(payload, "color")};
    malformed("unknown type tag '" + type + "'");
}

}  // namespace nemo
