#pragma once

#include "nemo/core/document/ParameterValue.hpp"
#include <nlohmann/json.hpp>
#include <string_view>

namespace nemo {

[[nodiscard]] nlohmann::json parameterValueToJson(const ParameterValue& value);
[[nodiscard]] ParameterValue parameterValueFromJson(const nlohmann::json& value);

// True when `tag` names one of the typed ParameterValue representations this
// build interprets. Callers that must not lose authored data can preserve a
// record with an unknown tag verbatim instead of rejecting the whole document;
// a known tag with a malformed payload remains an error.
[[nodiscard]] bool parameterValueTypeTagKnown(std::string_view tag);

}  // namespace nemo
