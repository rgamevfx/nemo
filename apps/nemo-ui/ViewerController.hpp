#pragma once

#include "ViewerRuntime.hpp"
#include "nemo/core/evaluation/ViewerResolution.hpp"

#include <QPointer>
#include <QRectF>
#include <QSizeF>
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
    Q_PROPERTY(QString resolutionMode READ resolutionMode WRITE setResolutionMode NOTIFY resolutionChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(QPointF pan READ pan WRITE setPan NOTIFY panChanged)
    Q_PROPERTY(int frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(int effectiveScale READ effectiveScale NOTIFY effectiveScaleChanged)
    Q_PROPERTY(QRectF presentedRegion READ presentedRegion NOTIFY frameArrived)
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
    [[nodiscard]] QString resolutionMode() const { return mode_; }
    [[nodiscard]] double zoom() const { return zoom_; }
    [[nodiscard]] QPointF pan() const { return pan_; }
    [[nodiscard]] int frame() const { return frame_; }
    [[nodiscard]] int effectiveScale() const { return effectiveScale_; }
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
    void frameArrived();
    // Qt handed the rendered frame to the window system. This is not a
    // physical scanout timestamp; benchmark reports name that boundary.
    void framePresented(int frame, int width, int height, bool cacheHit, double requestToSwapMs);

private:
    void buildGraph(const SourceReference& reference);
    void refreshRequest();
    void receive();
    void fail(QString message);
    ViewerRuntime* runtime_;
    Document document_;
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
    QString status_;
    QString error_;
    QString sourceDescription_;
    std::uint64_t generation_{};
    std::uint64_t lastRevision_{};
    std::optional<EvaluationRequest> lastRequest_;
    std::shared_ptr<const ViewerResult> presentation_;
    QList<QPointer<ViewerItem>> items_;
    QPointer<ViewerItem> primary_;
    // Outlives all QML nodes and retains their images through Qt frame-slot
    // completion, including when a viewer panel is closed during a frame.
    std::unique_ptr<WindowPresentationState> presentationState_;
};
}  // namespace nemo::ui
