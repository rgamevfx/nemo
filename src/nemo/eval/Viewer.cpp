#include "nemo/eval/Viewer.hpp"
#include "nemo/core/evaluation/Params.hpp"

#include <utility>

#include "nemo/media/ViewingTransform.hpp"

namespace nemo::eval {

ViewerSession::ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                             const std::filesystem::path& shaderDirectory)
    : device_(device), allocator_(allocator),
      sources_(instance, device, allocator, shaderDirectory / "mediaConvert.spv"),
      effects_(loadSlangEffectLibrary(shaderDirectory, shaderDirectory)), reuse_(512) {}

ViewerSession::~ViewerSession() = default;

ViewerSession::SourceProbe ViewerSession::probeSource(const Document& document, const std::string& sourceKey) const {
    const SourceSession::Probe probe = sources_.probe(document, sourceKey);
    return SourceProbe{probe.info, probe.decision};
}

CacheCounts ViewerSession::reuseCounts() const {
    return reuse_.counts();
}

gpu::GpuViewingTransform& ViewerSession::viewingTransformFor(const ColorPolicy& policy) {
    if (ocioConfigPath_.empty())
        ocioConfigPath_ = media::resolveConfigPath({});
    const auto key = std::pair{policy.workingSpace, policy.viewerTransform};
    // GpuViewingTransform compiles/uploads its LUTs once per program; a
    // policy edit resolves another transform while existing ones stay
    // valid — different representations of the viewing state coexist.
    auto it = viewing_.find(key);
    if (it == viewing_.end()) {
        it = viewing_
                 .emplace(key,
                          std::make_unique<gpu::GpuViewingTransform>(
                              device_, allocator_,
                              media::buildViewingTransformGpu(ocioConfigPath_, policy.workingSpace,
                                                              policy.viewerTransform, media::TransformKind::Viewer)))
                 .first;
    }
    return *it->second;
}

ViewerFrame ViewerSession::render(const Document& document, const EvaluationRequest& request,
                                  std::uint64_t timeout_ns) {
    // Worker-only contract; validate here so a malformed request fails on
    // the caller's thread with a precise reason before any GPU work.
    validateRequest(document, request);

    const auto requestId = nextRequestId_++;

    // 1. Shared dependency plan (synthetic fixtures and decoded real media
    //    take the SAME path): scene-linear reuse under the ticket's
    //    freshness guard; decoded frames flow through SourceSession.
    //    evaluateGpu waits for its completion before returning, so the
    //    composition result is visible on the graphics queue here.
    const auto evaluation =
        evaluateGpu(document, request, effects_, device_, allocator_, timeout_ns, &reuse_, &sources_);

    // 2. Exactly-once interpretation check: the composition result must be
    //    scene-linear. A display-referred result would mean the transform
    //    already ran somewhere — applying it again would double-transform,
    //    so it is REFUSED with the offending node named (spec section 8:
    //    viewing transforms never contaminate reusable results).
    const GpuNodeImage& composition = *evaluation.images.at(request.output);
    if (composition.layout.color != ColorInterpretation::SceneLinear) {
        const Node* node = document.graph.node(request.output);
        throw EvaluationException(
            describeNode(*node) + ": produced a " +
                std::string(composition.layout.color == ColorInterpretation::DisplayReferred ? "display-referred"
                                                                                             : "uninterpreted") +
                " result; the viewing transform is applied exactly once and a second application is refused",
            request.output, node->name);
    }

    // 3. GPU OCIO viewing transform — once. The transform itself re-checks
    //    the declared interpretation before any GPU work (the gpu-module
    //    seam), so a wrong interpretation cannot reach the queue.
    gpu::GpuViewingTransform& transform = viewingTransformFor(document.color);
    std::optional<gpu::GpuViewedImage> viewed = transform.submit(composition.image, composition.layout.color);
    if (!viewed) {
        throw gpu::GpuException(gpu::GpuError::InvalidRequest, "viewer transform submission capacity exhausted");
    }

    // 4. The returned frame is GPU-complete before it leaves the worker;
    //    presentation (gpu::prepareViewerPresentation) is the caller's
    //    next step — the session never quantizes or re-transforms.
    auto& queue = device_.submissions(device_.graphics_family());
    if (!queue.wait(viewed->completion, timeout_ns)) {
        throw gpu::GpuException(gpu::GpuError::SubmissionTimeout,
                                "viewer frame did not complete within " + std::to_string(timeout_ns) + " ns");
    }

    // 5. Identity: the frame records what it was rendered against.
    ViewerFrame frame;
    frame.image = std::move(viewed->image);
    frame.layout = composition.layout;
    frame.layout.color = ColorInterpretation::DisplayReferred;
    frame.request = request;
    frame.revision = document.stateRevision();
    frame.requestId = requestId;
    return frame;
}

}  // namespace nemo::eval
