#include "nemo/core/evaluation/CpuReference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "nemo/core/evaluation/Params.hpp"

namespace nemo {

namespace {

// ---------------------------------------------------------------------------
// Image identity (CPU reference side of the plan contract, ADR-0004).
// ---------------------------------------------------------------------------

void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3;
    }
}

void hashLayout(std::uint64_t& hash, const ImageLayout& layout) {
    hashBytes(hash, &layout.width, sizeof(layout.width));
    hashBytes(hash, &layout.height, sizeof(layout.height));
    hashBytes(hash, &layout.pixelAspect, sizeof(layout.pixelAspect));
    for (const auto& channel : layout.channels) {
        hashBytes(hash, channel.data(), channel.size());
    }
    const auto precision = static_cast<std::uint8_t>(layout.precision);
    const auto color = static_cast<std::uint8_t>(layout.color);
    hashBytes(hash, &precision, sizeof(precision));
    hashBytes(hash, &color, sizeof(color));
}

}  // namespace

std::uint64_t cpuImageHash(const CpuImage& image) {
    std::uint64_t hash = 0xcbf29ce484222325;
    hashLayout(hash, image.layout());
    const std::size_t pixelCount = static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height());
    hashBytes(hash, image.data(), pixelCount * CpuImage::channelCount() * sizeof(float));
    return hash;
}

namespace {

ImageIdentity identityOf(const CpuImage& image, Residency residency) {
    ImageIdentity identity;
    identity.contentHash = cpuImageHash(image);
    identity.layout = image.layout();
    identity.residency = residency;
    return identity;
}

// ---------------------------------------------------------------------------
// Node implementations. Each fills the effective parameters it actually
// consumed, so the plan records resolved state, not authored guesses.
// ---------------------------------------------------------------------------

void evalTestpattern(const Node& /*node*/, const EvaluationRequest& request,
                     std::map<std::string, std::string>& /*effectiveParams*/, CpuImage& out) {
    // Deterministic reference pattern: horizontal red gradient, vertical
    // green gradient, and a blue bar whose position tracks local time. Any
    // change here is an observable image change.
    //
    // Region semantics (declared contract for the native executor, issue
    // #8): the pattern is anchored to the full-resolution frame implied by
    // the request — coordinates evaluate at (region.x + x, region.y + y)
    // over dimensions (region.x + width, region.y + height). A full-frame
    // request (origin 0) is unchanged; a region-limited request preserves
    // full-resolution coordinate semantics instead of restarting the
    // gradient at the crop origin.
    const int width = out.width();
    const int height = out.height();
    const int fullX = request.region.x;
    const int fullY = request.region.y;
    const int fullWidth = fullX + width;
    const int fullHeight = fullY + height;
    const int barWidth = std::max(2, fullWidth / 16);
    const int barPos = static_cast<int>((request.localTime * (fullWidth / 8)) % (fullWidth + barWidth));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double u = fullWidth > 1 ? static_cast<double>(fullX + x) / (fullWidth - 1) : 0.0;
            const double v = fullHeight > 1 ? static_cast<double>(fullY + y) / (fullHeight - 1) : 0.0;
            const int fullPixelX = fullX + x;
            const bool inBar = fullPixelX >= barPos && fullPixelX < barPos + barWidth;
            out.setPixel(x, y, {static_cast<float>(u), static_cast<float>(v), inBar ? 1.0F : 0.0F, 1.0F});
        }
    }
}

void evalConstcolor(const Node& node, const EvaluationRequest& /*request*/,
                    std::map<std::string, std::string>& effectiveParams, CpuImage& out) {
    const std::array<float, 4> color = parseColor4(node, effectiveParams, "color", {1.0F, 1.0F, 1.0F, 1.0F});
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            out.setPixel(x, y, color);
        }
    }
}

