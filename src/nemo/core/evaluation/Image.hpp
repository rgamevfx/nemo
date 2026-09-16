#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/Request.hpp"

namespace nemo {

// Image contract per spec section 10.4: dimensions, pixel aspect, named
// channels, precision, alpha association, and color interpretation are
// explicit. The CPU buffer below is correctness-reference storage ONLY
// (ADR-0004): it must not become the universal evaluation interface. A
// native executor consumes the same plan and records device-resident
// images through the GPU module's explicit lifetime/completion rules.
//
// Images carry NAMED channels (issue #90), not a fixed four: an alpha-only
// matte keeps exactly its "A" channel, a multilayer render keeps its
// auxiliary layers, and no RGB or alpha is manufactured to fill a
// four-channel assumption. `kImageChannels` is only the arity of the
// explicit RGBA *projection* existing effect math reads and writes through
// (CpuImage::pixel/setPixel); storage is interleaved in the declared channel
// order at `layout().channels.size()` channels per pixel.
inline constexpr std::size_t kImageChannels = 4;

// Wildcard channel vocabulary (issue #90): a capability or requirement that
// admits every named channel the image actually carries. An EMPTY channel
// list means the same thing everywhere in the image contracts — every channel
// the image names — never "no channels".
inline constexpr std::string_view kAnyChannelCapability = "*";

enum class Precision { Float32 };

// Composition results are scene-linear; viewing transforms are downstream
// operations and must not contaminate reusable results (spec section 8).
//
// `Data` (issue #75) marks a raster whose samples were deliberately left
// uninterpreted (a Read Raw/Data input: masks, normals, displacement). It is
// not a third color space and never a claim of scene-linearity: the Viewer may
// still present it, but no display transform may be applied twice to it.
enum class ColorInterpretation { SceneLinear, DisplayReferred, Data };

[[nodiscard]] constexpr const char* colorInterpretationName(ColorInterpretation color) {
    switch (color) {
    case ColorInterpretation::SceneLinear:
        return "scene-linear";
    case ColorInterpretation::DisplayReferred:
        return "display-referred";
    case ColorInterpretation::Data:
        return "data";
    }
    return "scene-linear";
}

// Alpha association declared by an image description (spec section 10.4).
// Straight is the CPU reference inventory's convention; a premultiplied source
// or result declares itself instead of being silently converted.
enum class ImageAssociation { Straight, Premultiplied };

[[nodiscard]] constexpr const char* imageAssociationName(ImageAssociation association) {
    return association == ImageAssociation::Premultiplied ? "premultiplied" : "straight";
}

// The layer a channel name belongs to (issue #90). A stored channel name is
// the media's own name, verbatim: "R" is a root channel with no layer, and
// "rgba.R" or "depth.Z" name the same leaves inside a named layer. Nothing is
// renamed, prefixed or normalized.
[[nodiscard]] constexpr std::string_view channelLayer(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : name.substr(0, dot);
}

[[nodiscard]] constexpr std::string_view channelLeaf(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

namespace channelsDetail {

[[nodiscard]] constexpr char lowerAscii(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] constexpr bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (lowerAscii(left[index]) != lowerAscii(right[index])) {
            return false;
        }
    }
    return true;
}

// True when `name`'s leaf names primary-color role `role` (0 R, 1 G, 2 B,
// 3 alpha) inside a layer that may carry the primary set: the root layer, or
// the conventional "rgba"/"rgb" layer. Both the single-letter ("R") and the
// spelled-out ("red") spelling are accepted, case-insensitively, so a file
// that writes "rgba.red" or "red" is identified the same way. Any other layer
// stays data: only an identified primary RGB set is color-managed, and
// auxiliary/non-RGB channels bypass the input color transform.
[[nodiscard]] constexpr bool isPrimaryRoleName(std::string_view name, std::size_t role) {
    constexpr std::array<std::string_view, 4> letters{"r", "g", "b", "a"};
    constexpr std::array<std::string_view, 4> words{"red", "green", "blue", "alpha"};
    if (role >= letters.size()) {
        return false;
    }
    const std::string_view layer = channelLayer(name);
    if (!layer.empty() && !equalsIgnoreCase(layer, "rgba") && !equalsIgnoreCase(layer, "rgb")) {
        return false;
    }
    const std::string_view leaf = channelLeaf(name);
    return equalsIgnoreCase(leaf, letters[role]) || equalsIgnoreCase(leaf, words[role]);
}

}  // namespace channelsDetail

