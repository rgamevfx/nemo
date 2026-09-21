#pragma once

#include "ParameterInteraction.hpp"
#include "ViewerRuntime.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/Request.hpp"
#include "nemo/core/evaluation/ViewerResolution.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/ViewerDestination.hpp"
#include "nemo/gpu/ViewerPresentation.hpp"
#include <QPointer>
#include <QRectF>
#include <QSizeF>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <chrono>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nemo::ui {
class RotoController;
class ViewerItem;
class WindowPresentationState;
// GUI-thread presentation state. The explicitly composed ProjectSession owns
// the live Document/history; this facade only submits commands and reads it.
// Scenegraph reads happen only during updatePaintNode, while Qt blocks the GUI
// thread. The worker sees immutable Document copies.
class ViewerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasSource READ hasSource NOTIFY sourceChanged)
    // Platform drag threshold in logical pixels. The shared numeric editor uses
    // the same distance the rest of the application treats as a drag, without
    // depending on a Qt version that exposes style hints to QML.
    Q_PROPERTY(int dragDistance READ dragDistance CONSTANT)
    Q_PROPERTY(QSizeF sourceSize READ sourceSize NOTIFY sourceChanged)
    Q_PROPERTY(double pixelAspect READ pixelAspect NOTIFY sourceChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY sourceChanged)
    // Actionable viewer state, never progress: the guidance for an empty or
    // unavailable target, a failure, or a cancelled/queued command. A normal
    // render states no text (issue #98) — the owner-approved removal of the
    // transient progress/node-name flash — so a pending frame leaves this
    // empty while the retained frame stays on screen.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)
    Q_PROPERTY(QString renderState READ renderState NOTIFY statusChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY statusChanged)
    Q_PROPERTY(bool outdated READ outdated NOTIFY statusChanged)
    // Whether a completed frame is retained. A panel needs this beside
    // renderState to tell a stale image from an empty area: a failed or pending
    // request may still have an image to show, while an unbound Read has none.
    Q_PROPERTY(bool hasPresentation READ hasPresentation NOTIFY frameArrived)
    Q_PROPERTY(QString resolutionMode READ resolutionMode WRITE setResolutionMode NOTIFY resolutionChanged)
    // Coverage switch, panel-local like every other view property: when it is
    // on, the request covers the whole image domain instead of the region the
    // panel is looking at, at the unchanged sampling mode. It changes what is
    // rendered, never the display scale, the pan or the sampling density.
    Q_PROPERTY(bool forceFullFrame READ forceFullFrame WRITE setForceFullFrame NOTIFY forceFullFrameChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(QPointF pan READ pan WRITE setPan NOTIFY panChanged)
    Q_PROPERTY(int frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(int effectiveScale READ effectiveScale NOTIFY effectiveScaleChanged)
    Q_PROPERTY(QRectF presentedRegion READ presentedRegion NOTIFY frameArrived)
    // Full-resolution image domain in effect: the retained frame's own domain
    // while one is displayed, otherwise the ACTUAL format the target's
    // description states for the current frame (issue #88). Display math reads
    // this instead of the media-only source size; a target whose description
    // states no format reports no size rather than a substituted canvas.
    Q_PROPERTY(QSizeF compositionSize READ compositionSize NOTIFY frameArrived)
    Q_PROPERTY(QString viewerTargetName READ viewerTargetName NOTIFY viewerTargetChanged)
    Q_PROPERTY(QString viewerTargetId READ viewerTargetId NOTIFY viewerTargetChanged)
    Q_PROPERTY(QString rootNetworkId READ rootNetworkId NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphNodes READ graphNodes NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphEdges READ graphEdges NOTIFY graphChanged)
    Q_PROPERTY(QVariantList nodeCatalog READ nodeCatalog NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList timelineClips READ timelineClips NOTIFY timelineChanged)
    Q_PROPERTY(qulonglong queued READ queued NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong dropped READ dropped NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong staleRejected READ staleRejected NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong completed READ completed NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cacheQueued READ cacheQueued NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cacheDropped READ cacheDropped NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cacheErrors READ cacheErrors NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cachePublished READ cachePublished NOTIFY schedulerChanged)
    Q_PROPERTY(QString cacheError READ cacheError NOTIFY schedulerChanged)
    // Panel-instance routing and transport (issue #47). Every one of these is
    // panel-local: it reads and writes only this controller's destination.
    Q_PROPERTY(bool hasDestination READ hasDestination NOTIFY destinationChanged)
    Q_PROPERTY(qulonglong destinationId READ destinationId NOTIFY destinationChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(int inFrame READ inFrame NOTIFY marksChanged)
    Q_PROPERTY(int outFrame READ outFrame NOTIFY marksChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY frameRateChanged)
    Q_PROPERTY(QString channel READ channel NOTIFY displayChanged)
    Q_PROPERTY(QString layer READ layer NOTIFY displayChanged)
    // Named channel layers this panel can address, from the target's described
    // channels (plus the root layer and the current selection). A layer is never
    // fabricated: before an answer the root layer is the only entry.
    Q_PROPERTY(QStringList layers READ availableLayers NOTIFY displayChanged)
    // Display choices of the selected layer: the composite first, then that
    // layer's real channel names in declared order.
    Q_PROPERTY(QStringList displayChannels READ availableChannels NOTIFY displayChanged)
    // Why the current layer addresses no channel of the target, or a plain
    // statement when it does. A layer the target does not carry is reported,
    // never silently replaced by another layer.
    Q_PROPERTY(QString layerReason READ layerReason NOTIFY displayChanged)
    Q_PROPERTY(QString timecode READ timecode NOTIFY frameChanged)
public:
    explicit ViewerController(ViewerRuntime* runtime, nemo::ProjectSession& session, ParameterInteraction& interaction);
    ~ViewerController() override;
    // The panel owns its query adapter; the application-owned session outlives
    // every QML panel. No per-panel document or animation history is created.
    Q_INVOKABLE QObject* createAnimationModel(QObject* owner);
    // One adapter per node and context group: inspector and viewer share
    // selection/drafts without coupling independent group clocks.
    Q_INVOKABLE QObject* createRotoControllerFor(const QString& networkValue, const QVariant& nodeValue,
                                                 const QString& group, QObject* owner);
    // Panel-instance scheduler destination. Panel destinations are values >= 2;
    // Interactive and Cache stay reserved. Without a destination this
    // controller is a pure command/metadata facade: it never probes, submits,
    // or consumes a result.
    void setDestination(std::optional<eval::ViewerDestination> destination);
    [[nodiscard]] std::optional<eval::ViewerDestination> destination() const { return destination_; }
    Q_INVOKABLE void openSource(const QString& path);
    Q_INVOKABLE void setResolutionMode(const QString& mode);
    // Forces whole-domain coverage for this panel's requests. The sampling mode
    // and the panel's display transform are deliberately retained: this is a
    // coverage statement, not a view or a quality statement.
    Q_INVOKABLE void setForceFullFrame(bool force);
    Q_INVOKABLE void setZoom(double zoom);
    Q_INVOKABLE void zoomBy(double factor);
    Q_INVOKABLE void setPan(QPointF pan);
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void setFrame(int frame);
    // Graph editing is command-owned. IDs cross the QML boundary as decimal
    // strings so JavaScript never rounds a 64-bit identity.
    Q_INVOKABLE QString createGraphNode(const QString& networkId, const QString& type, const QString& name, double x,
                                        double y, const QVariant& anchorId, const QVariantList& shiftedNodes);
    Q_INVOKABLE QString insertGraphNode(const QString& networkId, const QVariant& edgeId, const QString& type,
                                        const QString& name, double x, double y);
    Q_INVOKABLE bool insertExistingGraphNodeOnEdge(const QString& networkId, const QVariant& nodeId,
                                                   const QVariant& edgeId, double x, double y);
    Q_INVOKABLE bool deleteGraphNodes(const QString& networkId, const QVariantList& nodeIds);
    Q_INVOKABLE bool connectOrReplaceGraph(const QString& networkId, const QVariant& fromNode, int fromPort,
                                           const QVariant& toNode, int toPort);
    Q_INVOKABLE bool rewireGraphEdge(const QString& networkId, const QVariant& edgeId, const QVariant& fromNode,
                                     int fromPort, const QVariant& toNode, int toPort);
    // Atomic swap of two declared input slots. Both occupied exchanges the
    // sources, one occupied moves the occupancy, and an empty pair or two edges
    // from the same source are refused without touching history. The mask slot,
    // operation parameters and node layout are retained.
    Q_INVOKABLE bool swapNodeInputs(const QString& networkId, const QVariant& nodeId, int firstPort, int secondPort);
    // Presentation query for the Merge A/B roles: whether a meaningful swap
    // exists, plus the connected state of each declared input.
    Q_INVOKABLE QVariantMap nodeInputOccupancy(const QString& networkId, const QVariant& nodeId) const;
    Q_INVOKABLE bool disconnectGraphEdge(const QString& networkId, const QVariant& edgeId);
    Q_INVOKABLE bool commitGraphMove(const QString& networkId, const QVariantList& positions);
    Q_INVOKABLE bool commitGraphRoute(const QString& networkId, const QVariant& edgeId, const QVariantList& points);
    Q_INVOKABLE bool insertGraphRoutePoint(const QString& networkId, const QVariant& edgeId, int index, double x,
                                           double y);
    Q_INVOKABLE bool moveGraphRoutePoint(const QString& networkId, const QVariant& edgeId, int index, double x,
                                         double y);
    Q_INVOKABLE bool removeGraphRoutePoint(const QString& networkId, const QVariant& edgeId, int index);
    // Collapse/unpack are shared hierarchy commands. The controller only
    // returns identities read back from the committed session snapshot.
    Q_INVOKABLE QString collapseSelection(const QString& networkId, const QVariantList& nodeIds,
                                          const QString& name = QStringLiteral("Subnet"));
    Q_INVOKABLE bool unpackInstance(const QString& instanceId);
    // Exposed parameters belong to the definition network. The occurrence
    // query resolves the definition from a selected subnet node and reports
    // whether that definition is owned locally or shared with other instances,
    // so the popout and inspector never guess at link state.
    Q_INVOKABLE QVariantMap subnetExposure(const QString& networkId, const QVariant& nodeId) const;
    // Promotes a definition-local parameter to a typed exposed control. An
    // empty `name` derives a unique label from the source schema. Index -1
    // appends; an explicit insertion position is part of the same undo step.
    Q_INVOKABLE bool promoteParameter(const QString& networkId, const QVariant& nodeId, const QString& key,
                                      const QString& name = {}, int index = -1);
    Q_INVOKABLE bool renameExposedParameter(const QString& networkId, const QVariant& parameterId, const QString& name);
    Q_INVOKABLE bool removeExposedParameter(const QString& networkId, const QVariant& parameterId);
    Q_INVOKABLE bool moveExposedParameter(const QString& networkId, const QVariant& parameterId, int index);
    // Linked instances share one definition. Duplicate creates another linked
    // occurrence; Make Independent detaches only the selected occurrence.
    Q_INVOKABLE QString duplicateLinkedInstance(const QString& networkId, const QVariant& nodeId, double x, double y);
    Q_INVOKABLE bool makeIndependent(const QString& instanceId);
    // Graphical copy/paste uses the shared selection-copy command. The
    // clipboard is presentation state: it survives panel changes but is never
    // part of the document or its history.
    Q_INVOKABLE bool copyGraphSelection(const QString& networkId, const QVariantList& nodeIds);
    Q_INVOKABLE QString pasteGraphSelection(const QString& networkId, double x, double y);
    // Parameter entry points remain command-backed and are used by the inspector.
    Q_INVOKABLE void setNodeParameter(const QVariant& nodeId, const QString& key, const QVariant& value);
    // Explicit text-entry adapter; parsing remains catalog-owned and avoids
    // converting signed 64-bit values through JavaScript Number.
    Q_INVOKABLE void setNodeParameterText(const QVariant& nodeId, const QString& key, const QString& text);
    Q_INVOKABLE void setNodeParameters(const QVariantList& edits);
    // Identity-scoped reset to the schema default. It resolves the same
    // occurrence/exposed target as the inspector gestures, so a subnet
    // occurrence or exposed control resets its own scope. A static parameter
    // resets its authored value; an animated parameter authors the default at
    // the current frame and never removes the channel or its other keys. One
    // history entry; a rejected or stale reset changes nothing.
    Q_INVOKABLE bool resetNodeParameterEdit(const QString& networkId, const QVariant& nodeId, const QString& key);
    // Schema-driven parameter inspector: one map with the node identity, its
    // descriptor metadata, and sectioned parameter rows carrying the value
    // evaluated at the current frame plus animation/key state.
    Q_INVOKABLE QVariantMap parameterInspector(const QString& networkId, const QVariant& nodeId) const;
    // Keying at the current frame. Single validated commands through the
    // session; authored values are never mutated directly.
    Q_INVOKABLE QString nodeParameterKeyStatus(const QString& networkId, const QVariant& nodeId,
                                               const QString& key) const;
    Q_INVOKABLE bool keyNodeParameter(const QString& networkId, const QVariant& nodeId, const QString& key);
    Q_INVOKABLE bool removeNodeParameterKey(const QString& networkId, const QVariant& nodeId, const QString& key);
    // Retire unfinished parameter input before another control starts editing.
    // The same injected owner serves every controller/window of this project.
    Q_INVOKABLE void prepareParameterInteraction();
    [[nodiscard]] ParameterInteraction& parameterInteraction() { return interaction_; }
    // Continuous edit gesture: begin returns a decimal token ("" on failure),
    // update previews, commit publishes one history entry, cancel discards.
    // Begin supersedes the previous presentation interaction, not core safety.
    Q_INVOKABLE QString beginNodeParameterEdit(const QString& networkId, const QVariant& nodeId, const QString& key);
    Q_INVOKABLE bool updateNodeParameterEdit(const QString& token, const QVariant& value);
    // Batch shape of the same gesture. Each address follows the core owner's
    // per-address rule (an existing channel authors the current-frame key, an
    // unanimated parameter takes the static value), so one atomic edit can
    // change an animated choice and its static companion together: one preview,
    // one history entry, one semantic validation of the whole resolved set.
    Q_INVOKABLE QString beginNodeParameterEdits(const QString& networkId, const QVariant& nodeId,
                                                const QStringList& keys);
    Q_INVOKABLE bool updateNodeParameterEdits(const QString& token, const QVariantMap& values);
    Q_INVOKABLE bool commitNodeParameterEdit(const QString& token);
    Q_INVOKABLE bool cancelNodeParameterEdit(const QString& token);
    // The current persistent model exposes source timing, not timeline clip
    // occurrences. These edit SourceReference through the command API.
    Q_INVOKABLE void slipTimelineClip(const QString& source, int delta);
    Q_INVOKABLE void retimeTimelineClip(const QString& source, int step);
    // Viewer attachment is command-owned like every other graph edit. The
    // shared controller renders one viewer at a time: the active viewer's
    // attached upstream node is the evaluation target.
    Q_INVOKABLE void setActiveViewer(const QString& networkId, int viewerIndex);
    Q_INVOKABLE int viewerCount(const QString& networkId) const;
    Q_INVOKABLE QVariantMap viewerAttachment(const QString& networkId, int viewerIndex) const;
    Q_INVOKABLE bool assignViewer(const QString& networkId, int viewerIndex, const QVariant& nodeId);
    Q_INVOKABLE void cancelRender();
    Q_INVOKABLE void requestRange(int first, int last);
    // Panel transport. Playback presents frames from the ordered playback window
    // (issue #106): the transport clock consumes READY frames while bounded
    // replay preparation and read-ahead overlap presentation, instead of waiting
    // for one outstanding live render per frame. A frame with no retained cache
    // representation is the one frame that renders live, exactly as the serial
    // path did, and a prefetched neighbour never renders. A single-shot pacing
    // timer decides WHEN the next frame is due, so playback holds the
    // composition rate while frames arrive early and falls behind honestly when
    // they do not; frames are always produced in order and none are skipped.
    // Loops inclusively within [inFrame_, outFrame_] in the stated direction.
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    // Pauses and seeks to inFrame_.
    Q_INVOKABLE void stop();
    // Ordered playback direction (issue #106). The packaged transport exposes
    // forward playback only — the prototype's in/start/previous/play/stop/next/
    // end/out bar with Space/Left/Right keys, which the production panel keeps —
    // so reverse is stated through this plain public seam rather than an
    // invented UI control, and the ordered playback window is exercised and
    // driven through it by the native diagnostic harness. Changing direction
    // retires the window, because its order changed.
    enum class PlaybackDirection : std::uint8_t { Forward, Reverse };
    void setPlaybackDirection(PlaybackDirection direction);
    [[nodiscard]] PlaybackDirection playbackDirection() const { return playbackDirection_; }
    // Playback rate in frames per second, pacing the transport. This is the
    // COMPOSITION rate and is deliberately never taken from the probed media: a
    // 25 fps clip inside a 24 fps composition plays at the composition rate, and
    // retargeting a Read cannot change playback cadence. The rate is not yet a
    // persistent per-project timebase in the Document model (#83), so it is a
    // session property with the 24 fps default until that lands.
    Q_INVOKABLE void setFrameRate(double rate);
    Q_INVOKABLE void stepBy(int delta);
    Q_INVOKABLE void seekToIn();
    Q_INVOKABLE void seekToOut();
    // Mark commands clamp into the current frame range and never invert.
    Q_INVOKABLE void setMarkIn();
    Q_INVOKABLE void setMarkOut();
    Q_INVOKABLE void setMarkInFrame(int frame);
    Q_INVOKABLE void setMarkOutFrame(int frame);
    // Named display selection (issue #90). The layer picks which named channel
    // layer this panel evaluates and the channel picks what the presentation
    // isolates inside it. Both are validated against the target's ACTUAL
    // described channels; an unavailable name is reported, never substituted.
    Q_INVOKABLE void setChannel(const QString& channel);
    Q_INVOKABLE void setLayer(const QString& layer);
    // Asynchronous channel availability of one node's image inputs (issue #90).
    // The answer comes from the worker-side description planner through the
    // runtime's bounded admission, so a mapping editor never probes media or
    // evaluates on the GUI thread, and never invents a channel it cannot
    // observe. Repeated calls for one identity reuse the retained answer; a
    // superseded identity replaces the outstanding query instead of queueing
    // beside it. `nodeChannelsChanged` reports the answer.
    Q_INVOKABLE QVariantMap nodeInputChannels(const QString& networkValue, const QVariant& nodeValue);
    // --- viewport sampling (issue #102) ------------------------------------
    // The identity of the frame this panel currently DISPLAYS: the submission
    // that produced it and the authored revision it was rendered from. A
    // viewport pick is stated against this identity and re-checks it before it
    // authors anything, so a retargeted, reframed or re-rendered panel can
    // never commit a sample taken from another image.
    struct DisplayedFrameIdentity {
        std::uint64_t request{};
        std::uint64_t revision{};
        friend bool operator==(const DisplayedFrameIdentity&, const DisplayedFrameIdentity&) = default;
    };
    // One on-demand working-space sample demand for `imageX`/`imageY`, in the
    // displayed frame's own full-resolution image coordinates. `document` is the
    // snapshot the demand addresses — for the media role it is the same
    // request-owned composition the render used, so the sample and the frame on
    // screen describe one image — and `request` is exactly one full-resolution
    // pixel of the displayed target in the target's OWN channels. `displayed`
    // is the identity the demand was stated against.
    struct WorkingSampleRequest {
        Document document;
        EvaluationRequest request;
        DisplayedFrameIdentity displayed;
    };
    [[nodiscard]] std::optional<DisplayedFrameIdentity> displayedFrameIdentity() const;
    // nullopt when this panel displays no frame, or when the coordinate names no
    // pixel of the displayed image (a point in the surround is never clamped to
    // an edge pixel).
    [[nodiscard]] std::optional<WorkingSampleRequest> workingSampleRequest(double imageX, double imageY) const;
    // The document's authored canvas presets (#96), ascending by name, each
    // {name, width, height, pixelAspect}. This is a read of the session's own
    // stored state: presentation owns no second format registry, and a preset
    // is only ever applied by copying its value.
    Q_INVOKABLE QVariantList namedFormats() const;
    // The saved canvas of ONE network ({available, width, height, pixelAspect}),
    // which a format-source control states for its composition choice. A
    // network with no format reports `available` false rather than a
    // substituted canvas.
    Q_INVOKABLE QVariantMap networkFormat(const QString& networkValue) const;
    // Create/replace and delete an authored canvas preset through the shared
    // document commands: each is one history entry, and the presentation layer
    // never mutates stored formats itself.
    Q_INVOKABLE bool setNamedFormat(const QString& name, int width, int height, double pixelAspect);
    Q_INVOKABLE bool removeNamedFormat(const QString& name);
    Q_INVOKABLE QString timecodeForFrame(int frame) const;
    Q_INVOKABLE int frameForTimecode(const QString& text) const;
    // Forwards the resolved panel context (PanelContextRouter) into the render
    // target: role is "graph", "timeline", or "media".
    Q_INVOKABLE void setViewerContext(const QString& role, const QString& target, int clock);
    void attachWindow(QQuickWindow* window);
    void attachViewerItem(ViewerItem* item);
    void detachViewerItem(ViewerItem* item);
    void setPrimaryViewerItem(ViewerItem* item);
    void viewportChanged(QSizeF physicalPixels);
    [[nodiscard]] bool hasSource() const { return !sourceSize_.isEmpty(); }
    [[nodiscard]] int dragDistance() const;
    [[nodiscard]] QSizeF sourceSize() const { return sourceSize_; }
    [[nodiscard]] double pixelAspect() const { return pixelAspect_; }
    [[nodiscard]] int frameCount() const { return frameCount_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString renderState() const;
    [[nodiscard]] bool pending() const { return pending_; }
    [[nodiscard]] bool outdated() const { return outdated_; }
    [[nodiscard]] bool hasPresentation() const { return static_cast<bool>(presentation_); }
    [[nodiscard]] QString resolutionMode() const { return mode_; }
    [[nodiscard]] bool forceFullFrame() const { return forceFullFrame_; }
    [[nodiscard]] double zoom() const { return zoom_; }
    [[nodiscard]] QPointF pan() const { return pan_; }
    [[nodiscard]] int frame() const { return frame_; }
    [[nodiscard]] int effectiveScale() const { return effectiveScale_; }
    [[nodiscard]] QString viewerTargetName() const { return viewerTargetName_; }
    [[nodiscard]] QString viewerTargetId() const;
    [[nodiscard]] QString rootNetworkId() const;
    Q_INVOKABLE QVariantMap graphSnapshot(const QString& networkId) const;
    // The document revision a graph snapshot is projected from. The interaction
    // layer stamps its scene with it and aborts a live gesture when it changes,
    // which is why it is not part of the snapshot map: the projection's content
    // stays exactly what it always was.
    Q_INVOKABLE qulonglong graphRevision() const;
    Q_INVOKABLE QVariantMap graphScope(const QString& rootNetworkId, const QVariantList& instancePath) const;
    [[nodiscard]] QVariantList graphNodes() const;
    [[nodiscard]] QVariantList graphEdges() const;
    [[nodiscard]] QVariantList nodeCatalog() const;
    [[nodiscard]] QVariantList timelineClips() const;
    [[nodiscard]] qulonglong queued() const;
    [[nodiscard]] qulonglong dropped() const;
    [[nodiscard]] qulonglong staleRejected() const;
    [[nodiscard]] qulonglong completed() const;
    [[nodiscard]] qulonglong cacheQueued() const { return schedulerCounts_.cacheQueued; }
    [[nodiscard]] qulonglong cacheDropped() const { return schedulerCounts_.cacheDropped; }
    [[nodiscard]] qulonglong cacheErrors() const { return schedulerCounts_.cacheErrors; }
    [[nodiscard]] qulonglong cachePublished() const { return schedulerCounts_.cachePublished; }
    [[nodiscard]] QString cacheError() const {
        return rangeError_.isEmpty() ? QString::fromStdString(schedulerCounts_.cacheError) : rangeError_;
    }
    [[nodiscard]] QRectF presentedRegion() const;
    [[nodiscard]] QSizeF compositionSize() const;
    [[nodiscard]] bool hasDestination() const { return destination_.has_value(); }
    [[nodiscard]] qulonglong destinationId() const;
    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] int inFrame() const { return inFrame_; }
    [[nodiscard]] int outFrame() const { return outFrame_; }
    [[nodiscard]] double frameRate() const { return frameRate_; }
    [[nodiscard]] QString channel() const { return channel_; }
    [[nodiscard]] QString layer() const { return layer_; }
    [[nodiscard]] QStringList availableLayers() const;
    [[nodiscard]] QStringList availableChannels() const;
    [[nodiscard]] QString layerReason() const;
    [[nodiscard]] QString timecode() const;
    [[nodiscard]] std::shared_ptr<const ViewerResult> presentation() const { return presentation_; }
    // The one retained presentation host shared by every panel; the runtime
    // owns its lifetime across panels and Qt teardown.
    [[nodiscard]] WindowPresentationState& presentationState() const { return runtime_->windowPresentation(); }
    [[nodiscard]] bool filterLinear() const { return runtime_->presentationFilterLinear(); }

signals:
    void parameterEditEnded(const QString& token);
    void sourceChanged();
    void statusChanged();
    void resolutionChanged();
    void forceFullFrameChanged();
    void zoomChanged();
    void panChanged();
    void frameChanged();
    void effectiveScaleChanged();
    void viewerTargetChanged();
    void graphChanged();
    void timelineChanged();
    void catalogChanged();
    void schedulerChanged();
    void frameArrived();
    void destinationChanged();
    void playbackChanged();
    void marksChanged();
    void frameRateChanged();
    void displayChanged();
    // A node's input channel answer advanced (issue #90). Presenters re-query
    // `nodeInputChannels` instead of caching a list of their own.
    void nodeChannelsChanged();
    // Qt handed the rendered frame to the window system. This is not a
    // physical scanout timestamp; benchmark reports name that boundary.
    void framePresented(int frame, int width, int height, bool cacheHit, double requestToSwapMs);

private:
    // Which routed panel context this controller renders. The graph role
    // renders this controller's own Viewer attachment; the media role renders
    // the root-network source node addressing the routed catalog target; the
    // timeline role is deferred to issue #54 and renders nothing.
    enum class ContextRole { Graph, Media, Timeline };
    // Frame domain for a document with no probed media length. The viewer
    // panel's ruler uses the same 240-frame (0..239) fallback, so marks and
    // playback cover exactly the domain the ruler paints.
    static constexpr int kDefaultFrameCount = 240;
    static void sessionDocumentChanged(void* context) noexcept;
    void documentChanged();
    void refreshViewerTarget();
    // Re-resolves contextRole_/contextTarget_ against the live document and
    // records the reason a routed target has no render target.
    void refreshContextTarget();
    // The routed target resolution itself: the described-channel memo is
    // invalidated by refreshContextTarget only when the resolved target moved.
    void resolveContextTarget();
    [[nodiscard]] NetworkId renderTargetNetwork(const Document& document) const;
    [[nodiscard]] NodeId renderTargetNode() const;
    [[nodiscard]] QString unavailableStatus() const;
    // Inspector view of a subnet occurrence: its definition's exposed controls,
    // addressed by exposed identity and carrying instance-local values.
    [[nodiscard]] QVariantMap subnetInspector(nemo::NetworkId network, nemo::NodeId node) const;
    // Clamps into the same frame domain as setFrame: [0, frameCount - 1].
    [[nodiscard]] int clampFrame(int frame) const;
    // Inclusive last frame of the current domain. Unknown media length has no
    // authored upper bound.
    [[nodiscard]] int frameDomainEnd() const;
    void buildGraph(const SourceReference& reference);
    void refreshRequest();
    // The immutable view intent this panel states for `frame` (issues #98/#106):
    // the target, the panel's current resolution/coverage/zoom/pan/viewport,
    // layer and channel. Shared by the latest-wins render and the ordered
    // playback window, so a replayed frame states exactly the view a live render
    // would and the worker can serve its retained record without re-resolving
    // it. `privateMediaSource` normalizes the request-owned media node out of
    // the identity; the caller re-states the real target before submitting.
    [[nodiscard]] eval::ViewIntent viewIntentFor(NetworkId network, NodeId target, int frame,
                                                 bool privateMediaSource) const;
    // The described output of the frame this panel last presented (issue #98):
    // its actual format, pixel aspect and channels. A view is resolved on the
    // worker against the CURRENT frame, so this memo is presentation state —
    // what the layer and channel selectors address, and the display fallback
    // before the first frame arrives — never an input to a request.
    //
    // A panel with no measured viewport has no view to resolve, so it is a
    // metadata-only consumer: the same memo is filled by the worker's
    // description answer (`answered` false while that answer is in flight), and
    // nothing is rendered for a view that does not exist.
    struct TargetDescription {
        NodeId target{kInvalidNode};
        std::int64_t localTime{};
        std::uint64_t revision{};
        bool answered{false};
        ImageDescription description;
    };
    struct Framing {
        int width{};
        int height{};
        double pixelAspect{1.0};
    };
    // Framing comes only from the frame's resolved image description. Unknown
    // metadata stays unavailable; a known-empty image retains its format.
    [[nodiscard]] std::optional<Framing> targetFraming() const;
    // True when the memo ANSWERS for exactly this target, local time and
    // revision: a delivered frame, or the worker's metadata-only description
    // for a panel that has no viewport to resolve.
    [[nodiscard]] bool targetDescriptionAnswers(NodeId target, std::uint64_t revision, std::int64_t localTime) const;
    // The metadata-only query for one target at the current frame: the smallest
    // valid domain that identifies it, used when there is no view to resolve.
    [[nodiscard]] EvaluationRequest descriptionRequest(NetworkId network, NodeId target) const;
    // Real channels of the selected layer, in described order. Empty until a
    // frame has stated its description: absence is never filled in with a
    // guessed channel set.
    [[nodiscard]] QStringList layerChannels() const;
    // One input port of the queried node. `upstream` is kInvalidNode for a
    // disconnected port, which is a real state (`A` is optional), not an error.
    struct ChannelPort {
        QString name;
        QString kind;
        bool image{false};
        bool optional{false};
        NodeId upstream{kInvalidNode};
        bool answered{false};
        ImageDescription description;
        QString failure;
    };
    struct ChannelQuery {
        NetworkId network{kInvalidNetwork};
        NodeId node{kInvalidNode};
        std::uint64_t revision{};
        std::int64_t localTime{};
        std::vector<ChannelPort> ports;
        // Describes are submitted one port at a time: the runtime keeps one
        // publication slot per destination, so an outstanding answer is never
        // overwritten by its own sibling.
        std::size_t nextPort{};
        std::size_t pendingPort{std::numeric_limits<std::size_t>::max()};
        std::uint64_t outstanding{};
        bool complete{false};
    };
    // The destination channel availability queries are published to, allocated
    // on first use and independent of this panel's render destination so a
    // description never supersedes a render or another card's answer.
    [[nodiscard]] std::optional<eval::ViewerDestination> channelQueryDestination();
    void submitNextChannelQuery();
    void applyChannelDescription(std::uint64_t requestId, const ImageDescription& description);
    void applyChannelFailure(std::uint64_t requestId, const QString& message);
    [[nodiscard]] QVariantMap channelQueryAnswer() const;
    // Forgets the probed media when the request no longer addresses it: a
    // cleared Read, a replaced target, or a target that names no reference.
    void forgetProbedMedia();
    void invalidateRequest();
    void pollScheduler();
    void receive();
    void fail(QString message);
    bool applyEdit(const nemo::EditResult& result);
    void clearError();
    [[nodiscard]] nemo::EditOptions editOptions() const;
    // Applies a probed frame count to the frame domain, the authored marks, and
    // the current frame.
    void applyFrameCount(int frameCount);
    [[nodiscard]] int playbackInterval() const;
    // The single place that decides what the transport does next: present the
    // frame the clock is due for if it is ready, otherwise keep the ordered
    // playback window filled and look again shortly. A frame with no retained
    // representation renders live through the latest-wins path, one outstanding.
    void pumpPlayback();
    // The frame the transport walks to next in the stated direction, wrapping
    // inclusively inside [inFrame_, outFrame_].
    [[nodiscard]] int nextPlaybackFrame() const;
    [[nodiscard]] int playbackFrameAfter(int frame) const;
    // One frame of the ordered playback window (issue #106): the frame, the
    // preparation admitted for it, and the frame itself once it is ready. The
    // window is a contiguous run of frames starting at the frame the clock is
    // due to show next, so presenting its head is always the transport's next
    // step and a successor can never be shown out of order.
    struct PlaybackSlot {
        int frame{};
        std::uint64_t request{};
        std::uint64_t revision{};
        std::optional<eval::ViewIntent> intent;
        bool missing{};
        std::shared_ptr<const ViewerResult> ready;
    };
    // What the ordered window can do for the frame the clock is due to show.
    enum class PlaybackWindow { Prepared, LiveFrame, Unavailable };
    // Slides the window onto the current head, drops a gap's successors, and
    // admits the bounded read-ahead the transport still needs. Read-ahead is
    // replay-only: a frame the retained index does not know is never rendered
    // here, and filling stops at the first known gap.
    [[nodiscard]] PlaybackWindow ensurePlaybackWindow();
    // States one replay preparation for `slot` through the runtime's ordered
    // playback admission. False when the panel has no renderable view or the
    // bounded window refused it.
    bool preparePlaybackFrame(PlaybackSlot& slot);
    // Commits the head frame when the clock is due for it and it is ready.
    bool presentReadyPlaybackFrame();
    // Retires the whole ordered window: a latest-wins submission, a seek, an
    // edit, a range, a destination change or a failure supersedes it.
    void clearPlaybackWindow();
    [[nodiscard]] PlaybackSlot* playbackSlot(std::uint64_t request);
    // Publishes one completed frame as the panel's current presentation,
    // advancing the transport to the frame it carries. Shared by the live and
    // replay paths so both present identically.
    void publishFrame(std::shared_ptr<const ViewerResult> frame);
    ViewerRuntime* runtime_;
    nemo::ProjectSession& session_;
    ParameterInteraction& interaction_;
    // The scheduler destination this panel owns. Unset, the controller is a
    // pure command/metadata facade.
    std::optional<eval::ViewerDestination> destination_;
    // Routed panel context (PanelContextRouter): which target family this panel
    // displays and the catalog target it addresses.
    ContextRole contextRole_{ContextRole::Graph};
    QString contextTarget_;
    // Document source key the routed media target resolves to. A decimal
    // target is a catalog entry id and resolves through its persistent source
    // key; a non-decimal target is the key itself. Empty when the target names
    // no decodable Document source.
    std::string contextSourceKey_;
    NodeId contextTargetNode_{kInvalidNode};
    QString contextUnavailable_;
    // Source key of the probe this panel submitted. `generation_` already
    // rejects superseded probes; the key is carried so the accepted result
    // publishes exactly the reference that was probed.
    std::string probeSourceKey_{"src"};
    // Description of the frame in effect (issue #98): the actual authored
    // format the last presented frame was produced from. The worker resolves
    // views against the current frame, so this memo serves the selectors and
    // the display fallback only.
    std::optional<TargetDescription> targetDescription_;
    // Revision of the exact snapshot handed to the last submission. A result
    // carries that snapshot's revision, which is the authored revision except
    // when the media role added its request-owned node.
    std::uint64_t submittedRevision_{};
    SourceReference probedSource_;
    QSizeF sourceSize_;
    QSizeF viewport_;
    double pixelAspect_{1.0};
    int frameCount_{-1};
    int frame_{};
    int effectiveScale_{1};
    double zoom_{1.0};
    QPointF pan_;
    // The active viewer: its attached upstream node is the evaluation target.
    // Defaults to root network viewer 0 until a panel activates another.
    NetworkId activeViewerNetwork_{kInvalidNetwork};
    int activeViewerIndex_{0};
    NodeId viewerTargetNode_{kInvalidNode};
    QString viewerTargetName_;
    QString mode_{QStringLiteral("auto")};
    // Whole-domain coverage switch (see the property). Retained by the panel's
    // state record; never document state.
    bool forceFullFrame_{false};
    QString status_;
    QString error_;
    QString sourceDescription_;
    bool pending_{false};
    // Request this panel has submitted and not yet consumed (0 = nothing
    // outstanding). This is the LATEST-WINS slot only — a seek, edit or view
    // change. Ordered playback preparations travel through playbackWindow_
    // instead, so they never make each other stale.
    std::uint64_t outstandingRequest_{0};
    // Ordered playback window (issue #106): the bounded run of frames the
    // transport is walking to, each either prepared, ready or known to have no
    // retained representation. Read-ahead is bounded by this constant, which
    // stays inside the scheduler's own per-destination window.
    static constexpr std::size_t kPlaybackWindowFrames = 4;
    std::deque<PlaybackSlot> playbackWindow_;
    std::optional<eval::ViewerPlaybackContext> playbackContext_;
    NodeId playbackTarget_{kInvalidNode};
    // True while the ONE frame with no retained representation is rendering
    // live: the transport waits for it exactly as the serial path did, so a
    // missing frame costs one live render and never a burst of speculative ones.
    bool playbackLive_{false};
    // The same missed head deadline may be observed by several worker/poll
    // callbacks; report it once until a frame publishes or the window retires.
    bool playbackUnderrunReported_{false};
    PlaybackDirection playbackDirection_{PlaybackDirection::Forward};
    // Monotonic deadline for the next frame. Epoch means the first publication
    // establishes the phase; ordered publications advance it one interval,
    // while an explicit seek/edit publication rebases it to that new frame.
    std::chrono::steady_clock::time_point playbackDue_{};
    bool outdated_{false};
    std::uint64_t generation_{};
    std::uint64_t nextRequestId_{};
    std::uint64_t rangeGeneration_{};
    QString rangeError_;
    ViewerRuntimeCounts schedulerCounts_;
    // The authored revision of the last submitted view intent, and the intent
    // itself as submitted — with a request-owned media target normalized to
    // "none", because that node is allocated fresh per submission while the
    // routed source key and the view are what the panel asked for. Together
    // they are the "this exact view is already submitted or displayed" test
    // that keeps a steady frame at one worker request.
    std::uint64_t lastRevision_{};
    std::optional<eval::ViewIntent> lastIntent_;
    std::shared_ptr<const ViewerResult> presentation_;
    // True when the attached target is a media source node that names no
    // reference: an explicit empty viewer rather than a stale frame or the
    // default composition canvas.
    bool targetEmpty_{};
    // Memoized target media resolution, keyed by document revision, request
    // network and render target. The graph role's dependency walk then runs at
    // most once per revision instead of once per zoom/pan/frame request.
    std::uint64_t targetMediaRevision_{};
    NetworkId targetMediaNetwork_{kInvalidNetwork};
    NodeId targetMediaTarget_{kInvalidNode};
    std::string targetMediaKey_;
    bool targetMediaUnbound_{};
    // Panel-local selection. The layer and channel are resolved against the
    // described image on the worker (issue #98), which also derives the
    // presentation-only isolation, so a changing image can never retain
    // obsolete color/scalar interpretation here.
    QString channel_{QStringLiteral("RGBA")};
    QString layer_{QStringLiteral("rgba")};
    // Channel availability query (issue #90). `channelQuery_` is the memoized
    // answer for one node/revision/frame; the destination is allocated on first
    // use so it is never another panel's and never a render's.
    std::optional<ChannelQuery> channelQuery_;
    std::optional<eval::ViewerDestination> channelQueryDestination_;
    std::uint64_t nextChannelRequestId_{0};
    double frameRate_{24.0};
    bool playing_{false};
    bool marksAuthored_{false};
    int inFrame_{0};
    int outFrame_{kDefaultFrameCount - 1};
    QList<QPointer<ViewerItem>> items_;
    QPointer<ViewerItem> primary_;
    QTimer schedulerPoll_;
    QTimer playback_;
    nemo::ProjectSession::Subscription sessionSubscription_;
    // One continuous parameter gesture at a time, owner-thread-only. The
    // token is the session's; this facade only remembers its scope.
    // Last observed ProjectSession project generation. A change means the
    // published project was reopened or replaced, which is the explicit
    // config-freshness boundary the runtime refreshes on.
    std::uint64_t observedProjectGeneration_{0};
    // One internal owner for both shapes: the convenience single-key calls are
    // the one-element case of these.
    [[nodiscard]] QString beginParameterGestureFor(const QString& networkValue, const QVariant& nodeValue,
                                                   const QStringList& keys);
    bool updateParameterGestureValues(const QString& tokenValue, const QVariantMap& values);
    void finishParameterGesture();
    // The media role's displayed target is the routed CATALOG reference, which
    // is not a persisted node: one request-owned Read addressing it is inserted
    // into the request's own snapshot, so the viewport sampler and the render
    // demand describe the same image while no node, used-media mark or history
    // entry is ever persisted for a catalog open.
    [[nodiscard]] NodeId adoptMediaSourceNode(Document& document, const std::string& sourceKey) const;

    std::optional<nemo::ParameterAddress> parameterGestureAddress_;
    nemo::ParameterGestureToken parameterGestureToken_{0};
    // Set when an update preview is inadmissible: the live session gesture is
    // kept so the caller's token discipline still works, but it can never be
    // committed. A later admissible update clears it.
    bool parameterGestureInvalid_{false};
    // Presentation-only clipboard: the source network and node identities of
    // the last graphical copy. Never part of the document or its history.
    std::optional<nemo::NetworkId> clipboardNetwork_;
    std::vector<nemo::NodeId> clipboardNodes_;
    // QObject-owned adapters; authored state remains in ProjectSession.
    std::map<std::tuple<nemo::NetworkId, nemo::NodeId, QString>, RotoController*> rotoControllers_;
};
}  // namespace nemo::ui
