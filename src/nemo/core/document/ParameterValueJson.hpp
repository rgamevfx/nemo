#pragma once

#include "nemo/core/document/ParameterValue.hpp"
#include <nlohmann/json.hpp>

namespace nemo {

[[nodiscard]] nlohmann::json parameterValueToJson(const ParameterValue& value);
[[nodiscard]] ParameterValue parameterValueFromJson(const nlohmann::json& value);

}  // namespace nemo
