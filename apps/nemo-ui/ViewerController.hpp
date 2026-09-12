#pragma once

#include "ViewerRuntime.hpp"
#include "nemo/core/evaluation/ViewerResolution.hpp"
#include "nemo/core/nodes/NodeCatalog.hpp"
#include "nemo/core/session/ProjectSession.hpp"
#include <QPointer>
#include <QRectF>
#include <QSizeF>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace nemo::ui {
class ViewerItem;
class WindowPresentationState;
// GUI-thread presentation state. The explicitly composed ProjectSession owns
// the live Document/history; this facade only submits commands and reads it.
// Scenegraph reads happen only during updatePaintNode, while Qt blocks the GUI
// thread. The worker sees immutable Document copies.
class ViewerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasSource READ hasSource NOTIFY sourceChanged)
    Q_PROPERTY(QSizeF sourceSize READ sourceSize NOTIFY sourceChanged)
    Q_PROPERTY(double pixelAspect READ pixelAspect NOTIFY sourceChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY sourceChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)
    Q_PROPERTY(QString renderState READ renderState NOTIFY statusChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY statusChanged)
    Q_PROPERTY(bool outdated READ outdated NOTIFY statusChanged)
    Q_PROPERTY(QString resolutionMode READ resolutionMode WRITE setResolutionMode NOTIFY resolutionChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(QPointF pan READ pan WRITE setPan NOTIFY panChanged)
    Q_PROPERTY(int frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(int effectiveScale READ effectiveScale NOTIFY effectiveScaleChanged)
    Q_PROPERTY(QRectF presentedRegion READ presentedRegion NOTIFY frameArrived)
    // Full-resolution image domain the current presentation was evaluated
    // against: the probed media size, or the default composition canvas when
    // no media source is loaded. Display math reads this rather than the
    // media-only source size so a media-free graph still shows its result.
    Q_PROPERTY(QSizeF compositionSize READ compositionSize NOTIFY frameArrived)
    Q_PROPERTY(QString viewerTargetName READ viewerTargetName NOTIFY viewerTargetChanged)
    Q_PROPERTY(QString viewerTargetId READ viewerTargetId NOTIFY viewerTargetChanged)
    Q_PROPERTY(QString rootNetworkId READ rootNetworkId NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphNodes READ graphNodes NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphEdges READ graphEdges NOTIFY graphChanged)
    Q_PROPERTY(QVariantList nodeCatalog READ nodeCatalog NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList timelineClips READ timelineClips NOTIFY timelineChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(qulonglong queued READ queued NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong dropped READ dropped NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong staleRejected READ staleRejected NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong completed READ completed NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cacheQueued READ cacheQueued NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cacheDropped READ cacheDropped NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cacheErrors READ cacheErrors NOTIFY schedulerChanged)
    Q_PROPERTY(qulonglong cachePublished READ cachePublished NOTIFY schedulerChanged)
    Q_PROPERTY(QString cacheError READ cacheError NOTIFY schedulerChanged)
public:
    explicit ViewerController(ViewerRuntime* runtime, nemo::ProjectSession& session);
    ~ViewerController() override;
    Q_INVOKABLE void openSource(const QString& path);
    Q_INVOKABLE void setResolutionMode(const QString& mode);
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
    Q_INVOKABLE bool disconnectGraphEdge(const QString& networkId, const QVariant& edgeId);
    Q_INVOKABLE bool commitGraphMove(const QString& networkId, const QVariantList& positions);
    Q_INVOKABLE bool commitGraphRoute(const QString& networkId, const QVariant& edgeId, const QVariantList& points);
    Q_INVOKABLE bool insertGraphRoutePoint(const QString& networkId, const QVariant& edgeId, int index, double x,
                                           double y);
    Q_INVOKABLE bool moveGraphRoutePoint(const QString& networkId, const QVariant& edgeId, int index, double x,
                                         double y);
    Q_INVOKABLE bool removeGraphRoutePoint(const QString& networkId, const QVariant& edgeId, int index);
    // Parameter entry points remain command-backed and are used by the inspector.
    Q_INVOKABLE void setNodeParameter(const QVariant& nodeId, const QString& key, const QVariant& value);
    // Explicit text-entry adapter; parsing remains catalog-owned and avoids
    // converting signed 64-bit values through JavaScript Number.
    Q_INVOKABLE void setNodeParameterText(const QVariant& nodeId, const QString& key, const QString& text);
    Q_INVOKABLE void resetNodeParameter(const QVariant& nodeId, const QString& key);
    Q_INVOKABLE void setNodeParameters(const QVariantList& edits);
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
    // Continuous edit gesture: begin returns a decimal token ("" on failure),
    // update previews, commit publishes one history entry, cancel discards.
    Q_INVOKABLE QString beginNodeParameterEdit(const QString& networkId, const QVariant& nodeId, const QString& key);
    Q_INVOKABLE bool updateNodeParameterEdit(const QString& token, const QVariant& value);
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
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE void cancelRender();
    Q_INVOKABLE void requestRange(int first, int last);
    void attachWindow(QQuickWindow* window);
    void attachViewerItem(ViewerItem* item);
    void detachViewerItem(ViewerItem* item);
    void setPrimaryViewerItem(ViewerItem* item);
    void viewportChanged(QSizeF physicalPixels);
    [[nodiscard]] bool hasSource() const { return !sourceSize_.isEmpty(); }
    [[nodiscard]] QSizeF sourceSize() const { return sourceSize_; }
    [[nodiscard]] double pixelAspect() const { return pixelAspect_; }
    [[nodiscard]] int frameCount() const { return frameCount_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString renderState() const;
    [[nodiscard]] bool pending() const { return pending_; }
    [[nodiscard]] bool outdated() const { return outdated_; }
    [[nodiscard]] QString resolutionMode() const { return mode_; }
    [[nodiscard]] double zoom() const { return zoom_; }
    [[nodiscard]] QPointF pan() const { return pan_; }
    [[nodiscard]] int frame() const { return frame_; }
    [[nodiscard]] int effectiveScale() const { return effectiveScale_; }
    [[nodiscard]] QString viewerTargetName() const { return viewerTargetName_; }
    [[nodiscard]] QString viewerTargetId() const;
    [[nodiscard]] QString rootNetworkId() const;
    Q_INVOKABLE QVariantMap graphSnapshot(const QString& networkId) const;
    [[nodiscard]] QVariantList graphNodes() const;
    [[nodiscard]] QVariantList graphEdges() const;
    [[nodiscard]] QVariantList nodeCatalog() const;
    [[nodiscard]] QVariantList timelineClips() const;
    [[nodiscard]] bool canUndo() const { return session_.canUndo(); }
    [[nodiscard]] bool canRedo() const { return session_.canRedo(); }
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
    [[nodiscard]] std::shared_ptr<const ViewerResult> presentation() const { return presentation_; }
    [[nodiscard]] WindowPresentationState& presentationState() const { return *presentationState_; }
    [[nodiscard]] bool filterLinear() const { return runtime_->presentationFilterLinear(); }

signals:
    void sourceChanged();
    void statusChanged();
    void resolutionChanged();
    void zoomChanged();
    void panChanged();
    void frameChanged();
    void effectiveScaleChanged();
    void viewerTargetChanged();
    void graphChanged();
    void timelineChanged();
    void catalogChanged();
    void historyChanged();
    void schedulerChanged();
    void frameArrived();
    // Qt handed the rendered frame to the window system. This is not a
    // physical scanout timestamp; benchmark reports name that boundary.
    void framePresented(int frame, int width, int height, bool cacheHit, double requestToSwapMs);

private:
    static void sessionDocumentChanged(void* context) noexcept;
    void documentChanged();
    void refreshViewerTarget();
    void buildGraph(const SourceReference& reference);
    void refreshRequest();
    void invalidateRequest();
    void pollScheduler();
    void receive();
    void fail(QString message);
    bool applyEdit(const nemo::EditResult& result);
    void clearError();
    [[nodiscard]] nemo::EditOptions editOptions() const;
    ViewerRuntime* runtime_;
    nemo::ProjectSession& session_;
    SourceReference probedSource_;
    ViewerResolutionPolicy policy_;
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
    QString status_;
    QString error_;
    QString sourceDescription_;
    bool pending_{false};
    bool outdated_{false};
    std::uint64_t generation_{};
    std::uint64_t nextRequestId_{};
    std::uint64_t rangeGeneration_{};
    QString rangeError_;
    ViewerRuntimeCounts schedulerCounts_;
    std::uint64_t lastRevision_{};
    std::optional<EvaluationRequest> lastRequest_;
    std::shared_ptr<const ViewerResult> presentation_;
    QList<QPointer<ViewerItem>> items_;
    QPointer<ViewerItem> primary_;
    QTimer schedulerPoll_;
    // Outlives all QML nodes and retains their images through Qt frame-slot
    // completion, including when a viewer panel is closed during a frame.
    std::unique_ptr<WindowPresentationState> presentationState_;
    nemo::ProjectSession::Subscription sessionSubscription_;
    // One continuous parameter gesture at a time, owner-thread-only. The
    // token is the session's; this facade only remembers its scope.
    std::optional<nemo::ParameterAddress> parameterGestureAddress_;
    nemo::ParameterGestureToken parameterGestureToken_{};
    bool parameterGestureKeyed_{false};
};
}  // namespace nemo::ui