void evalMerge(const Node& node, const EvaluationRequest&, std::map<std::string, std::string>& effectiveParams,
               const std::vector<CpuImage>& inputs, CpuImage& out) {
    const std::string operation = effectiveParams.count("operation") > 0 ? effectiveParams.at("operation") : [&] {
        effectiveParams.emplace("operation", "over");
        return std::string{"over"};
    }();
    if (operation != "over") {
        failNode(node, "unsupported merge operation '" + operation + "' (CPU reference implements 'over' only)");
    }
    const CpuImage& base = inputs[0];    // port A: over base (background)
    const CpuImage& source = inputs[1];  // port B: over source (foreground)
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const std::array<float, 4> bg = base.pixel(x, y);
            const std::array<float, 4> fg = source.pixel(x, y);
            // Straight-alpha "over": out = fg.a*fg + (1 - fg.a)*bg.
            const float alpha = fg[3] + (1.0F - fg[3]) * bg[3];
            std::array<float, 4> result{};
            for (int c = 0; c < 3; ++c) {
                result[c] = fg[3] * fg[c] + (1.0F - fg[3]) * bg[c];
            }
            result[3] = alpha;
            out.setPixel(x, y, result);
        }
    }
}

void evalOutput(const Node&, const EvaluationRequest&, std::map<std::string, std::string>&,
                const std::vector<CpuImage>& inputs, CpuImage& out) {
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            out.setPixel(x, y, inputs[0].pixel(x, y));
        }
    }
}

// ---------------------------------------------------------------------------
// Topological scheduling and shared request validation.
// ---------------------------------------------------------------------------

const Node* findNode(const Document& document, NodeId id) {
    return document.graph.node(id);
}
}  // namespace

// Collects the required dependency set of `output` (spec section 10.3:
// schedule only required dependencies), then orders it dependencies-first.
// Graph::connect rejects cycles, so a simple in-degree pass terminates.
// Shared by the CPU reference and the native GPU effect executor (issue
// #8): both consume the same scheduled plan.
std::vector<const Node*> scheduleDependencies(const Document& document, NodeId output) {
    std::set<NodeId> required{output};
    std::vector<NodeId> stack{output};
    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();
        for (const auto& edge : document.graph.edgesInto(current)) {
            if (required.insert(edge.from.node).second) {
                stack.push_back(edge.from.node);
            }
        }
    }

    std::map<NodeId, std::size_t> pendingInputs;
    for (const NodeId id : required) {
        std::size_t count = 0;
        for (const auto& edge : document.graph.edgesInto(id)) {
            if (required.contains(edge.from.node)) {
                ++count;
            }
        }
        pendingInputs.emplace(id, count);
    }

    std::vector<const Node*> order;
    std::vector<NodeId> ready;
    for (const auto& [id, count] : pendingInputs) {
        if (count == 0) {
            ready.push_back(id);
        }
    }
    while (!ready.empty()) {
        const NodeId id = ready.back();
        ready.pop_back();
        order.push_back(findNode(document, id));
        for (const auto& edge : document.graph.edges()) {
            if (edge.from.node != id || !required.contains(edge.to.node)) {
                continue;
            }
            auto& count = pendingInputs.at(edge.to.node);
            if (--count == 0) {
                ready.push_back(edge.to.node);
            }
        }
    }
    return order;
}

NodeId resolveOutput(const Document& document, const std::string& outputName) {
    std::vector<const Node*> outputs;
    for (const auto& node : document.graph.nodes()) {
        if (node.type == "output") {
            outputs.push_back(&node);
        }
    }
    if (!outputName.empty()) {
        const Node* named = document.graph.nodeByName(outputName);
        if (named == nullptr) {
            throw EvaluationException("no node named '" + outputName + "' in document '" + document.name + "'");
        }
        if (named->type != "output") {
            throw EvaluationException(describeNode(*named) + ": --output must name an Output node");
        }
        return named->id;
    }
    if (outputs.empty()) {
        throw EvaluationException("document '" + document.name +
                                  "' has no Output node: add an output node for evaluation to produce an image");
    }
    if (outputs.size() > 1) {
        std::ostringstream names;
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            names << (i == 0 ? "" : ", ") << "'" << outputs[i]->name << "' (id " << outputs[i]->id << ")";
        }
        throw EvaluationException("document '" + document.name + "' has multiple Output nodes (" +
                                  std::move(names).str() + "); disambiguate with --output");
    }
    return outputs.front()->id;
}

