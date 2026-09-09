#pragma once

#include <QQuickItem>
#include <QtQml/qqmlregistration.h>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vulkan/vulkan.h>

namespace nemo::gpu {
class Device;
}
namespace nemo::ui {
class ViewerController;
struct ViewerResult;

// Controller-owned, render-thread-confined state. QSG nodes register the
// images they use; every Qt frame pins them until that frame slot is reused.
// Closing the final panel cannot destroy the in-flight pins. The controller
// outlives the QML engine, and shutdown drains Qt before destroying this state.
class WindowPresentationState {
public:
    void setNode(const QSGNode* node, std::shared_ptr<const ViewerResult> result);
    void removeNode(const QSGNode* node);
    void beginFrame(QQuickWindow* window, gpu::Device& device);
    // Called at Qt's frameSwapped boundary on the render thread. Reports
    // only a new viewer request actually present in the rendered scene.
    [[nodiscard]] std::shared_ptr<const ViewerResult> takeNewPresentedFrame();

private:
    struct Pin {
        std::shared_ptr<const ViewerResult> result;
        std::set<int> frameSlots;
        bool transitioned{};
    };
    std::map<const QSGNode*, std::shared_ptr<const ViewerResult>> nodes_;
    std::map<VkImage, Pin> pins_;
    std::uint64_t lastReportedRequest_{};
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
    ViewerController* controller_{};
    QRectF displayRect_;
    bool primary_{};
};
}  // namespace nemo::ui
