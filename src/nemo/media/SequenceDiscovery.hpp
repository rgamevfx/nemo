#pragma once

// Numbered image-sequence discovery (issue #80).
//
// A Read/import selection is one path: either a literal file, an explicit
// '#'/'@' pattern (see resolveFramePath), or a numbered file that denotes the
// frame of a sequence ("shot.1001.exr"). This adapter turns that selection
// into validated coverage facts — available first/last, file count, and the
// compact set of missing frames inside the range — by scanning the parent
// directory once.
//
// Contract:
//   * Read-only and bounded: the scan is capped by directory entries and by
//     the discovered frame span. Reaching either cap is an explicit failure
//     (`status Failed`) — never a partial claim.
//   * Cancellable: `cancel` is polled while scanning, so a superseded request
//     stops early instead of finishing unbounded work.
//   * No guessing: a numbered run is the LAST maximal digit run of the file
//     name; a name with no numbered run and no marker is a still. A numbered
//     selection that denotes exactly one file is reported as a still (an
//     incomplete one-frame delivery is not silently reinterpreted as a
//     sequence), while an explicit '#'/'@' pattern is always a sequence.
//   * Errors name the offending path (repo rule: errors identify the
//     offending relationship).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nemo::media {

// One contiguous missing interval inside a discovered sequence, inclusive.
// Compact so a sparse sequence never allocates per missing frame.
struct SequenceFrameRange {
    std::int64_t first{0};
    std::int64_t last{0};
    [[nodiscard]] bool operator==(const SequenceFrameRange&) const = default;
};

enum class SequenceDiscoveryStatus {
    Still,     // one file, no numbered sequence
    Sequence,  // a numbered run with matching files
    // More than one numbered run of the selection has real sibling members, so
    // choosing one would silently reinterpret the other (a version token as the
    // frame, or vice versa). No facts are claimed; `detail` names the candidates
    // and the caller must ask for an explicit '#'/'@' pattern.
    Ambiguous,
    Failed,  // the scan could not produce validated facts (see `detail`)
};

// Validated coverage of one selection. `first`/`last` are the actual
// available members (never a presumed zero frame); `missingCount` counts
// frames inside [first, last] with no file, described compactly by `holes`.
struct SequenceDiscovery {
    SequenceDiscoveryStatus status{SequenceDiscoveryStatus::Still};
    // Canonical pattern ('#' marker run) for a sequence; the input path for a
    // still or a failure.
    std::string pattern;
    std::int64_t first{0};
    std::int64_t last{0};
    std::int64_t availableCount{0};
    std::int64_t missingCount{0};
    std::vector<SequenceFrameRange> holes;
    // Failure diagnostic; empty unless `status == Failed`.
    std::string detail;
};

struct SequenceDiscoveryLimits {
    // Directory entries examined before the scan fails explicitly.
    std::size_t maxDirectoryEntries{65536};
    // Inclusive frame span (last - first + 1) accepted for one sequence.
    std::int64_t maxFrameSpan{1000000};
};

// Discovers the numbered sequence `path` selects. `cancel` may be null; a
// cancelled scan returns Failed with detail "cancelled" and no facts.
[[nodiscard]] SequenceDiscovery discoverSequenceRange(const std::string& path,
                                                      const std::atomic<bool>* cancel = nullptr,
                                                      const SequenceDiscoveryLimits& limits = {});

}  // namespace nemo::media
