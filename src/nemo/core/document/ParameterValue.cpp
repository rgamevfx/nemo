#include "nemo/core/document/ParameterValue.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nemo {
namespace {

template <typename T>
std::string numberText(T value) {
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                                      std::numeric_limits<T>::max_digits10);
    if (result.ec == std::errc{})
        return std::string(buffer, result.ptr);
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
    return stream.str();
}

template <std::size_t N>
std::string vectorText(const std::array<float, N>& values, char separator) {
    std::string result;
    for (std::size_t index = 0; index < N; ++index) {
        if (index != 0)
            result.push_back(separator);
        result += numberText(values[index]);
    }
    return result;
}

template <std::size_t N>
std::string canonicalVector(std::string_view tag, const std::array<float, N>& values) {
    std::string result{tag};
    result.push_back(':');
    result += std::to_string(N);
    result.push_back(':');
    result += vectorText(values, ',');
    return result;
}

}  // namespace

std::optional<std::string> validateParameterValueRepresentation(const ParameterValue& value) {
    return std::visit(
        [](const auto& typed) -> std::optional<std::string> {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(typed) || !std::isfinite(static_cast<float>(typed)))
                    return "must be finite and float-representable";
            } else if constexpr (std::is_same_v<T, Vector2Value>) {
                for (const float component : typed.value)
                    if (!std::isfinite(component))
                        return "must contain finite components";
            } else if constexpr (std::is_same_v<T, Vector3Value>) {
                for (const float component : typed.value)
                    if (!std::isfinite(component))
                        return "must contain finite components";
            } else if constexpr (std::is_same_v<T, ColorValue>) {
                for (const float component : typed.value)
                    if (!std::isfinite(component))
                        return "must contain finite channels";
            }
            return std::nullopt;
        },
        value);
}

std::string parameterValueText(const ParameterValue& value) {
    return std::visit(
        [](const auto& typed) -> std::string {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, bool>)
                return typed ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return std::to_string(typed);
            else if constexpr (std::is_same_v<T, double>)
                return numberText(typed);
            else if constexpr (std::is_same_v<T, std::string>)
                return typed;
            else if constexpr (std::is_same_v<T, ChoiceValue>)
                return typed.value;
            else if constexpr (std::is_same_v<T, Vector2Value>)
                return vectorText(typed.value, ' ');
            else if constexpr (std::is_same_v<T, Vector3Value>)
                return vectorText(typed.value, ' ');
            else
                return vectorText(typed.value, ' ');
        },
        value);
}

std::string canonicalParameterValue(const ParameterValue& value) {
    if (const auto problem = validateParameterValueRepresentation(value))
        throw std::invalid_argument("cannot canonicalize parameter value: " + *problem);
    return std::visit(
        [](const auto& typed) -> std::string {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, bool>)
                return std::string{"boolean:"} + (typed ? "true" : "false");
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return "integer:" + std::to_string(typed);
            else if constexpr (std::is_same_v<T, double>)
                return "float:" + numberText(typed);
            else if constexpr (std::is_same_v<T, std::string>)
                return "string:" + std::to_string(typed.size()) + ":" + typed;
            else if constexpr (std::is_same_v<T, ChoiceValue>)
                return "choice:" + std::to_string(typed.value.size()) + ":" + typed.value;
            else if constexpr (std::is_same_v<T, Vector2Value>)
                return canonicalVector("vector2", typed.value);
            else if constexpr (std::is_same_v<T, Vector3Value>)
                return canonicalVector("vector3", typed.value);
            else
                return canonicalVector("color", typed.value);
        },
        value);
}

}  // namespace nemo
