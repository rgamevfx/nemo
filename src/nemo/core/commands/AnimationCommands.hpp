#pragma once

#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Document.hpp"

#include <string>
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

// The keyed form of one value edit: the current-frame key for a parameter,
// preserving an existing key's identity, interpolation and tangents, and a new
// key's discrete-vs-continuous interpolation policy. This is the single owner of
// that policy, shared by the keyed/mixed value gestures and by command-level
// value authors such as a Read binding a media file.
[[nodiscard]] Keyframe keyframeForParameterEdit(const Document& document, const Document* priorSnapshot,
                                                const ParameterEdit& edit, double time);

// The keyed form of a whole batch; `priorSnapshot`, when supplied, restores the
// key identities captured by an earlier preview of the same gesture.
[[nodiscard]] std::vector<KeyframeEdit> keyframeEditsForParameters(const Document& document,
                                                                   const Document* priorSnapshot, double time,
                                                                   const std::vector<ParameterEdit>& edits);

// One atomic command for a value batch whose addresses are routed per address:
// `keyed` addresses author a current-frame key through the keyframe owner above,
// `staticValues` addresses hold an ordinary static value. Applied as one command,
// so a caller gets one history entry and one undo for the whole batch.
[[nodiscard]] Command parameterValueCommand(const Document& document, const Document* priorSnapshot, double time,
                                            const std::vector<ParameterEdit>& keyed,
                                            const std::vector<ParameterEdit>& staticValues);

}  // namespace nemo
