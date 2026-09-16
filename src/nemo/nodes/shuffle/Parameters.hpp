#pragma once

// Authored Shuffle mapping (issue #90).
//
// Each output row names a source group (input1/input2), a stored channel, and
// its destination channel. The group's B/A selector resolves the physical
// input. Keeping the group reference preserves wire ownership across fanout,
// duplicate group selections, selector changes, history and hold animation.
// The in/out layer selectors describe the editor's visible channel groups.
//
// This header owns the typed interpretation and the authoring/admissibility
// rules of those fields; the descriptor (src/nemo/nodes/shuffle/Contribution.cpp)
// owns the schema they are read through, and the pixel/description
// implementation owns what the mapping means. Nothing here decides pixels.

#include <array>
#include <cstddef>
#include <string>

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {

// Resolved physical input or constant, shared by description and execution.
enum class ShuffleSource { InputB, InputA, Zero, One };

struct ShuffleRow {
    ShuffleSource source{ShuffleSource::InputB};
    // Exact stored channel name in the chosen input, used only by InputB/InputA.
    // A name the chosen input does not carry is zero-filled, never invented.
    std::string sourceChannel;
    // Exact output channel name; empty disables the row entirely.
    std::string outputChannel;

    [[nodiscard]] bool enabled() const { return !outputChannel.empty(); }
};

inline constexpr std::size_t kShuffleRows = 8;
inline constexpr std::size_t kShuffleGroupRows = 4;

// The whole mapping. Row order is the authored order and is what an editor
// shows; a source channel may fan out to any number of outputs.
struct ShuffleParameters {
    std::array<ShuffleRow, kShuffleRows> rows{};
};

// One row's authored parameter key, e.g. "sourceKind3". The schema declares
// exactly these keys; nothing else parses them.
[[nodiscard]] inline std::string shuffleRowKey(const char* field, std::size_t row) {
    return std::string{field} + std::to_string(row);
}

// Resolves the authored mapping into the typed form execution and description
// consume, and rejects what cannot be a mapping. Every failure identifies the
// node and the authored field (the shared `failNode` convention).
//
// Rejected here:
//   * an unknown `sourceKind{i}` choice (a persisted value this build cannot
//     interpret is never silently treated as a mapping);
//   * an enabled row whose source is B or A but names no source channel;
//   * two enabled rows claiming the same output channel — a conflicting
//     authored destination, not a fan-out (a source channel fans OUT to many
//     outputs; an output is written by exactly one row).
// A disabled row (empty `outputChannel`) contributes nothing and is not
// validated further: incomplete authored rows are how an editor holds a
// mapping it has not finished, and they create no channel.
[[nodiscard]] inline ShuffleParameters effectiveShuffle(const NodeCatalog& catalog, const NodeInstance& node,
                                                        const ParameterValues& effectiveParams) {
    ShuffleParameters params;
    std::array<ShuffleSource, 2> inputs{};
    for (std::size_t group = 0; group < inputs.size(); ++group) {
        const char* key = group == 0 ? "input1" : "input2";
        const std::string& selected = effectiveChoice(catalog, node, effectiveParams, key);
        if (selected != "B" && selected != "A")
            failNode(node, std::string("parameter '") + key + "' must be one of B, A, got '" + selected + "'");
        inputs[group] = selected == "A" ? ShuffleSource::InputA : ShuffleSource::InputB;
    }
    for (std::size_t index = 0; index < kShuffleRows; ++index) {
        ShuffleRow& row = params.rows[index];
        const std::string sourceKey = shuffleRowKey("sourceKind", index);
        const std::string& kind = effectiveChoice(catalog, node, effectiveParams, sourceKey.c_str());
        if (kind == "input1") {
            row.source = inputs[0];
        } else if (kind == "input2") {
            row.source = inputs[1];
        } else if (kind == "zero") {
            row.source = ShuffleSource::Zero;
        } else if (kind == "one") {
            row.source = ShuffleSource::One;
        } else {
            failNode(node,
                     "parameter '" + sourceKey + "' must be one of input1, input2, zero, one, got '" + kind + "'");
        }
        const std::string channelKey = shuffleRowKey("sourceChannel", index);
        row.sourceChannel = effectiveText(catalog, node, effectiveParams, channelKey.c_str());
        const std::string outputKey = shuffleRowKey("outputChannel", index);
        row.outputChannel = effectiveText(catalog, node, effectiveParams, outputKey.c_str());
        if (!row.enabled()) {
            continue;
        }
        if (row.source != ShuffleSource::Zero && row.source != ShuffleSource::One && row.sourceChannel.empty()) {
            failNode(node, "parameter '" + outputKey + "' maps output channel '" + row.outputChannel + "' but '" +
                               channelKey + "' names no source channel");
        }
    }
    // Authored destinations must be unique: two rows writing one channel would
    // make the result depend on row order rather than on the mapping.
    for (std::size_t left = 0; left < kShuffleRows; ++left) {
        if (!params.rows[left].enabled()) {
            continue;
        }
        for (std::size_t right = left + 1; right < kShuffleRows; ++right) {
            if (params.rows[right].enabled() && params.rows[right].outputChannel == params.rows[left].outputChannel) {
                failNode(node, "output channel '" + params.rows[left].outputChannel + "' is authored by both '" +
                                   shuffleRowKey("outputChannel", left) + "' and '" +
                                   shuffleRowKey("outputChannel", right) + "'");
            }
        }
    }
    return params;
}

}  // namespace nemo
