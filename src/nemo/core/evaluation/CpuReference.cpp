#include "nemo/core/evaluation/CpuReference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/Reuse.hpp"
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
    // Sample the full-resolution image domain, not the ROI's dimensions.
    // Cropping and reduced sampling never re-normalize the generator.
    const int scale = request.samplingScale;
    const int width = out.width();
    const int height = out.height();
    const int fullX = request.region.x;
    const int fullY = request.region.y;
    const int fullWidth = request.imageWidth();
    const int fullHeight = request.imageHeight();
    const int barWidth = std::max(2, fullWidth / 16);
    const int barPos = static_cast<int>((request.localTime * (fullWidth / 8)) % (fullWidth + barWidth));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int fullPixelX = fullX + x * scale;
            const int fullPixelY = fullY + y * scale;
            const double u = fullWidth > 1 ? static_cast<double>(fullPixelX) / (fullWidth - 1) : 0.0;
            const double v = fullHeight > 1 ? static_cast<double>(fullPixelY) / (fullHeight - 1) : 0.0;
            const bool inBar = fullPixelX >= barPos && fullPixelX < barPos + barWidth;
            out.setPixel(x, y, {static_cast<float>(u), static_cast<float>(v), inBar ? 1.0F : 0.0F, 1.0F});
        }
    }
}

void evalConstcolor(const NodeCatalog& catalog, const Node& node, const EvaluationRequest& /*request*/,
                    std::map<std::string, std::string>& effectiveParams, CpuImage& out) {
    const std::array<float, 4> color = parseColor4(catalog, node, effectiveParams, "color");
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            out.setPixel(x, y, color);
        }
    }
}

void evalMerge(const NodeCatalog& catalog, const Node& node, const EvaluationRequest&,
               std::map<std::string, std::string>& effectiveParams, const std::vector<const CpuImage*>& inputs,
               CpuImage& out) {
    const std::string& operation = effectiveParameter(catalog, node, effectiveParams, "operation");
    if (operation != "over") {
        failNode(node, "unsupported merge operation '" + operation + "' (CPU reference implements 'over' only)");
    }
    const CpuImage& base = *inputs[0];    // port A: over base (background)
    const CpuImage& source = *inputs[1];  // port B: over source (foreground)
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
                const std::vector<const CpuImage*>& inputs, CpuImage& out) {
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            out.setPixel(x, y, inputs[0]->pixel(x, y));
        }
    }
}

