#include "ViewerItem.hpp"
#include "ViewerController.hpp"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <algorithm>

namespace nemo::ui {
namespace {
class ViewerNode final : public QSGSimpleTextureNode {
public:
    explicit ViewerNode(WindowPresentationState& state) : state_(state) {}
    ~ViewerNode() override { state_.removeNode(this); }
    void adopt(QSGTexture* texture, std::shared_ptr<const ViewerResult> result) {
        texture_.reset(texture);
        setTexture(texture_.get());
        requestId = result->requestId;
        state_.setNode(this, std::move(result));
    }
    std::uint64_t requestId{};

private:
    WindowPresentationState& state_;
    std::unique_ptr<QSGTexture> texture_;
};
}  // namespace

void WindowPresentationState::setNode(const QSGNode* node, std::shared_ptr<const ViewerResult> result) {
    nodes_[node] = std::move(result);
}
void WindowPresentationState::removeNode(const QSGNode* node) {
    nodes_.erase(node);
}
void WindowPresentationState::beginFrame(QQuickWindow* window, gpu::Device& device) {
    const int slot = window->graphicsStateInfo().currentFrameSlot;
    for (auto it = pins_.begin(); it != pins_.end();) {
        it->second.frameSlots.erase(slot);  // QRhi already waited this slot's fence.
        const bool current = std::any_of(nodes_.begin(), nodes_.end(), [&](const auto& node) {
            return node.second->presentation.image.handle() == it->first;
        });
        if (it->second.frameSlots.empty() && !current)
            it = pins_.erase(it);
        else
            ++it;
    }
    for (const auto& [node, result] : nodes_) {
        (void)node;
        const auto image = result->presentation.image.handle();
        auto& pin = pins_[image];
        pin.result = result;
        pin.frameSlots.insert(slot);
        if (pin.transitioned)
            continue;
        window->beginExternalCommands();
        const auto* resource =
            window->rendererInterface()->getResource(window, QSGRendererInterface::CommandListResource);
        if (!resource)
            qFatal("viewer: Qt Vulkan command buffer unavailable during beforeRendering");
        const auto command = *static_cast<const VkCommandBuffer*>(resource);
        gpu::acquireViewerPresentation(device, result->presentation, command);
        window->endExternalCommands();
        pin.transitioned = true;
    }
}

std::vector<PresentedFrame> WindowPresentationState::takeNewPresentedFrames() {
    // One representative per destination: the scene may hold several nodes for
    // a destination during a panel relayout, and only its newest request is a
    // presentation of that destination.
    std::map<eval::ViewerDestination, std::shared_ptr<const ViewerResult>> latest;
    for (const auto& [node, result] : nodes_) {
        (void)node;
        auto& current = latest[result->destination];
        if (!current || result->requestId > current->requestId)
            current = result;
    }
    std::vector<PresentedFrame> presented;
    for (auto& [destination, result] : latest) {
        auto& reported = lastReportedRequest_[destination];
        if (result->requestId <= reported)
            continue;
        reported = result->requestId;
        presented.push_back(PresentedFrame{destination, std::move(result)});
    }
    return presented;
}

ViewerItem::ViewerItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}
ViewerItem::~ViewerItem() {
    if (controller_)
        controller_->detachViewerItem(this);
}
QObject* ViewerItem::controller() const {
    return controller_;
}
void ViewerItem::setController(QObject* value) {
    auto* next = qobject_cast<ViewerController*>(value);
    if (next == controller_)
        return;
    if (controller_) {
        controller_->detachViewerItem(this);
        disconnect(controller_, nullptr, this, nullptr);
    }
    controller_ = next;
    if (controller_) {
        connect(controller_, &ViewerController::frameArrived, this, &QQuickItem::update);
        controller_->attachViewerItem(this);
    }
    emit controllerChanged();
    reportViewport();
    update();
}
void ViewerItem::setDisplayRect(QRectF rect) {
    if (rect == displayRect_)
        return;
    displayRect_ = rect;
    emit displayRectChanged();
    update();
}
void ViewerItem::setPrimary(bool primary) {
    primary_ = primary;
    reportViewport();
}
void ViewerItem::makePrimary() {
    if (controller_)
        controller_->setPrimaryViewerItem(this);
}
void ViewerItem::reportViewport() {
    if (controller_ && primary_ && window() && isVisible()) {
        const auto dpr = window()->effectiveDevicePixelRatio();
        controller_->viewportChanged(QSizeF(width() * dpr, height() * dpr));
    }
}
void ViewerItem::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    reportViewport();
}
void ViewerItem::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange || change == ItemVisibleHasChanged || change == ItemDevicePixelRatioHasChanged)
        reportViewport();
}
QSGNode* ViewerItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto result = controller_ ? controller_->presentation() : nullptr;
    if (!result || !window() || width() <= 0 || height() <= 0) {
        std::unique_ptr<QSGNode> retired(old);
        return nullptr;
    }
    auto* node = static_cast<ViewerNode*>(old);
    std::unique_ptr<ViewerNode> created;
    if (!node) {
        created = std::make_unique<ViewerNode>(controller_->presentationState());
        node = created.get();
    }
    if (node->requestId != result->requestId) {
        auto* texture = QNativeInterface::QSGVulkanTexture::fromNative(
            result->presentation.image.handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, window(),
            QSize(result->frame.width, result->frame.height), QQuickWindow::TextureHasAlphaChannel);
        if (!texture)
            qFatal("viewer: Qt failed to import the Vulkan presentation image");
        node->adopt(texture, result);
        node->setFiltering(controller_->filterLinear() ? QSGTexture::Linear : QSGTexture::Nearest);
        node->setSourceRect(QRectF(0, 0, result->frame.width, result->frame.height));
    }
    node->setRect(displayRect_);
    if (created)
        return created.release();  // Ownership transferred to Qt's scenegraph.
    return node;
}
}  // namespace nemo::ui
