#pragma once

#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Document.hpp"

#include <vector>

namespace nemo {

struct KeyframeEdit {
    ParameterAddress address;
    Keyframe key;
};

// All factories return the same validated, candidate-applied command used by
// graph and parameter edits. A zero key id means upsert/create; nonzero ids
// identify an existing key and are never recycled.
[[nodiscard]] Command setKeyframesCommand(std::vector<KeyframeEdit> edits);
[[nodiscard]] Command removeKeyframesCommand(std::vector<KeyframeRef> refs);
[[nodiscard]] Command moveKeyframesCommand(std::vector<KeyframeRef> refs, double deltaTime);
[[nodiscard]] Command insertKeyframeCommand(ParameterAddress address, double time);

}  // namespace nemo
