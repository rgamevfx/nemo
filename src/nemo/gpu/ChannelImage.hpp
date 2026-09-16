#pragma once

// The native channel image layout, in one place (issues #90, #98).
//
// A native image always carries every STORED channel of its raster, in stored
// order, and never drops or pads one. Its device representation is chosen by
// the stored channel COUNT alone, so an arbitrary four-channel image (any names,
// any order, no RGBA roles required) gets the packed vector representation while
// one-, two- and three-channel data is not expanded into four components:
//
//   four stored channels   packed RGBA32F at the LOGICAL extent W×H: component
//                          c of texel (x, y) is stored channel c of logical
//                          pixel (x, y). One vector load or store per pixel.
//   any other count        R32_SFLOAT, one scalar plane per stored channel
//                          stacked vertically at W×(H*C): stored channel c of
//                          logical pixel (x, y) is the texel at (x, y + c*H).
//
// The LOGICAL height is the same word in both representations: it is what
// dispatch, every request origin/extent and every kernel coordinate use. Only
// the DEVICE extent differs, which is what nativeChannelHeight reports.
//
// Both formats are bound as formatless storage images (the executor requires
// shaderStorageImageRead/WriteWithoutFormat), so a kernel reads whichever
// representation a raster actually carries instead of a format it guessed from
// the description.
//
// These are pure layout queries shared by the executor, the media producers and
// the presentation/cache consumers: no device, allocator or state.

#include <cstdint>
#include <vulkan/vulkan.h>

namespace nemo::gpu {

// The device format of a raster with `storedChannelCount` stored channels: the
// packed four-channel format for exactly four channels, the scalar plane format
// for every other count. A count of zero is not a raster (callers validate the
// description first); it still maps to the scalar plane format so this query
// stays total.
[[nodiscard]] inline VkFormat nativeChannelFormat(std::uint32_t storedChannelCount) {
    return storedChannelCount == 4 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32_SFLOAT;
}

// The DEVICE height of a raster with `logicalHeight` logical rows and
// `storedChannelCount` stored channels: the logical height itself when the four
// channels are packed into one texel, otherwise one plane row per channel.
[[nodiscard]] inline std::uint64_t nativeChannelHeight(std::uint32_t logicalHeight, std::uint32_t storedChannelCount) {
    if (storedChannelCount == 4) {
        return logicalHeight;
    }
    return static_cast<std::uint64_t>(logicalHeight) * storedChannelCount;
}

// The components per texel of a real device image format: 4 for the packed
// representation, 1 for the scalar plane representation, 0 for a format that is
// NOT a native channel image. Consumers derive an image's actual storage from
// its own format through this query (never from a description), and must refuse
// the 0 case rather than guess.
[[nodiscard]] inline std::uint32_t nativeChannelComponents(VkFormat format) {
    if (format == VK_FORMAT_R32G32B32A32_SFLOAT) {
        return 4;
    }
    if (format == VK_FORMAT_R32_SFLOAT) {
        return 1;
    }
    return 0;
}

}  // namespace nemo::gpu
