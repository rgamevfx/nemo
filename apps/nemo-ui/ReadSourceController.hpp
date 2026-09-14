#pragma once

// Read node media control (issues #61/#82): the presentation adapter behind the
// node-local file control the inspector hosts through ParameterEditorRegistry
// id "nemo.read.source".
//
// Ownership:
//   - Authored state stays in Document: the shared SourceReference plus the
//     Read's own node parameters (range mode, custom range, offset/step,
//     boundary and missing policies, input transform, alpha). VALUE edits are
//     owned by the shared parameter gesture (the panel/controller batch
//     gesture), which decides per address whether an existing channel authors
//     the current-frame key or the parameter takes the static value — this
//     adapter never edits values itself. It owns binding, probing, reload,
//     relink, clearing and the queries below.
//   - Probing/scanning/decoding stays with the media adapter's single import
//     worker (MediaImportService); this adapter is one requester of it
//     (requestReferenceProbe) and commits a probe only after the result
//     validates.
//   - Resolving the effective request (shared facts + node overrides +
//     discovered coverage) is core's `resolveSourceRequest`; this adapter never
//     re-derives mapping, precedence or coverage. The Start At editor uses
//     core's `startAtOffset`, not UI arithmetic.
//   - The native dialog belongs to NativeFileChooser; this adapter is the
//     requester, so it receives exactly its own outcomes, and a cancelled
//     browse publishes nothing.

#include "NativeFileChooser.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/media/InputColor.hpp"
#include "nemo/media/MediaImportService.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace nemo::ui {

class MediaLibraryModel;

class ReadSourceController final : public QObject {
    Q_OBJECT
public:
    ReadSourceController(nemo::ProjectSession& session, MediaLibraryModel& media, NativeFileChooser& chooser,
                         QObject* parent = nullptr);
    ~ReadSourceController() override;

    // Presentation state of the node's media reference, resolved at `frame`
    // (the panel's current frame), so the derived fields never lag a current
    // frame change. The editor's CONTROL values come from the shared parameter
    // inspector query (the effective-value owner); this map carries the media
    // FACTS and the core-resolved request facts. Keys:
    //   state ("empty"|"unresolved"|"ready"|"offline"), sourceKey, path,
    //   resolvedPath, pending, error;
    //   rangeMode ("auto"|"custom"), rangeFirst, rangeLast (authored custom
    //   endpoints as strings, "" in Auto), originalFirst/originalLast/
    //   originalCount/missingCount/coverageQuality (committed shared facts),
    //   selectedFirst/selectedLast (the effective selected interval);
    //   frameOffset, frameStep, startAt (the same mapping shown either way);
    //   beforePolicy, afterPolicy, missingPolicy;
    //   inputTransform, inputColorSpace, alphaMode, resolvedInputColorSpace,
    //   inputTransformOrigin, workingSpace;
    //   kind, width, height, pixelAspect, rate, frameSpan, precision, channels,
    //   offline, status (the core-resolved request status name or "");
    //   choiceRequired/choicePath/choicePattern/choiceFirst/choiceLast/
    //   choiceCount (an explicit numbered Sequence vs Single Image choice);
    //   shared (the source key is named by more than one Read node).
    // Facts cross as decimal strings so QML never rounds a frame; the control
    // values are the query's own exact `valueText`.
    // `instanceId` is the OCCURRENCE the editor was hosted with (empty keeps the
    // definition scope): occurrence overrides and occurrence animation compose
    // through the core owner, never through a global selection.
    Q_INVOKABLE QVariantMap info(const QString& networkId, const QVariant& nodeId, int frame = 0,
                                 const QVariant& instanceId = {}) const;

    // The searchable Input Transform list: the ACTIVE project config's own
    // color spaces (empty when the config cannot be resolved). Cached per
    // config path so a panel refresh does not reload the config.
    Q_INVOKABLE QStringList inputTransformChoices() const;

    // Browse for a file (or type a sequence pattern). A validated still or
    // explicit '#'/'@' pattern binds immediately; a numbered selection whose
    // run matches several files waits for the explicit Sequence/Single choice.
    // The request carries the OCCURRENCE the editor was hosted with and the
    // FROZEN frame, and the probe, the binding and every reported error stay on
    // exactly that scope.
    Q_INVOKABLE void chooseSource(const QString& networkId, const QVariant& nodeId, const QVariant& instanceId = {},
                                  int frame = 0);
    Q_INVOKABLE bool setSourcePath(const QString& networkId, const QVariant& nodeId, const QString& path,
                                   const QVariant& instanceId = {}, int frame = 0);
    // Commits the explicit choice for the pending numbered selection:
    // `sequence` binds the discovered pattern and aligns the mapping so the
    // available first frame is local zero; otherwise the literal file binds as
    // one still.
    // The binding is authored at the FROZEN frame of the recorded choice, so no
    // frame argument is needed here.
    Q_INVOKABLE bool confirmSourceChoice(const QString& networkId, const QVariant& nodeId, bool sequence,
                                         const QVariant& instanceId = {});

