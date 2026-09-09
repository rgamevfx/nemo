#pragma once

#include <cstddef>
#include <cstdint>

namespace nemo::eval {

// Publication freshness is scoped to a destination. Interactive viewer work
// and explicit cache-range work may share a cache file, but neither stream
// may supersede the other's in-flight publication identity.
// Values 0 and 1 are the runtime defaults. Other bounded values are valid
// application-owned viewer destinations (for example, a second viewer).
enum class ViewerDestination : std::uint32_t {
    Interactive = 0,
    Cache = 1,
};

inline constexpr std::size_t kMaxViewerDestinations = 64;

}  // namespace nemo::eval
