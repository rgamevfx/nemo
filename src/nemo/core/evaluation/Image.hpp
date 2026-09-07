#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
enum class ColorInterpretation { SceneLinear };

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
