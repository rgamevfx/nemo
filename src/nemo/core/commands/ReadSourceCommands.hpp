#pragma once

// Read-node media registration and authored-choice commands (issues #61/#75).
//
// A Read node (persistent type `source`) names a document media reference
// through its `source` parameter, while the choices that belong to that Read —
// range mode and custom range, offset/step mapping, boundary/missing policies,
// explicit input transform and alpha association, migrated
// media-interpretation hints — are ordinary node parameters on the node itself,
// edited through the shared parameter commands and value gestures like any other
// node parameter. These commands own media identity and the shared-scope
// repairs:
//
//   * registerReadSourceCommand points a Read at a file (reusing the shared
//     reference and Media Bin entry for the same normalized path) and writes the
//     Read's own choices.
//   * relinkReadSourceCommand and reloadReadSourceCommand are the shared-scope
//     repairs: relink repoints a moved file, reload refreshes discovered facts
//     and advances the source content revision. Neither touches node choices, so
//     a custom trim survives both.
//
// Every entry point validates before it creates a command, so a rejected path,
// revision or probe leaves document state and history unchanged.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/MediaCatalog.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"

namespace nemo {

// Lexically normalized form used for reference de-duplication. Pure string
// math: it never consults the filesystem, so redo/resolve cannot depend on the
// process working directory.
[[nodiscard]] std::string normalizedSourcePath(std::string_view path);

// Resolves or creates the media reference for `path` (reusing the existing
// reference and Media Bin entry with the same normalized path), commits the
// classified `kind` and validated `probe`, and binds it to `target`, which
// addresses the Read's own `source` parameter. `target` may address a definition
// node or one occurrence of it (an exposed/nested Read), so binding a file on an
// occurrence writes that occurrence's override and leaves the definition, every
// other occurrence and every other authored choice untouched.
//
// `initializeOverrides` carries the Read's authored choices and is accepted ONLY
// for the first binding of a definition node (no occurrence, no existing source
// value); a replacement or an occurrence binding passes nullopt and preserves
// what is already authored. `time` is the gesture frame: an address that already
// has an animation channel receives a current-frame key through the animation
// owner's keyframe policy, any other address receives a static value, and the
// whole binding is ONE command and one history entry. `assignedKey`, when
// supplied, receives the resolved document source key.
[[nodiscard]] Command registerReadSourceCommand(ParameterAddress target, double time, std::string path,
                                                std::optional<ReadNodeOverrides> initializeOverrides, MediaKind kind,
                                                MediaProbeMetadata probe,
                                                std::shared_ptr<std::string> assignedKey = {});

// Relinks the reference a Read node already names, in place: the path changes
// while identity, interpretation and revision-bounded content identity advance,
// so every node sharing the reference recovers together and no authored state
// is lost. `expected` must still match the current reference or the command
// rejects with GraphError::StaleMediaSource. The new path's probe replaces the
// obsolete one on every catalog entry sharing the key.
[[nodiscard]] Command relinkReadSourceCommand(std::string sourceKey, SourceReference expected, std::string path,
                                              MediaKind kind, MediaProbeMetadata probe);

// Explicit Reload/Rescan of one shared source: rechecks the source's facts,
// advances the source content revision exactly once (so overwritten media is a
// new content identity and affected results are invalidated), and replaces the
// committed facts on every catalog entry sharing the key. `expected` must still
// match the current reference or the command rejects with
// GraphError::StaleMediaSource. Node choices are not touched: an automatic range
// follows the refreshed facts while a custom trim keeps its authored endpoints.
[[nodiscard]] Command reloadReadSourceCommand(std::string sourceKey, SourceReference expected, MediaKind kind,
                                              MediaProbeMetadata probe);

}  // namespace nemo
