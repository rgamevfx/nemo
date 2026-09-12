#pragma once

#include "nemo/eval/ViewerDestination.hpp"

#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <vulkan/vulkan.h>

namespace nemo::gpu {
class Device;
}
namespace nemo::ui {
class ViewerController;
struct ViewerResult;

// One destination's newly presented frame. The destination scopes the report
// so one panel's presentation is never reported as another panel's.
struct PresentedFrame {
    eval::ViewerDestination destination{};
    std::shared_ptr<const ViewerResult> result;
};

// Runtime-owned, render-thread-confined state shared by every panel's
// ViewerItem. QSG nodes register the images they use; every Qt frame pins
// them until that frame slot is reused. Closing one panel cannot destroy
// another panel's in-flight pins. The runtime outlives the QML engine, and
// shutdown drains Qt before destroying this state.
class WindowPresentationState {
public:
    void setNode(const QSGNode* node, std::shared_ptr<const ViewerResult> result);
    void removeNode(const QSGNode* node);
    void beginFrame(QQuickWindow* window, gpu::Device& device);
    // Called at Qt's frameSwapped boundary on the render thread. Reports one
    // entry for every destination whose rendered scene holds a newer viewer
    // request than the previous call reported for that destination.
    [[nodiscard]] std::vector<PresentedFrame> takeNewPresentedFrames();

private:
    struct Pin {
        std::shared_ptr<const ViewerResult> result;
        std::set<int> frameSlots;
        bool transitioned{};
    };
    std::map<const QSGNode*, std::shared_ptr<const ViewerResult>> nodes_;
    std::map<VkImage, Pin> pins_;
    // Last request reported per destination; a destination only ever reports
    // a newer request than the one it already reported.
    std::map<eval::ViewerDestination, std::uint64_t> lastReportedRequest_;
};

class ViewerItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(QRectF displayRect READ displayRect WRITE setDisplayRect NOTIFY displayRectChanged)
public:
    explicit ViewerItem(QQuickItem* parent = nullptr);
    ~ViewerItem() override;
    [[nodiscard]] QObject* controller() const;
    void setController(QObject* controller);
    [[nodiscard]] QRectF displayRect() const { return displayRect_; }
    void setDisplayRect(QRectF rect);
    void setPrimary(bool primary);
    Q_INVOKABLE void makePrimary();
signals:
    void controllerChanged();
    void displayRectChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    void reportViewport();
    // Borrowed from ViewerControllerRegistry. The registry retires a panel's
    // controller with deleteLater() when the panel is removed, while this
    // item may still be destroyed on a later event-loop pass; a weak pointer
    // keeps that destruction (and setController) from dereferencing a
    // controller that has already been retired.
    QPointer<ViewerController> controller_;
    QRectF displayRect_;
    bool primary_{};
};
}  // namespace nemo::ui
