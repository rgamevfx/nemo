#pragma once

// Shared schema-building helpers for the built-in node modules (issue #83).
//
// These are schema facts only: no callback, Qt object or GPU handle lives here.
// A node module keeps its own descriptor, pixel implementation and editor
// declarations; the conventions several built-ins genuinely share (the
// optional-mask effect contract, the Mix control) are stated once here so the
// built-ins cannot drift from each other.

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nemo/core/nodes/NodeCatalog.hpp"

namespace nemo::nodes {

// Every built-in declares the full-quality contract at the three sampling
// scales; a Read additionally participates in time. Region support is declared
// alongside it: every built-in that produces pixels states its spatial
// dependency rule (NodeContribution::inputRequirements) instead of refusing
// regions, and keeps the shared "unchanged image properties" description default
// unless it really changes the image's meaning (issue #88).
//
// Channels are a NAMED vocabulary (issue #90): the wildcard says this node
// carries every named channel the image actually has — an alpha-only matte, a
// multilayer render, an auxiliary data layer — instead of claiming a fixed
// four. A built-in that must restrict its vocabulary states the names (or the
// legacy concatenated single-letter sets, e.g. "RGBA") it really accepts.
[[nodiscard]] inline NodeCapabilities builtinCapabilities(bool temporal = false) {
    return NodeCapabilities{.samplingScales = {1, 2, 4},
                            .qualityModes = {Quality::Full},
                            .channels = {std::string{kAnyChannelCapability}},
                            .supportsRegion = true,
                            .temporal = temporal};
}

// Every native effect shares the same optional-mask contract: a required image
// at input 0 and an optional mask at input 1. The pixel math lives with each
// contribution; only the schema is shared.
[[nodiscard]] inline std::vector<PortSpec> effectImageInputs() {
    return {{PortKind::Image, "image", false}, {PortKind::Mask, "mask", true}};
}

// The mask footer is one schema row: the channel choice and its invert toggle
// render on one line (issue #102), exactly like every other grouped row, so the
// host needs no effect-name branch for it. The channel choice leads the row, so
// the group's label is the row name.
[[nodiscard]] inline std::vector<ParameterSpec> maskParameterSpecs() {
    return {
        {.name = "maskChannel",
         .type = ParameterType::Choice,
         .defaultValue = ParameterValue{ChoiceValue{"A"}},
         .choices = {"none", "R", "G", "B", "A"},
         .label = "Mask Channel",
         .section = "Mask",
         .editor = {},
         .row = "Mask"},
        {.name = "invertMask",
         .type = ParameterType::Boolean,
         .defaultValue = ParameterValue{false},
         .label = "Invert",
         .section = "Mask",
         .editor = {},
         .row = "Mask"},
    };
}

// Mix is an ordinary effect control, not a mask control: it must stay reachable
// and usable with no mask connected (stories 38 and 71). Each effect appends it
// to its primary list and the shared mask specs stay the only "Mask" section
// members.
[[nodiscard]] inline ParameterSpec mixParameterSpec(std::string section) {
    // Mix declares no hard range (issue #103): the shared blend is
    // `source + (effect - source) * mix` in every executor, which is defined
    // for every finite factor, so 0..1 is the useful slider travel rather than
    // an authoring validity rule. A typed, scrubbed or stepped value outside
    // the travel is stored and evaluated exactly; only the finite,
    // float-representable requirement remains.
    return {.name = "mix",
            .type = ParameterType::Float,
            .defaultValue = ParameterValue{1.0},
            .label = "Mix",
            .section = std::move(section),
            .editor = {},
            .softMinimum = 0.0,
            .softMaximum = 1.0};
}

[[nodiscard]] inline std::vector<ParameterSpec> withMaskParameters(std::vector<ParameterSpec> specific) {
    auto mask = maskParameterSpecs();
    specific.reserve(specific.size() + mask.size());
    for (auto& parameter : mask)
        specific.push_back(std::move(parameter));
    return specific;
}

// Runs a node's typed interpretation for authoring: its failure message, or
// nullopt when the resolved parameters are admissible. No graph is evaluated and
// no image is produced.
template <typename Resolve>
[[nodiscard]] std::optional<std::string> authoringAdmissibility(Resolve&& resolve) {
    try {
        std::forward<Resolve>(resolve)();
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string{error.what()};
    }
}

}  // namespace nemo::nodes
