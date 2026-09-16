#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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

inline constexpr std::size_t kImageChannels = 4;

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
//   * the remaining fields are the image's naming and interpretation contract.
//     Every one of them is semantic: they participate in reuse identity.
//
// `ImageLayout` stays what it always was — raster-storage geometry for one
// executor allocation (width/height plus the samples' interpretation) — and is
// deliberately not duplicated here. `imageLayoutOf` is the one bridge.
//
// A description is complete by contract: `format` always has positive extents
// and `pixelAspect` is always finite and positive. Unknown geometry is never a
// zero sentinel — a source whose media cannot be inspected reports an explicit
// node- and path-identifying failure instead — so no consumer has to guess
// whether a number means "unknown square-ish zero" or "no image".
struct ImageDescription {
    Region format;
    Region dataBounds;
    float pixelAspect{1.0F};
    std::array<std::string, 4> channels{"R", "G", "B", "A"};
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
    // Named channels in storage order; the CPU inventory stores RGBA.
    std::array<std::string, 4> channels{"R", "G", "B", "A"};
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

// CPU reference pixel storage: straight (non-premultiplied) alpha, RGBA
// interleaved floats, row-major. The alpha convention is part of the image
// contract; merge (over) in this inventory composites straight-alpha values.
class CpuImage {
public:
    CpuImage() = default;

    explicit CpuImage(ImageLayout layout) : layout_(std::move(layout)) {
        pixels_.assign(
            static_cast<std::size_t>(layout_.width) * static_cast<std::size_t>(layout_.height) * kImageChannels, 0.0F);
    }
    CpuImage(int width, int height) : layout_{.width = width, .height = height} {
        pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kImageChannels, 0.0F);
    }

    [[nodiscard]] const ImageLayout& layout() const { return layout_; }
    // Update interpretation after an in-place color transform, without copying pixels.
    void setColorInterpretation(ColorInterpretation color) { layout_.color = color; }
    [[nodiscard]] int width() const { return layout_.width; }
    [[nodiscard]] int height() const { return layout_.height; }
    [[nodiscard]] static std::size_t channelCount() { return kImageChannels; }

    [[nodiscard]] const float* data() const { return pixels_.data(); }
    [[nodiscard]] float* data() { return pixels_.data(); }

    [[nodiscard]] std::array<float, kImageChannels> pixel(int x, int y) const {
        const float* p = &pixels_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(layout_.width) +
                                   static_cast<std::size_t>(x)) *
                                  kImageChannels];
        return {p[0], p[1], p[2], p[3]};
    }

    void setPixel(int x, int y, const std::array<float, kImageChannels>& value) {
        float* p = &pixels_[(static_cast<std::size_t>(y) * static_cast<std::size_t>(layout_.width) +
                             static_cast<std::size_t>(x)) *
                            kImageChannels];
        for (std::size_t c = 0; c < kImageChannels; ++c) {
            p[c] = value[c];
        }
    }

private:
    ImageLayout layout_;
    std::vector<float> pixels_;
};

// FNV-1a 64 over the storage bit patterns plus layout identity. Content
// addressing lets the plan detect identical images without carrying pixels.
[[nodiscard]] std::uint64_t cpuImageHash(const CpuImage& image);

}  // namespace nemo