    // Relink the shared reference the node names, in place: identity, timing
    // and the node's own choices are preserved, so every node sharing it
    // recovers (the explicit SHARED repair). The scope/frame are the editor's
    // admitted occurrence and frozen frame, so the shared key repaired is the
    // one this node resolves there.
    Q_INVOKABLE void chooseRelinkSource(const QString& networkId, const QVariant& nodeId,
                                        const QVariant& instanceId = {}, int frame = 0);
    Q_INVOKABLE bool relinkSource(const QString& networkId, const QVariant& nodeId, const QString& path,
                                  const QVariant& instanceId = {}, int frame = 0);

    // Explicit Reload/Rescan of the node's shared media: re-discovers coverage,
    // advances the shared content revision once and replaces the committed
    // facts, so overwritten files and appended frames are picked up without a
    // global cache clear. Authored choices (custom range, offset, policies,
    // color) are preserved.
    Q_INVOKABLE bool reloadSource(const QString& networkId, const QVariant& nodeId, const QVariant& instanceId = {},
                                  int frame = 0);

    // Value edits (range, mapping, Start At, policies, input transform, alpha,
    // encoding hints) are NOT owned here: they go through the shared parameter
    // gesture, which authors the current-frame key on an animated channel and
    // the static value otherwise, batched atomically. This adapter keeps only
    // the derivation the gesture needs (the exact text Start At resolves to).
    Q_INVOKABLE QString startAtOffsetValue(const QString& networkId, const QVariant& nodeId, const QString& startAt,
                                           int frame = 0, const QVariant& instanceId = {}) const;

    [[nodiscard]] QString error() const { return error_; }

signals:
    // The node's media state changed (probe started/finished, command
    // committed, clearing). The hosted control re-reads info().
    void changed();

private:
    enum class ProbeAction { Register, Relink, Reload };

    [[nodiscard]] bool resolveNode(const QString& networkId, const QVariant& nodeId, nemo::NetworkId& network,
                                   nemo::NodeId& node) const;
    // Public-boundary occurrence resolution: unusable identities are refused
    // (naming the offending scope) instead of being normalised to 0.
    [[nodiscard]] bool resolveOccurrence(const QVariant& value, nemo::NetworkId network, nemo::NodeId node,
                                         nemo::NetworkInstanceId& occurrence, bool report) const;
    // The node's current (key, reference) pair; false when it names nothing the
    // document resolves.
    [[nodiscard]] bool currentBinding(nemo::NetworkId network, nemo::NodeId node, nemo::NetworkInstanceId occurrence,
                                      int frame, std::string& key, nemo::SourceReference& reference) const;
    // The EFFECTIVE source key bound at a frame/occurrence (a keyed `source`
    // publishes its current-frame value).
    [[nodiscard]] std::optional<std::string> effectiveSourceKey(nemo::NetworkId network, nemo::NodeId node,
                                                                nemo::NetworkInstanceId occurrence, int frame) const;
    [[nodiscard]] const nemo::NodeInstance* nodeInstance(nemo::NetworkId network, nemo::NodeId node) const;
    // The node's effective request, resolved by core. False when the authored
    // state cannot be resolved (the error is reported through `problem`).
    [[nodiscard]] bool effectiveRequest(nemo::NetworkId network, nemo::NodeId node, nemo::NetworkInstanceId instanceId,
                                        int frame, nemo::EffectiveSourceRequest& request, QString& problem) const;
    // The node with its CURRENT-FRAME parameters composed in, through the core
    // animation owner (`applyAnimationParameters`) — the same composition the
    // evaluator and the inspector query use, so no animation math is duplicated
    // here and a displayed/derived value is never the stale authored one.
    [[nodiscard]] std::optional<nemo::NodeInstance> effectiveNode(nemo::NetworkId network, nemo::NodeId node,
                                                                  nemo::NetworkInstanceId instanceId, int frame) const;
    // Retains one input-color context per project color policy and reports the
    // configuration's content identity ("" when the policy opens no config).
    // Freshness follows the agreed boundary: a same-path external config edit is
    // observed at project reopen/replacement (the observer below drops this
    // state), never by polling the filesystem per query, and a source Reload
    // stays source-only.
    [[nodiscard]] std::string inputColorIdentityFor(const std::string& configPath,
                                                    const std::string& workingSpace) const;
    // Drops every retained color/config presentation cache at a project
    // boundary (replacement/reopen or a policy/config-reference change).
    void invalidateColorState();
    static void sessionChanged(void* context) noexcept;
    void onSessionChanged();
    // The node's authored color choices plus the hint map core's resolver
    // merged (node scope first, else the shared reference's policy), resolved at
    // the same admitted scope/frame the probe uses.
    [[nodiscard]] nemo::media::InputColorChoice colorChoice(nemo::NetworkId network, nemo::NodeId node,
                                                            nemo::NetworkInstanceId occurrence, int frame) const;
    // Resolved Auto interpretation and its origin for the Color group: the
    // media color owner decides, this adapter never re-derives precedence.
    void describeInputColor(QVariantMap& out, const nemo::ReadNodeOverrides& overrides,
                            const nemo::EffectiveSourceRequest* request, const nemo::MediaProbeMetadata* probe,
                            nemo::MediaKind kind, const std::string& resolvedPath) const;
    // Requests one probe at the ADMITTED scope/frame; a Relink/Reload also
    // freezes the repair identity (key + shared reference) the completion must
    // still find.
    void beginProbe(ProbeAction action, nemo::NetworkId network, nemo::NodeId node, nemo::NetworkInstanceId occurrence,
                    int frame, QString path);
    // Publishes a probe result at the FROZEN scope/frame: a repair applies only
    // while the node still resolves the frozen key, and the frozen reference is
    // the expected value the core command itself validates.
    void handleProbe(std::uint64_t generation, ProbeAction action, nemo::NetworkId network, nemo::NodeId node,
                     nemo::NetworkInstanceId occurrence, int frame, QString path,
                     const nemo::media::MediaImportResult& result);
    // Commits a validated register proposal: the pattern (when the selection is
    // a sequence) is bound and aligned to the available first frame.
    [[nodiscard]] bool commitRegister(nemo::NetworkId network, nemo::NodeId node, nemo::NetworkInstanceId occurrence,
                                      int frame, const QString& path, const nemo::media::MediaImportResult& result,
                                      bool sequence);
    // ONE error slot per ADDRESS: an error is always reported at the admitted
    // (or explicitly requested) scope, never normalised onto the definition, so
    // one occurrence's rejection is never shown on another.
    void setError(const nemo::ParameterAddress& target, QString message);
    // Clears the slot only when it holds that exact scope's error: one scope's
    // success never erases a sibling scope's reported rejection.
    void clearError(const nemo::ParameterAddress& target);