// The stored index of the exactly named channel, or -1 when the image does not
// carry that name (issue #90). Lookup is exact string equality: names are
// authored/stored identifiers, never patterns. Resolve outside pixel loops.
[[nodiscard]] inline int channelIndex(std::span<const std::string> channels, std::string_view name) {
    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (std::string_view{channels[index]} == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

[[nodiscard]] inline bool hasChannel(std::span<const std::string> channels, std::string_view name) {
    return channelIndex(channels, name) >= 0;
}

// Stored indices of the identified primary channels (R, G, B, alpha), -1 for a
// role the image does not carry. This is the one place the RGBA projection of
// pixel/setPixel and the color-managed primary set are resolved, so no pixel
// loop ever looks a channel name up (issue #90).
[[nodiscard]] inline std::array<int, 4> rgbaChannelIndices(std::span<const std::string> channels) {
    std::array<int, 4> indices{-1, -1, -1, -1};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        for (std::size_t role = 0; role < indices.size(); ++role) {
            if (indices[role] < 0 && channelsDetail::isPrimaryRoleName(channels[index], role)) {
                indices[role] = static_cast<int>(index);
                break;
            }
        }
    }
    return indices;
}

// True when the image identifies a complete primary RGB set, which is the
// precondition for managing its color. Anything else — an alpha-only matte, a
// normals or depth layer — is uninterpreted data and bypasses the input color
// transform (issue #90, #87 story 9).
[[nodiscard]] inline bool hasPrimaryRgb(std::span<const std::string> channels) {
    const std::array<int, 4> indices = rgbaChannelIndices(channels);
    return indices[0] >= 0 && indices[1] >= 0 && indices[2] >= 0;
}

// The logical description of one node's image (issue #88, spec section 10.4):
// what the image *is*, independently of which raster currently backs it.
//
//   * `format` is the image's logical format — the rectangle the image claims
//     to be, in full-resolution image coordinates. Source display windows are
//     normalized to origin 0 under the approved window policy, so a source's
//     format is `{0, 0, displayWidth, displayHeight}`.
//   * `dataBounds` is the signed rectangle that actually holds data. It may
//     extend outside `format` (negative coordinates and overscan survive
//     normalization), and it may be empty (`width <= 0 || height <= 0`), which
//     describes a valid connected image whose every pixel is transparent —
//     never an absent input. `dataBounds` is the support of the image: every
//     sample outside it is transparent black, and a sample inside it is kept
//     even when it lies outside `format` (overscan IS data). The executor
//     enforces this centrally on every produced raster, so no effect's pixel
//     math has to remember it.
//   * `channels` names the stored channels in storage order (issue #90). The
//     default four are the conventional RGBA image; an alpha-only or multilayer
//     image states its own names instead, and no name is invented. Channel
//     names are semantic: they participate in reuse identity.
//   * the remaining fields are the image's naming and interpretation contract.
//     Every one of them is semantic: they participate in reuse identity.
//
// `ImageLayout` stays what it always was — raster-storage geometry for one
// executor allocation (width/height plus the samples' interpretation) — and is
// deliberately not duplicated here. `imageLayoutOf` is the one bridge.
//
// A description is complete by contract: `format` always has positive extents,
// `pixelAspect` is always finite and positive, and at least one channel is
// named. Unknown geometry is never a zero sentinel — a source whose media
// cannot be inspected reports an explicit node- and path-identifying failure
// instead — so no consumer has to guess whether a number means "unknown
// square-ish zero" or "no image".
struct ImageDescription {
    Region format;
    Region dataBounds;
    float pixelAspect{1.0F};
    std::vector<std::string> channels{"R", "G", "B", "A"};
    Precision precision{Precision::Float32};
    ImageAssociation association{ImageAssociation::Straight};
    ColorInterpretation color{ColorInterpretation::SceneLinear};

    [[nodiscard]] bool operator==(const ImageDescription&) const = default;
};

// True when a description carries no image format. That is never admissible:
// the planner rejects it, so a described image always has a real format.
[[nodiscard]] inline bool hasNoImageFormat(const ImageDescription& description) {
    return description.format.width <= 0 || description.format.height <= 0;
}

struct ImageLayout {
    int width{0};
    int height{0};
    float pixelAspect{1.0F};
    // Named channels in storage order; pixels are interleaved in exactly this
    // order at one float per channel (issue #90).
    std::vector<std::string> channels{"R", "G", "B", "A"};
    Precision precision{Precision::Float32};
    ColorInterpretation color{ColorInterpretation::SceneLinear};

    [[nodiscard]] bool operator==(const ImageLayout&) const = default;
};

// Raster-storage geometry for one allocation that carries this description's
// interpretation: the description's channel naming, precision, pixel aspect and
// colour interpretation are adopted, while width/height stay the executor's
// actual raster extents (a request region at a sampling scale, a halo, a
// resident rectangle). Effects build their output layout through this bridge, so
// an interpreted image — a Data bypass, a premultiplied source — keeps its
// meaning instead of being reset to the default scene-linear interpretation.
[[nodiscard]] inline ImageLayout imageLayoutOf(const ImageDescription& description, int width, int height) {
    return ImageLayout{
        width, height, description.pixelAspect, description.channels, description.precision, description.color};
}

// CPU reference pixel storage: straight (non-premultiplied) alpha, interleaved
// float32 in the declared channel order, row-major (issue #90). The alpha
// convention is part of the image contract; merge (over) in this inventory
// composites straight-alpha values. Storage is dynamic: `channelCount()` is the
// number of named channels the layout declares, not four.
//
// `pixel`/`setPixel` stay the explicit RGBA *projection* existing effect math
// already uses: the four values are the identified primary channels in R, G, B,
// alpha order, and a write only touches channels the image actually stores — a
// projected value for a role that does not exist is discarded rather than
// stored under a manufactured name. A named channel read (`channel`) never
// invents anything: a missing name reads as 0.
//
// The projection's missing-role defaults are fixed so CPU and native agree bit
// for bit: a missing RGB role reads 0, and a missing alpha role reads 1 (opaque)
// for an image that identifies color at all (it stores at least one of R, G, B)
// and 0 (transparent) for a data-only image that identifies no color.
class CpuImage {
public:
    // The default image is the conventional four-channel one, and its
    // projection is resolved like any other so a default-constructed buffer
    // never reads channels it does not describe.
    CpuImage() { resolveProjection(); }

    explicit CpuImage(ImageLayout layout) : layout_(std::move(layout)) {
        resolveProjection();
        pixels_.assign(static_cast<std::size_t>(layout_.width) * static_cast<std::size_t>(layout_.height) *
                           layout_.channels.size(),
                       0.0F);
    }
    CpuImage(int width, int height) : layout_{.width = width, .height = height} {
        resolveProjection();
        pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * layout_.channels.size(),
                       0.0F);
    }

    [[nodiscard]] const ImageLayout& layout() const { return layout_; }
    // Update interpretation after an in-place color transform, without copying pixels.
    void setColorInterpretation(ColorInterpretation color) { layout_.color = color; }
    [[nodiscard]] int width() const { return layout_.width; }
    [[nodiscard]] int height() const { return layout_.height; }
    [[nodiscard]] std::size_t channelCount() const { return layout_.channels.size(); }
    // Resolved once at construction, so no pixel loop looks a name up.
    [[nodiscard]] const std::array<int, 4>& rgbaIndices() const { return rgba_; }

    [[nodiscard]] const float* data() const { return pixels_.data(); }
    [[nodiscard]] float* data() { return pixels_.data(); }

    // One stored channel by index (0 <= index < channelCount()), 0 outside the
    // raster or for an index the image does not store.
    [[nodiscard]] float channel(int x, int y, int index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= layout_.channels.size() || x < 0 || y < 0 ||
            x >= layout_.width || y >= layout_.height) {
            return 0.0F;
        }
        return pixels_[pixelOffset(x, y) + static_cast<std::size_t>(index)];
    }

    void setChannel(int x, int y, int index, float value) {
        if (index < 0 || static_cast<std::size_t>(index) >= layout_.channels.size() || x < 0 || y < 0 ||
            x >= layout_.width || y >= layout_.height) {
            return;
        }
        pixels_[pixelOffset(x, y) + static_cast<std::size_t>(index)] = value;
    }

    // Every stored channel of one sample becomes zero (transparent black for
    // color and alpha, numeric zero for data). The one clear used by the
    // executor's data-window guard, which must not leave an auxiliary channel
    // behind.
    void clearPixel(int x, int y) {
        if (x < 0 || y < 0 || x >= layout_.width || y >= layout_.height) {
            return;
        }
        float* p = &pixels_[pixelOffset(x, y)];
        std::fill(p, p + layout_.channels.size(), 0.0F);
    }

    // Explicit RGBA projection (see the class comment): x/y must address this
    // raster. Missing color roles read as 0; a missing alpha role reads as
    // `missingAlpha_`.
    [[nodiscard]] std::array<float, kImageChannels> pixel(int x, int y) const {
        std::array<float, kImageChannels> value{0.0F, 0.0F, 0.0F, missingAlpha_};
        if (layout_.channels.empty()) {
            return value;
        }
        const float* p = &pixels_[pixelOffset(x, y)];
        for (std::size_t role = 0; role < kImageChannels; ++role) {
            const int index = rgba_[role];
            if (index >= 0) {
                value[role] = p[static_cast<std::size_t>(index)];
            }
        }
        return value;
    }

    void setPixel(int x, int y, const std::array<float, kImageChannels>& value) {
        if (layout_.channels.empty()) {
            return;
        }
        float* p = &pixels_[pixelOffset(x, y)];
        for (std::size_t role = 0; role < kImageChannels; ++role) {
            const int index = rgba_[role];
            if (index >= 0) {
                p[static_cast<std::size_t>(index)] = value[role];
            }
        }
    }

private:
    // Resolved once: the primary roles this layout stores, and the alpha a
    // projection without an alpha role reports (1 for a color image, 0 for a
    // data-only image).
    void resolveProjection() {
        rgba_ = rgbaChannelIndices(layout_.channels);
        missingAlpha_ = rgba_[0] >= 0 || rgba_[1] >= 0 || rgba_[2] >= 0 ? 1.0F : 0.0F;
    }

    [[nodiscard]] std::size_t pixelOffset(int x, int y) const {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(layout_.width) + static_cast<std::size_t>(x)) *
               layout_.channels.size();
    }

    ImageLayout layout_;
    std::array<int, kImageChannels> rgba_{-1, -1, -1, -1};
    float missingAlpha_{1.0F};
    std::vector<float> pixels_;
};

// FNV-1a 64 over the storage bit patterns plus layout identity. Content
// addressing lets the plan detect identical images without carrying pixels.
[[nodiscard]] std::uint64_t cpuImageHash(const CpuImage& image);

}  // namespace nemo