// Real source media (issue #11). The reference lives in the Document; the
// pixels come from the provider. There is NO synthetic fallback: an
// unresolved or unprovided source is an explicit evaluation error that
// identifies the node.
void evalSource(const Document& document, const Node& node, const EvaluationRequest& request,
                std::map<std::string, std::string>& effectiveParams, CpuImage& out, SourceProvider* provider) {
    const auto keyIt = node.params.find("source");
    if (keyIt == node.params.end() || keyIt->second.empty()) {
        failNode(node, "source node has no 'source' parameter naming a document source");
    }
    const std::string& key = keyIt->second;
    const auto referenceIt = document.sources.find(key);
    if (referenceIt == document.sources.end()) {
        failNode(node, "unresolved source '" + key +
                           "': no source reference with this key in the document (real media is never "
                           "evaluated as synthetic content)");
    }
    const SourceReference& reference = referenceIt->second;
    effectiveParams["source"] = key;
    effectiveParams["sourcePath"] = reference.path;
    std::int64_t mappedFrame = 0;
    try {
        mappedFrame = reference.frameAt(request.localTime);
    } catch (const std::exception& error) {
        failNode(node, std::string("source time mapping failed: ") + error.what());
    }
    effectiveParams["frame"] = std::to_string(mappedFrame);
    if (provider == nullptr) {
        failNode(node, "source '" + key + "' (" + reference.path +
                           ") requires a decode provider; this "
                           "executor cannot evaluate real media and never substitutes synthetic content");
    }
    CpuImage decoded;
    try {
        decoded = provider->frame(document, reference, mappedFrame, request);
    } catch (const EvaluationException&) {
        throw;
    } catch (const std::exception& error) {
        failNode(node, "source provider failed for '" + key + "' at frame " + std::to_string(mappedFrame) + ": " +
                           error.what());
    }
    const int expectedWidth = scaledDimension(request.region.width, request.samplingScale);
    const int expectedHeight = scaledDimension(request.region.height, request.samplingScale);
    if (decoded.width() != expectedWidth || decoded.height() != expectedHeight) {
        failNode(node, "source '" + key + "' decoded raster " + std::to_string(decoded.width()) + "x" +
                           std::to_string(decoded.height()) + " does not cover the requested raster " +
                           std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight) +
                           " (full-resolution region at sampling scale " + std::to_string(request.samplingScale) + ")");
    }
    for (int y = 0; y < decoded.height(); ++y) {
        for (int x = 0; x < decoded.width(); ++x) {
            out.setPixel(x, y, decoded.pixel(x, y));
        }
    }
}

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
        const auto* schema = document.graph.descriptor(node.type);
        if (schema != nullptr && schema->isOutput)
            outputs.push_back(&node);
    }
    if (!outputName.empty()) {
        const Node* named = document.graph.nodeByName(outputName);
        if (named == nullptr) {
            throw EvaluationException("no node named '" + outputName + "' in document '" + document.name + "'");
        }
        const auto* schema = document.graph.descriptor(named->type);
        if (schema == nullptr || !schema->isOutput) {
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
// GPU, issues #8/#11): executor support (currently Full/RGBA), region
// bounds, output identity, and every declared capability of every scheduled
// dependency. Unsupported metadata is an explicit error, never a silent
// approximation or executor substitution.
void validateRequest(const Document& document, const EvaluationRequest& request) {
    // These are executor limitations, not schema declarations. A future
    // executor may advertise more modes/channels, but this CPU/GPU pair
    // currently implements only the full-quality RGBA contract.
    if (!isSamplingScale(request.samplingScale)) {
        throw EvaluationException("sampling scale " + std::to_string(request.samplingScale) +
                                  " is not a declared reduction (supported scales: 1, 2, 4; spec section 8: "
                                  "reductions are explicit, never silent)");
    }
    if (request.region.width <= 0 || request.region.height <= 0) {
        throw EvaluationException("request region must have positive width and height");
    }
    constexpr int kMaxDimension = 8192;
    if (request.region.width > kMaxDimension || request.region.height > kMaxDimension) {
        throw EvaluationException("request region exceeds the " + std::to_string(kMaxDimension) +
                                  " pixel reference limit");
    }
    if (request.imageWidth() > kMaxDimension || request.imageHeight() > kMaxDimension)
        throw EvaluationException("full image domain exceeds the 8192 pixel reference limit");
    if (request.region.x < 0 || request.region.y < 0 || request.fullWidth < 0 || request.fullHeight < 0)
        throw EvaluationException("image domain and region origin must be nonnegative");
    if ((request.fullWidth == 0) != (request.fullHeight == 0) ||
        ((request.region.x != 0 || request.region.y != 0) && request.fullWidth == 0))
        throw EvaluationException("cropped requests require explicit fullWidth and fullHeight");
    if (request.region.x > request.imageWidth() - request.region.width ||
        request.region.y > request.imageHeight() - request.region.height)
        throw EvaluationException("requested region lies outside the full-resolution image domain");
    const Node* output = findNode(document, request.output);
    if (output == nullptr) {
        throw EvaluationException("request output node " + std::to_string(request.output) + " does not exist");
    }
    const auto* outputSchema = document.graph.descriptor(output->type);
    if (outputSchema == nullptr || !outputSchema->isOutput) {
        failNode(*output, "evaluation request must target an Output node");
    }

    const bool wholeImage = request.region.x == 0 && request.region.y == 0 &&
                            request.region.width == request.imageWidth() &&
                            request.region.height == request.imageHeight();
    // A request is valid only when every scheduled dependency advertises the
    // requested metadata. The executor checks above remain separate: a
    // descriptor can be registered and discoverable without supplying a CPU
    // or GPU implementation.
    for (const Node* node : scheduleDependencies(document, request.output)) {
        const auto* schema = document.graph.descriptor(node->type);
        if (schema == nullptr) {
            // Unknown persisted types remain recoverable. Preserve the
            // existing explicit scale failure for such a type, while its
            // executor-specific failure handles a full-resolution request.
            if (request.samplingScale != 1)
                failNode(*node, "sampling scale " + std::to_string(request.samplingScale) +
                                    " cannot be validated because the node type has no descriptor");
            continue;
        }
        const auto& capabilities = schema->capabilities;
        if (std::find(capabilities.qualityModes.begin(), capabilities.qualityModes.end(), request.quality) ==
            capabilities.qualityModes.end()) {
            failNode(*node, std::string("quality '") + qualityName(request.quality) +
                                "' is not declared by node type '" + node->type + "'");
        }
        if (std::find(capabilities.channels.begin(), capabilities.channels.end(), request.channels) ==
            capabilities.channels.end()) {
            failNode(*node, "channels '" + request.channels + "' are not declared by node type '" + node->type + "'");
        }
        if (std::find(capabilities.samplingScales.begin(), capabilities.samplingScales.end(), request.samplingScale) ==
            capabilities.samplingScales.end()) {
            failNode(*node, "sampling scale " + std::to_string(request.samplingScale) +
                                " is not declared by node type '" + node->type + "'");
        }
        if (!capabilities.supportsRegion && !wholeImage) {
            failNode(*node, "does not support region-of-interest requests");
        }
    }
    // Apply the executor's narrower implementation contract only after the
    // per-node declarations have been checked, so declaration violations
    // retain the offending node context.
    if (request.quality != Quality::Full) {
        throw EvaluationException(std::string("quality '") + qualityName(request.quality) +
                                  "' is not implemented by this executor (spec section 8: reduced quality must "
                                  "not substitute for full quality)");
    }
    if (request.channels != "RGBA") {
        throw EvaluationException("channels '" + request.channels + "' are not implemented (supported: RGBA)");
    }
}

std::vector<NodeId> resolveStepInputs(const Document& document, const Node& node,
                                      const std::map<NodeId, ImageIdentity>& evaluated, PlanStep& step) {
    const auto& inPorts = document.graph.inputPortsFor(node.type);
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

CpuEvaluation evaluateCpu(const Document& document, EvaluationRequest request, ResultCache<CpuImage>* reuse,
                          SourceProvider* sources) {
    validateRequest(document, request);

    const std::vector<const Node*> order = scheduleDependencies(document, request.output);
    // Publication freshness (issue #9): capture revision + generation at
    // request start; computed results publish only while both hold.
    const EvaluationTicket ticket = reuse != nullptr ? reuse->beginTicket(document) : EvaluationTicket{};

    // Execute dependencies-first; each step's image identity feeds the plan.
    // Results live in the cache (or locally when no cache is given) as
    // shared ownership so a downstream step can read an image the cache
    // also retains.
    std::map<NodeId, std::shared_ptr<const CpuImage>> images;
    std::map<NodeId, ImageIdentity> identities;
    std::map<NodeId, ResultKey> keys;
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

        // Reuse identity: effective state, including the effective input
        // results' keys in port order (spec section 10.3).
        std::vector<std::uint64_t> inputKeyHashes;
        inputKeyHashes.reserve(producers.size());
        for (const NodeId producer : producers) {
            inputKeyHashes.push_back(keys.at(producer).hash);
        }
        const ResultKey key = nodeResultKey(document, *node, inputKeyHashes, request);
        keys.emplace(node->id, key);

        std::shared_ptr<const CpuImage> image;
        if (reuse != nullptr) {
            if (const std::optional<ResultCache<CpuImage>::Entry> hit = reuse->find(key)) {
                image = hit->image;
                step.produced = hit->identity;
                step.cacheReused = true;
            }
        }

        if (!image) {
            auto fresh = std::make_shared<CpuImage>(scaledDimension(request.region.width, request.samplingScale),
                                                    scaledDimension(request.region.height, request.samplingScale));
            std::vector<const CpuImage*> inputs;
            inputs.reserve(producers.size());
            for (const NodeId producer : producers) {
                inputs.push_back(images.at(producer).get());
            }

            if (node->type == "testpattern") {
                evalTestpattern(*node, request, step.effectiveParams, *fresh);
            } else if (node->type == "source") {
                evalSource(document, *node, request, step.effectiveParams, *fresh, sources);
            } else if (node->type == "constcolor") {
                evalConstcolor(document.graph.catalog(), *node, request, step.effectiveParams, *fresh);
            } else if (node->type == "merge") {
                evalMerge(document.graph.catalog(), *node, request, step.effectiveParams, inputs, *fresh);
            } else if (node->type == "output") {
                evalOutput(*node, request, step.effectiveParams, inputs, *fresh);
            } else if (document.graph.descriptor(node->type) != nullptr) {
                failNode(*node, "declared node type has no CPU reference implementation (executor unavailable)");
            } else {
                failNode(*node, "unknown node type has no CPU reference implementation");
            }

            step.produced = identityOf(*fresh, Residency::HostCpuReference);
            image = fresh;
            if (reuse != nullptr) {
                reuse->publish(document, ticket, key, fresh, step.produced);
            }
        }

        identities.emplace(node->id, step.produced);
        images.emplace(node->id, std::move(image));
        plan.steps.push_back(std::move(step));
    }

    plan.result = identities.at(request.output);
    CpuEvaluation evaluation;
    evaluation.plan = std::move(plan);
    // The CPU reference is a correctness reference (ADR-0004); when reuse
    // is active the output image is shared with the cache and copied out.
    evaluation.image = *images.at(request.output);
    return evaluation;
}

}  // namespace nemo
