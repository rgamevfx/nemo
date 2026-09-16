#pragma once

// Immutable viewer view intent (issue #98).
//
// A panel states WHAT the artist is looking at — the target, the time, the
// display demand and the layer/channel selection — and the worker resolves it
// against the CURRENT frame's described image. That is what removes the
// per-frame Describe -> GUI -> Render round trip: the one worker job that
// resolves the authored format also states the demand and executes it, so the
// region, sampling scale and channel list always describe the frame that is
// rendered rather than a neighbouring frame's guess.
//
// Everything here is Qt-free: the layer naming convention, the demand
// arithmetic (fit/zoom/pan/coverage/sampling) and the failure of a view that
// addresses nothing the current frame carries.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/core/document/Ids.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/ViewerResolution.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"

namespace nemo::eval {

// The layer name the display selectors state for root-spelled channels. The
// channel-name convention itself is core's (`nemo::channelLayer`/`channelLeaf`):
// a stored name's layer is the prefix before its last dot and a root channel has
// none, so a file writing "R" and one writing "rgba.R" are both addressed as
// layer "rgba" without renaming either.
inline constexpr std::string_view kViewRootLayerName{"rgba"};

// The spelling the channel selector uses for the whole layer, in place of one
// of its channel names.
inline constexpr std::string_view kViewCompositeChannel{"RGBA"};

// The layer one stored channel belongs to, as the selectors state it: the
// naming convention's layer, or the root layer's spelling for a root channel.
[[nodiscard]] inline std::string viewLayerName(std::string_view channel) {
    const std::string_view layer = channelLayer(channel);
    return std::string{layer.empty() ? kViewRootLayerName : layer};
}

// True when `channel` is a channel of `layer` (the exact membership test the
// layer and channel selectors use). A predicate over a whole raster's channel
// names, so it compares the naming convention's string_view directly and never
// builds a name.
[[nodiscard]] inline bool channelInViewLayer(std::string_view channel, std::string_view layer) {
    const std::string_view name = channelLayer(channel);
    return (name.empty() ? kViewRootLayerName : name) == layer;
}

// Real channels of one layer, in described order. Absence is never filled in
// with a guessed channel set.
[[nodiscard]] inline std::vector<std::string> viewLayerChannels(const ImageDescription& description,
                                                                std::string_view layer) {
    std::vector<std::string> result;
    for (const auto& channel : description.channels) {
        if (channelInViewLayer(channel, layer)) {
            result.push_back(channel);
        }
    }
    return result;
}

// Layers the described image carries, in described order and without repeats.
[[nodiscard]] inline std::vector<std::string> viewDescribedLayers(const ImageDescription& description) {
    std::vector<std::string> layers;
    for (const auto& channel : description.channels) {
        std::string layer = viewLayerName(channel);
        if (std::find(layers.begin(), layers.end(), layer) == layers.end()) {
            layers.push_back(std::move(layer));
        }
    }
    return layers;
}

// Why `layer` addresses no channel of `description`, or a plain statement when
// it does. One owner for the text the panel states and the worker reports.
[[nodiscard]] inline std::string viewLayerReason(const ImageDescription& description, std::string_view layer) {
    if (!viewLayerChannels(description, layer).empty()) {
        return "Named channel layer to evaluate";
    }
    const std::vector<std::string> names = viewDescribedLayers(description);
    if (names.empty()) {
        return "This target names no channel";
    }
    std::string text = "Layer '" + std::string{layer} + "' is unavailable in this target. Available: ";
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) {
            text += ", ";
        }
        text += names[index];
    }
    return text;
}

// Statement of a selection that is not a channel of the addressed layer.
[[nodiscard]] inline std::string viewChannelUnavailable(std::string_view channel, std::string_view layer) {
    return "Channel '" + std::string{channel} + "' is unavailable in layer '" + std::string{layer} + "'";
}

// The current frame's described image carries no channel or spelling this view
// addresses: nothing is presented, and no fallback channel or canvas is
// substituted for it. The description that refused the view travels with the
// failure, so the panel adopts the CURRENT frame's channels in the same worker
// job — a menu that offered the previous frame's layers while the selection was
// refused could not offer the layer this frame really carries.
struct ViewUnavailable : std::runtime_error {
    ViewUnavailable(std::string message, ImageDescription description)
        : std::runtime_error(std::move(message)), description(std::move(description)) {}

    // The described image the view was resolved against and refused by.
    ImageDescription description;
};

// One immutable view statement. It identifies the target and local time, the
// panel's own display demand and the named layer/channel selection; it carries
// NO geometry, because the domain it is resolved against is the current frame's
// own described format.
struct ViewIntent {
    NetworkId network{kInvalidNetwork};
    NodeId target{kInvalidNode};
    std::int64_t localTime{};
    ViewerResolution mode{ViewerResolution::Auto};
    // Whole-domain coverage: a coverage statement, never a display or quality
    // statement. The sampling mode and the view are retained either way.
    bool forceFullFrame{};
    // Scale relative to the image fitted into the panel's area, plus an
    // image-space pan, exactly as the panel's view control states them.
    double zoom{1.0};
    double panX{};
    double panY{};
    // Physical panel size the demand is resolved against (0 = no viewport yet).
    double viewportWidth{};
    double viewportHeight{};
    std::string layer{std::string{kViewRootLayerName}};
    std::string channel{std::string{kViewCompositeChannel}};

    friend bool operator==(const ViewIntent&, const ViewIntent&) = default;
};

// One intent resolved against one frame's described image: the concrete
// evaluation request the worker executes, plus the presentation-only display
// isolation the selection asked for.
struct ResolvedView {
    EvaluationRequest request;
    // RGBA keeps the whole premultiplied presentation; a single identified
    // primary channel is isolated in the presentation copy only.
    gpu::ViewerChannel presentationChannel{gpu::ViewerChannel::RGBA};
};

// Resolves `intent` against `description` (the target's authored description
// for exactly `intent.localTime`), consuming the panel's Auto hysteresis state
// in `resolution`. The returned request names the described format as its
// domain, the visible region at the resolved sampling density, and the channels
// of the addressed layer; an unavailable layer or channel throws
// ViewUnavailable with the reason, and nothing is substituted.
//
// Implemented by the viewer session that owns the worker-side resolution
// (`Viewer.cpp`), so the demand arithmetic lives beside the render that
// executes it.
[[nodiscard]] ResolvedView resolveViewIntent(const ViewIntent& intent, const ImageDescription& description,
                                             ViewerResolutionPolicy& resolution);

}  // namespace nemo::eval
