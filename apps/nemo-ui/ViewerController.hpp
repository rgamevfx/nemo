#pragma once

#include "ViewerRuntime.hpp"
#include "nemo/core/evaluation/ViewerResolution.hpp"
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

// GUI-thread state. Scenegraph reads happen only during updatePaintNode,
// while Qt blocks the GUI thread. The worker sees immutable Document copies.
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
    Q_PROPERTY(QString outputName READ outputName WRITE setOutputName NOTIFY outputChanged)
    Q_PROPERTY(QStringList outputNames READ outputNames NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphNodes READ graphNodes NOTIFY graphChanged)
    Q_PROPERTY(QVariantList graphEdges READ graphEdges NOTIFY graphChanged)
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
    explicit ViewerController(ViewerRuntime* runtime);
    ~ViewerController() override;
    Q_INVOKABLE void openSource(const QString& path);
    Q_INVOKABLE void setResolutionMode(const QString& mode);
    Q_INVOKABLE void setZoom(double zoom);
    Q_INVOKABLE void zoomBy(double factor);
    Q_INVOKABLE void setPan(QPointF pan);
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void setFrame(int frame);
    // Graph/timeline surfaces use these validated command entry points.
    Q_INVOKABLE void addGraphNode(const QString& type, const QString& name);
    Q_INVOKABLE void connectGraphNodes(qulonglong fromNode, int fromPort, qulonglong toNode, int toPort);
    Q_INVOKABLE void setNodeParameter(const QString& nodeName, const QString& key, const QString& value);
    // The current persistent model exposes source timing, not timeline clip
    // occurrences. These edit SourceReference through the command API.
    Q_INVOKABLE void slipTimelineClip(const QString& source, int delta);
    Q_INVOKABLE void retimeTimelineClip(const QString& source, int step);
    Q_INVOKABLE void setOutputName(const QString& name);
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
    [[nodiscard]] QString outputName() const { return outputName_; }
    [[nodiscard]] QStringList outputNames() const;
    [[nodiscard]] QVariantList graphNodes() const;
    [[nodiscard]] QVariantList graphEdges() const;
    [[nodiscard]] QVariantList timelineClips() const;
    [[nodiscard]] bool canUndo() const { return commands_.canUndo(); }
    [[nodiscard]] bool canRedo() const { return commands_.canRedo(); }
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
    void outputChanged();
    void graphChanged();
    void timelineChanged();
    void historyChanged();
    void schedulerChanged();
    void frameArrived();
    // Qt handed the rendered frame to the window system. This is not a
    // physical scanout timestamp; benchmark reports name that boundary.
    void framePresented(int frame, int width, int height, bool cacheHit, double requestToSwapMs);

private:
    void buildGraph(const SourceReference& reference);
    void refreshRequest();
    void receive();
    void fail(QString message);
    void documentChanged();
    void invalidateRequest();
    void pollScheduler();
    ViewerRuntime* runtime_;
    Document document_;
    SourceReference probedSource_;
    CommandStack commands_;
    ViewerResolutionPolicy policy_;
    QSizeF sourceSize_;
    QSizeF viewport_;
    double pixelAspect_{1.0};
    int frameCount_{-1};
    int frame_{};
    int effectiveScale_{1};
    double zoom_{1.0};
    QPointF pan_;
    QString mode_{QStringLiteral("auto")};
    QString outputName_{QStringLiteral("result")};
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
};
}  // namespace nemo::ui