    nemo::ProjectSession& session_;
    MediaLibraryModel& media_;
    NativeFileChooser& chooser_;
    QString error_;

    // One outstanding probe: user actions supersede it by generation, so a
    // late result can never edit a node the user has since changed.
    std::uint64_t pendingToken_{0};
    std::uint64_t generation_{0};
    // ONE address-valued identity per outstanding request / shown error: the
    // scope comparison is a single value (network + node + key + occurrence), so
    // a parallel-ID comparison cannot forget the occurrence.
    nemo::ParameterAddress pendingTarget_{};
    nemo::ParameterAddress errorTarget_{};
    // The REPAIR identity frozen when the outstanding Relink/Reload probe was
    // requested: the key the node resolved at that scope/frame and the shared
    // reference as it was then. A completion applies only while the node still
    // resolves that key, and hands the frozen reference to the core command as
    // its expected value — so a binding re-pointed at another source is refused
    // outright and a reference changed underneath is refused by the command's own
    // check. Empty for a Register (which authors a NEW binding).
    std::string pendingSourceKey_;
    nemo::SourceReference pendingReference_{};

    // The explicit numbered Sequence vs Single Image choice awaiting the
    // artist's decision. Presentation-local: the document changes only when a
    // choice is committed, and a later probe/selection supersedes it.
    struct PendingChoice {
        nemo::NetworkId network{};
        nemo::NodeId node{};
        std::uint64_t projectGeneration{0};
        QString literalPath;
        QString pattern;
        // The numbered run could not be determined from member evidence: only
        // the explicit single-image choice is offered, and `detail` asks for an
        // explicit pattern.
        bool ambiguous{false};
        nemo::NetworkInstanceId occurrence{};
        nemo::ParameterAddress target{};
        int frame{0};
        nemo::media::MediaImportResult result;
    };
    std::optional<PendingChoice> choice_;

    // The project the outstanding probe was started in: integer identities are
    // only comparable within one project, so a result that arrives after a
    // project replacement is dropped instead of publishing into the new one.
    std::uint64_t probeProjectGeneration_{0};

    mutable std::string colorSpaceCacheConfig_;
    mutable bool colorSpaceCacheValid_{false};
    mutable QStringList colorSpaceCache_;

    // Retained input-color context for the color summary: one validated
    // processor per resolved input space, rebuilt only when the project color
    // policy (config/working space) changes — never opened per node or frame.
    // ONE immutable input-color generation per project color policy: built at
    // the project boundary and dropped (never mutated) when that boundary moves,
    // so a read still using the old generation keeps it alive. Identity, the
    // searchable space list, file rules and the resolved interpretation all come
    // from this single snapshot.
    mutable std::shared_ptr<const nemo::media::InputColorCache> inputColorCache_;
    mutable std::string inputColorPolicyConfig_;
    mutable std::string inputColorPolicyWorking_;
    // OCIO CONTENT identity of the cache's configuration (Config::getCacheID):
    // the presentation caches key on it, so a config edited in place cannot
    // serve a stale color-space list or a stale resolved interpretation.
    mutable std::string inputColorIdentity_;
    // Project-boundary detection only (replacement/reopen and color-policy or
    // config-reference changes): a new project never reuses the previous
    // project's retained context, even when the path and stamp match.
    nemo::ProjectSession::Subscription colorSubscription_;
    std::uint64_t colorProjectGeneration_{0};
    nemo::ColorPolicy colorPolicySnapshot_{};
    std::string colorConfigSnapshot_;
};

}  // namespace nemo::ui
