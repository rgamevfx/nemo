#pragma once

// Read-node media registration (issue #61).
//
// A Read node (persistent type `source`) names a document media reference
// through its `source` parameter, exactly as `evalSource` and
// `eval::SourceSession` already resolve it. These commands turn a file or
// '#'/'@' sequence pattern chosen on the node's own control into that
// authored reference + Media Bin entry, reusing an existing reference for the
// same normalized path + interpretation so two Read nodes naming one file
// share one media reference. Every entry point is one validated, undoable
// command; a cancelled browse or a rejected path produces no command at all.

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/MediaCatalog.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace nemo {

// Authored timing and interpretation of a Read node's reference. `firstFrame`
// and `lastFrame` are the inclusive source-frame range of a '#'/'@' sequence;
// absent means unbounded (a still, or a sequence with no authored range).
struct ReadSourceTiming {
    std::int64_t frameOffset{0};
    std::int64_t frameStep{1};
    std::optional<std::int64_t> firstFrame;
    std::optional<std::int64_t> lastFrame;
    std::map<std::string, std::string> interpretation;

    [[nodiscard]] bool operator==(const ReadSourceTiming&) const = default;
};

// Lexically normalized form used for reference de-duplication. Pure string
// math: it never consults the filesystem, so redo/resolve cannot depend on the
// process working directory.
[[nodiscard]] std::string normalizedSourcePath(std::string_view path);

// Resolves or creates the media reference for `path` (reusing an existing
// reference and Media Bin entry with the same normalized path and
// interpretation), commits `probe` as the entry's probe when it is validated,
// and points `node`'s `source` parameter at the reference. `assignedKey`, when
// supplied, receives the resolved document source key.
[[nodiscard]] Command registerReadSourceCommand(NetworkId network, NodeId node, std::string path,
                                                ReadSourceTiming timing, MediaProbeMetadata probe,
                                                std::shared_ptr<std::string> assignedKey = {});

// Relinks the reference a Read node already names, in place: the path changes
// while identity, timing and interpretation are preserved, so every node
// sharing the reference recovers together and no authored state is lost.
// `expected` must still match the current reference or the command rejects with
// GraphError::StaleMediaSource. The new path's probe replaces the obsolete one
// on every catalog entry sharing the key.
[[nodiscard]] Command relinkReadSourceCommand(std::string sourceKey, SourceReference expected, std::string path,
                                              MediaProbeMetadata probe);

// Updates authored timing/range/interpretation of an existing reference.
// `expected` must still match or the command rejects with
// GraphError::StaleMediaSource.
[[nodiscard]] Command setReadSourceTimingCommand(std::string sourceKey, SourceReference expected,
                                                 ReadSourceTiming timing);

}  // namespace nemo