// Shared request validation for both executors (CPU reference and native
// GPU, issue #8): quality (spec section 8: a reduced-quality result must
// not substitute for a full-quality request), channels, region bounds, and
// that the request targets an existing Output node.
void validateRequest(const Document& document, const EvaluationRequest& request) {
    if (request.quality != Quality::Full) {
        throw EvaluationException(std::string("quality '") + qualityName(request.quality) +
                                  "' is not implemented by this executor (spec section 8: reduced quality must "
                                  "not substitute for full quality)");
    }
    if (request.channels != "RGBA") {
        throw EvaluationException("channels '" + request.channels + "' are not implemented (supported: RGBA)");
    }
    if (request.region.width <= 0 || request.region.height <= 0) {
        throw EvaluationException("request region must have positive width and height");
    }
    constexpr int kMaxDimension = 8192;
    if (request.region.width > kMaxDimension || request.region.height > kMaxDimension) {
        throw EvaluationException("request region exceeds the " + std::to_string(kMaxDimension) +
                                  " pixel reference limit");
    }
    const Node* output = findNode(document, request.output);
    if (output == nullptr) {
        throw EvaluationException("request output node " + std::to_string(request.output) + " does not exist");
    }
    if (output->type != "output") {
        failNode(*output, "evaluation request must target an Output node");
    }
}

std::vector<NodeId> resolveStepInputs(const Document& document, const Node& node,
                                      const std::map<NodeId, ImageIdentity>& evaluated, PlanStep& step) {
    const auto& inPorts = inputPorts(node.type);
    std::vector<NodeId> producers;
    for (std::uint32_t port = 0; port < inPorts.size(); ++port) {
        const Edge* edge = nullptr;
        for (const auto& candidate : document.graph.edgesInto(node.id)) {
            if (candidate.to.port == port) {
                edge = &candidate;
                break;
            }
        }
        if (edge == nullptr || !evaluated.contains(edge->from.node)) {
            std::ostringstream what;
            what << "input port " << port << " ('" << inPorts[port].name << "') is not connected";
            failNode(node, std::move(what).str());
        }
        step.inputs.push_back(edge->from.node);
        step.inputImages.push_back(evaluated.at(edge->from.node));
        producers.push_back(edge->from.node);
    }
    return producers;
}

CpuEvaluation evaluateCpu(const Document& document, EvaluationRequest request) {
    validateRequest(document, request);

    const std::vector<const Node*> order = scheduleDependencies(document, request.output);

    // Execute dependencies-first; each step's image identity feeds the plan.
    std::map<NodeId, CpuImage> images;
    std::map<NodeId, ImageIdentity> identities;
    EvaluationPlan plan;
    plan.request = request;

    for (const Node* node : order) {
        PlanStep step;
        step.node = node->id;
        step.type = node->type;
        step.name = node->name;
        step.effectiveParams = node->params;

        // Resolve inputs in declared port order; every required dependency
        // must be connected and already evaluated.
        const std::vector<NodeId> producers = resolveStepInputs(document, *node, identities, step);
        std::vector<CpuImage> inputs;
        for (const NodeId producer : producers) {
            inputs.push_back(images.at(producer));
        }

        CpuImage image(request.region.width, request.region.height);
        if (node->type == "testpattern") {
            evalTestpattern(*node, request, step.effectiveParams, image);
        } else if (node->type == "constcolor") {
            evalConstcolor(*node, request, step.effectiveParams, image);
        } else if (node->type == "merge") {
            evalMerge(*node, request, step.effectiveParams, inputs, image);
        } else if (node->type == "output") {
            evalOutput(*node, request, step.effectiveParams, inputs, image);
        } else {
            failNode(*node, "type '" + node->type + "' has no CPU reference implementation");
        }

        step.produced = identityOf(image, Residency::HostCpuReference);
        identities.emplace(node->id, step.produced);
        images.emplace(node->id, std::move(image));
        plan.steps.push_back(std::move(step));
    }

    plan.result = identities.at(request.output);
    CpuEvaluation evaluation;
    evaluation.plan = std::move(plan);
    evaluation.image = std::move(images.at(request.output));
    return evaluation;
}

}  // namespace nemo
