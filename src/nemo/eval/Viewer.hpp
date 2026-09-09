#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"

namespace nemo::eval {
struct ViewerFrame {
    gpu::Image image;  // Display-referred RGBA32F, GENERAL, GPU complete.
    ImageLayout layout;
    EvaluationRequest request;
    std::uint64_t revision{};
    std::uint64_t requestId{};
};

// Worker-confined orchestration over the shared native dependency plan:
// source decode -> native effects -> GPU OCIO. Never host pixel readback.
// Matching scene-linear results reuse #9's cache; distinct representations
// coexist. Returned display images are exclusive, ready for presentation.
// timeout_ns bounds individual GPU waits, not CPU decoding/compilation or
// the total request. Cancellation/publication policy belongs to the caller;
// GPU resource retirement always belongs to #22's submission mechanism.
class ViewerSession {
public:
    ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& shaderDirectory);
    ~ViewerSession();
    ViewerSession(const ViewerSession&) = delete;
    ViewerSession& operator=(const ViewerSession&) = delete;
    [[nodiscard]] ViewerFrame render(const Document& document, const EvaluationRequest& request,
                                     std::uint64_t timeout_ns = 10'000'000'000ULL);
    struct SourceProbe {
        media::ClipInfo info;
        media::DecodeDecision decision;  // Selected candidate before decoding.
    };
    // Opens an independent decoder. May block; do not call on the UI thread.
    [[nodiscard]] SourceProbe probeSource(const Document& document, const std::string& sourceKey) const;
    [[nodiscard]] CacheCounts reuseCounts() const;

private:
    [[nodiscard]] gpu::GpuViewingTransform& viewingTransformFor(const ColorPolicy& policy);
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::string ocioConfigPath_;  // Resolve $OCIO on first viewing request.
    SourceSession sources_;
    EffectLibrary effects_;
    ResultCache<GpuNodeImage> reuse_;
    std::map<std::pair<std::string, std::string>, std::unique_ptr<gpu::GpuViewingTransform>> viewing_;
    std::uint64_t nextRequestId_{1};
};
}  // namespace nemo::eval
