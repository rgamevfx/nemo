#include "nemo/core/evaluation/CpuReference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <utility>

#include "nemo/core/evaluation/EffectCpu.hpp"
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

const NodeInstance* findNode(const Document& document, NetworkId networkId, NodeId id) {
    return document.network(networkId).graph().node(id);
}
}  // namespace
std::vector<ExpandedNode> expandDependencies(const Document& document, NetworkId rootNetwork, NodeId output) {
    struct Scope;
    struct ExternalBinding {
        const Scope* scope;
        PortRef source;
    };
    struct Scope {
        NetworkId network{kInvalidNetwork};
        NetworkInstanceId instance{kInvalidNetworkInstance};
        std::vector<NetworkInstanceId> path;
        const Scope* parent{};
        std::map<InterfacePortId, ExternalBinding> externalBindings;
    };
    std::vector<ExpandedNode> expanded;
    std::map<EvaluationNodeId, std::size_t> emitted;
    std::set<EvaluationNodeId> active;

    std::function<EvaluationNodeId(const Scope&, NodeId, std::uint32_t)> visit;
    visit = [&](const Scope& scope, NodeId nodeId, std::uint32_t outputPort) -> EvaluationNodeId {
        const NodeInstance* node = document.network(scope.network).graph().node(nodeId);
        if (node == nullptr)
            throw EvaluationException("network " + std::to_string(scope.network) + " has no node " +
                                      std::to_string(nodeId));
        const bool nested = node->definition != kInvalidNetwork;
        const EvaluationNodeId key{scope.network, scope.instance, nodeId, nested ? outputPort : kEvaluationWholeNode,
                                   scope.path};
        if (const auto found = emitted.find(key); found != emitted.end())
            return key;
        if (!active.insert(key).second)
            throw EvaluationException("nested evaluation cycle reaches node " + std::to_string(nodeId));

        const Network& network = document.network(scope.network);
        const Graph& graph = network.graph();
        std::vector<EvaluationNodeId> inputs;
        if (!nested) {
            const auto& ports = graph.inputPorts(nodeId);
            inputs.reserve(ports.size());
            for (std::uint32_t port = 0; port < ports.size(); ++port) {
                const Edge* edge = nullptr;
                for (const auto& candidate : graph.edgesInto(nodeId)) {
                    if (candidate.to.port == port) {
                        edge = &candidate;
                        break;
                    }
                }
                if (edge != nullptr) {
                    inputs.push_back(visit(scope, edge->from.node, edge->from.port));
                    continue;
                }
                const bool optional = ports[port].optional;
                const auto terminal = std::find_if(network.inputConnections().begin(), network.inputConnections().end(),
                                                   [nodeId, port](const TerminalConnection& connection) {
                                                       return connection.node == PortRef{nodeId, port};
                                                   });
                if (terminal == network.inputConnections().end()) {
                    if (optional) {
                        // Absent optional slot: keep the declared-port
                        // position with the invalid sentinel. No source node
                        // is manufactured or evaluated.
                        inputs.push_back(EvaluationNodeId{});
                        continue;
                    }
                    std::ostringstream message;
                    message << "input port " << port << " ('" << ports[port].name << "') on node '" << node->name
                            << "' is not connected";
                    throw EvaluationException(message.str(), node->id, node->name);
                }
                if (scope.parent == nullptr) {
                    if (optional) {
                        inputs.push_back(EvaluationNodeId{});
                        continue;
                    }
                    throw EvaluationException("formal input '" + network.input(terminal->terminal)->name +
                                              "' has no instance binding for node '" + node->name + "'");
                }
                const auto external = scope.externalBindings.find(terminal->terminal);
                if (external == scope.externalBindings.end()) {
                    if (optional) {
                        inputs.push_back(EvaluationNodeId{});
                        continue;
                    }
                    throw EvaluationException("formal input '" + network.input(terminal->terminal)->name +
                                              "' has no instance binding for node '" + node->name + "'");
                }
                inputs.push_back(
                    visit(*external->second.scope, external->second.source.node, external->second.source.port));
            }
        }

        std::optional<EvaluationNodeId> alias;
        if (nested) {
            const NetworkInstance* occurrence = document.instance(node->instance);
            if (occurrence == nullptr || occurrence->parentNetwork != scope.network ||
                occurrence->definition != node->definition) {
                throw EvaluationException("network instance node '" + node->name + "' has an invalid binding");
            }
            const Network& definition = document.network(occurrence->definition);
            Scope child{occurrence->definition, occurrence->id, scope.path, &scope, {}};
            child.path.push_back(occurrence->id);
            for (std::size_t index = 0; index < definition.inputs().size(); ++index) {
                const auto& formal = definition.inputs()[index];
                const auto edge = std::find_if(graph.edgesInto(nodeId).begin(), graph.edgesInto(nodeId).end(),
                                               [index](const Edge& candidate) { return candidate.to.port == index; });
                if (edge != graph.edgesInto(nodeId).end()) {
                    child.externalBindings.emplace(formal.id, ExternalBinding{&scope, edge->from});
                    continue;
                }
                const auto binding = occurrence->inputBindings.find(formal.id);
                if (binding != occurrence->inputBindings.end()) {
                    child.externalBindings.emplace(formal.id, ExternalBinding{&scope, binding->second});
                    continue;
                }
                const auto connection =
                    std::find_if(network.inputConnections().begin(), network.inputConnections().end(),
                                 [nodeId, index](const TerminalConnection& value) {
                                     return value.node == PortRef{nodeId, static_cast<std::uint32_t>(index)};
                                 });
                if (connection != network.inputConnections().end()) {
                    const auto external = scope.externalBindings.find(connection->terminal);
                    if (external != scope.externalBindings.end())
                        child.externalBindings.emplace(formal.id, external->second);
                }
            }
            const auto& outputs = definition.outputs();
            const std::size_t selected = outputPort == kEvaluationWholeNode ? 0 : static_cast<std::size_t>(outputPort);
            if (selected >= outputs.size()) {
                throw EvaluationException("network instance node '" + node->name +
                                          "' selects an unknown formal output port");
            }
            const auto connection =
                std::find_if(definition.outputConnections().begin(), definition.outputConnections().end(),
                             [&outputs, selected](const TerminalConnection& candidate) {
                                 return candidate.terminal == outputs[selected].id;
                             });
            if (connection != definition.outputConnections().end()) {
                alias = visit(child, connection->node.node, connection->node.port);
            } else {
                const auto passThrough = definition.outputInputBindings().find(outputs[selected].id);
                if (passThrough == definition.outputInputBindings().end())
                    throw EvaluationException("formal output '" + outputs[selected].name +
                                              "' has no internal producer");
                const auto external = child.externalBindings.find(passThrough->second);
                if (external == child.externalBindings.end())
                    throw EvaluationException("formal output '" + outputs[selected].name +
                                              "' has no bound pass-through input");
                alias = visit(*external->second.scope, external->second.source.node, external->second.source.port);
            }
            inputs.push_back(*alias);
        }

        active.erase(key);
        emitted.emplace(key, expanded.size());
        expanded.push_back(ExpandedNode{key, node, std::move(inputs), std::move(alias)});
        return key;
    };

    visit(Scope{rootNetwork, kInvalidNetworkInstance, {}, nullptr, {}}, output, kEvaluationWholeNode);
    return expanded;
}

namespace {

// Coverage a node must actually produce for one demand (issue #85): the demand
// rounded outward to whole coverage-padding blocks and clipped to the image
// domain. Padding keeps overlapping pans inside already resident coverage;
// clipping keeps a region inside the data that exists. An empty result (a
// demand entirely outside the domain) escalates to the whole domain rather than
// planning a zero-sized raster.
[[nodiscard]] Region plannedCoverage(const Region& demand, int scale, int domainWidth, int domainHeight) {
    const int block = kCoveragePaddingRasterPixels * (isSamplingScale(scale) ? scale : 1);
    Region coverage = block > 0 ? regionOnLattice(demand, block, domainWidth, domainHeight)
                                : regionOnLattice(demand, scale, domainWidth, domainHeight);
    if (coverage.width <= 0 || coverage.height <= 0)
        coverage = domainRegion(domainWidth, domainHeight);
    return coverage;
}

}  // namespace

RegionPlan planDependencyRegions(const Document& document, const EvaluationRequest& request,
                                 const NodeContributions& contributions,
                                 const std::map<EvaluationNodeId, float>& pixelAspects) {
    RegionPlan plan;
    plan.order = expandDependencies(document, request.network, request.output);
    if (plan.order.empty())
        throw EvaluationException("evaluation plan has no scheduled nodes");
    const EvaluationRequest normalized = canonicalizeRequest(request);
    const int scale = isSamplingScale(normalized.samplingScale) ? normalized.samplingScale : 1;
    const int domainWidth = normalized.imageWidth();
    const int domainHeight = normalized.imageHeight();
    const Region whole = domainRegion(domainWidth, domainHeight);

    // Pixel aspect of every node's own output, dependencies first: generators
    // are square, a source keeps a supplied aspect or reports unknown (never
    // assumed square), and every other node propagates its main input's aspect.
    std::map<EvaluationNodeId, float> aspects;
    for (const ExpandedNode& expanded : plan.order) {
        const NodeContribution* contribution = expanded.alias ? nullptr : contributions.find(expanded.node->type);
        float aspect = 1.0F;
        if (const auto supplied = pixelAspects.find(expanded.id); supplied != pixelAspects.end()) {
            aspect = supplied->second;
        } else if (contribution != nullptr && contribution->role == NodeRole::Source) {
            aspect = 0.0F;
        } else if (!expanded.inputs.empty() && expanded.inputs.front().node != kInvalidNode) {
            aspect = aspects.at(expanded.inputs.front());
        }
        aspects.emplace(expanded.id, aspect);
    }

    // Coverage travels backwards from the requested output, so a node is only
    // visited after every consumer has contributed its demand (the order is
    // dependency-first, hence its reverse is consumer-first).
    std::map<EvaluationNodeId, Region> demand;
    demand.emplace(plan.order.back().id, normalized.region);

    for (auto entry = plan.order.rbegin(); entry != plan.order.rend(); ++entry) {
        const ExpandedNode& expanded = *entry;
        const NodeContribution* contribution = expanded.alias ? nullptr : contributions.find(expanded.node->type);
        // A contribution that declares no regional support processes whole
        // images internally: its own coverage and every input's coverage
        // escalate, and consumers read the sub-region they asked for from it.
        const bool wholeFrameOnly = contribution != nullptr && !contribution->descriptor.capabilities.supportsRegion;

        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expanded, resolvedNode, static_cast<double>(normalized.localTime));
        const Region coverage =
            wholeFrameOnly ? whole : plannedCoverage(demand.at(expanded.id), scale, domainWidth, domainHeight);

        EvaluationRequest nodeRequest = normalized;
        nodeRequest.network = expanded.id.network;
        nodeRequest.region = coverage;
        plan.requests[expanded.id] = nodeRequest;

        // The node's own dependency rule states what it reads per declared
        // input port. Ports it does not cover are read at the node's own
        // coverage; a node with no rule reads every input at its own coverage
        // (the pointwise default).
        std::vector<Region> reads;
        if (!wholeFrameOnly && contribution != nullptr && contribution->inputRegions) {
            const NodeCatalog& catalog = document.network(expanded.id.network).graph().catalog();
            ParameterValues effectiveParams = effectiveNode->params;
            const NodeRegionContext regionContext{catalog, *effectiveNode, nodeRequest, effectiveParams,
                                                  aspects.at(expanded.id)};
            reads = contribution->inputRegions(regionContext);
        }
        for (std::size_t port = 0; port < expanded.inputs.size(); ++port) {
            const EvaluationNodeId& producer = expanded.inputs[port];
            if (producer.node == kInvalidNode)
                continue;  // absent optional slot: no source, no demand
            Region read = coverage;
            if (wholeFrameOnly) {
                read = whole;
            } else if (port < reads.size()) {
                read = plannedCoverage(reads[port], scale, domainWidth, domainHeight);
            }
            const auto found = demand.find(producer);
            if (found == demand.end()) {
                demand.emplace(producer, read);
            } else {
                found->second = regionUnion(found->second, read);
            }
        }
    }
    return plan;
}

// Collects the required dependency set of `output` (spec section 10.3:
// schedule only required dependencies), then orders it dependencies-first.
// Graph::connect rejects cycles, so a simple in-degree pass terminates.
// Shared by the CPU reference and the native GPU effect executor (issue
// #8): both consume the same scheduled plan.
std::vector<const NodeInstance*> scheduleDependencies(const Document& document, NetworkId networkId, NodeId output) {
    const auto& graph = document.network(networkId).graph();
    std::set<NodeId> required{output};
    std::vector<NodeId> stack{output};
    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();
        for (const auto& edge : graph.edgesInto(current)) {
            if (required.insert(edge.from.node).second) {
                stack.push_back(edge.from.node);
            }
        }
    }

    std::map<NodeId, std::size_t> pendingInputs;
    for (const NodeId id : required) {
        std::size_t count = 0;
        for (const auto& edge : graph.edgesInto(id)) {
            if (required.contains(edge.from.node)) {
                ++count;
            }
        }
        pendingInputs.emplace(id, count);
    }
    std::vector<const NodeInstance*> order;
    std::vector<NodeId> ready;
    for (const auto& [id, count] : pendingInputs) {
        if (count == 0) {
            ready.push_back(id);
        }
    }
    while (!ready.empty()) {
        const NodeId id = ready.back();
        order.push_back(findNode(document, networkId, id));
        for (const auto& edge : graph.edges()) {
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

NodeId resolveOutput(const Document& document, NetworkId networkId, const std::string& outputName) {
    const auto& network = document.network(networkId);
    const auto& graph = network.graph();
    if (!outputName.empty()) {
        const NodeInstance* named = graph.nodeByName(outputName);
        if (named == nullptr) {
            throw EvaluationException("no node named '" + outputName + "' in network '" + network.name() +
                                      "' of document '" + document.name + "'");
        }
        const auto* schema = graph.descriptor(named->type);
        if (schema == nullptr || !schema->isOutput) {
            throw EvaluationException(describeNode(*named) + ": --output must name an Output node");
        }
        return named->id;
    }

    const NodeId defaultOutput = network.defaultOutput();
    if (defaultOutput != kInvalidNode) {
        const NodeInstance* named = graph.node(defaultOutput);
        const auto* schema = named != nullptr ? graph.descriptor(named->type) : nullptr;
        if (named != nullptr && schema != nullptr && schema->isOutput)
            return named->id;
    }

    std::vector<const NodeInstance*> outputs;
    for (const auto& node : graph.nodes()) {
        const auto* schema = graph.descriptor(node.type);
        if (schema != nullptr && schema->isOutput)
            outputs.push_back(&node);
    }
    if (outputs.empty()) {
        throw EvaluationException("network '" + network.name() + "' in document '" + document.name +
                                  "' has no Output node: add an output node for evaluation to produce an image");
    }
    if (outputs.size() > 1) {
        std::ostringstream names;
        for (std::size_t i = 0; i < outputs.size(); ++i)
            names << (i == 0 ? "" : ", ") << "'" << outputs[i]->name << "' (id " << outputs[i]->id << ")";
        throw EvaluationException("network '" + network.name() + "' in document '" + document.name +
                                  "' has multiple Output nodes (" + std::move(names).str() +
                                  "); disambiguate with --output");
    }
    return outputs.front()->id;
}
// Shared request validation for both executors (CPU reference and native
// GPU, issues #8/#11): executor support (currently Full/RGBA), region
// bounds, output identity, and every declared capability of every scheduled
// dependency. Unsupported metadata is an explicit error, never a silent
// approximation or executor substitution.
void validateRequest(const Document& document, const EvaluationRequest& request) {
    if (request.network == kInvalidNetwork)
        throw EvaluationException("evaluation request must identify a network");
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
    const NodeInstance* output = findNode(document, request.network, request.output);
    if (output == nullptr) {
        throw EvaluationException("request output node " + std::to_string(request.output) +
                                  " does not exist in network " + std::to_string(request.network));
    }
    const auto& graph = document.network(request.network).graph();
    const auto* outputSchema = graph.descriptor(output->type);
    // Processors with declared image outputs are valid evaluation targets too:
    // the interactive viewer renders the attached upstream node directly.
    if (outputSchema == nullptr || (!outputSchema->isOutput && outputSchema->outputs.empty())) {
        failNode(*output, "evaluation request must target an Output node or a node type with declared outputs");
    }

    // A request is valid only when every expanded dependency advertises the
    // requested metadata. Nested network instances are routing aliases, not
    // executable node types; their definition's expanded nodes are checked.
    for (const ExpandedNode& expandedNode : expandDependencies(document, request.network, request.output)) {
        const NodeInstance& node = *expandedNode.node;
        if (node.definition != kInvalidNetwork)
            continue;
        const auto& scopedGraph = document.network(expandedNode.id.network).graph();
        const auto* schema = scopedGraph.descriptor(node.type);
        if (schema == nullptr) {
            // Unknown persisted types remain recoverable. Preserve the
            // existing explicit scale failure for such a type, while its
            // executor-specific failure handles a full-resolution request.
            if (request.samplingScale != 1)
                failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                                   " cannot be validated because the node type has no descriptor");
            continue;
        }
        const auto& capabilities = schema->capabilities;
        if (std::find(capabilities.qualityModes.begin(), capabilities.qualityModes.end(), request.quality) ==
            capabilities.qualityModes.end()) {
            failNode(node, std::string("quality '") + qualityName(request.quality) +
                               "' is not declared by node type '" + node.type + "'");
        }
        if (std::find(capabilities.channels.begin(), capabilities.channels.end(), request.channels) ==
            capabilities.channels.end()) {
            failNode(node, "channels '" + request.channels + "' are not declared by node type '" + node.type + "'");
        }
        if (std::find(capabilities.samplingScales.begin(), capabilities.samplingScales.end(), request.samplingScale) ==
            capabilities.samplingScales.end()) {
            failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                               " is not declared by node type '" + node.type + "'");
        }
        // Declared region support is a planner fact, not a rejection
        // (issue #85): a contribution that declares supportsRegion=false is
        // processed over the whole image domain internally and its consumers
        // still receive exactly the region they asked for.
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

std::vector<NodeId> resolveStepInputs(const Document& document, NetworkId network, const NodeInstance& node,
                                      const std::map<NodeId, ImageIdentity>& evaluated, PlanStep& step) {
    const auto& graph = document.network(network).graph();
    const auto& inPorts = graph.inputPortsFor(node.type);
    std::vector<NodeId> producers;
    for (std::uint32_t port = 0; port < inPorts.size(); ++port) {
        const Edge* edge = nullptr;
        for (const auto& candidate : graph.edgesInto(node.id)) {
            if (candidate.to.port == port) {
                edge = &candidate;
                break;
            }
        }
        if (edge == nullptr || !evaluated.contains(edge->from.node)) {
            if (inPorts[port].optional) {
                // Absent optional slot: preserve declared-port alignment with
                // the invalid sentinel instead of a manufactured producer.
                step.inputs.push_back(kInvalidNode);
                step.inputImages.push_back(ImageIdentity{});
                producers.push_back(kInvalidNode);
                continue;
            }
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
                          SourceProvider* sources, std::shared_ptr<const NodeContributions> contributions) {
    validateRequest(document, request);
    if (!contributions)
        throw std::invalid_argument("evaluateCpu requires a node registration snapshot");

    // The caller's coverage, normalized to the image-space sampling lattice, is
    // what this call delivers. Per-node coverage is planned from it and may be
    // larger (padding, halos, whole-domain escalation); every consumer reads an
    // input through the coverage its producer really has.
    const EvaluationRequest normalized = canonicalizeRequest(request);
    const RegionPlan regionPlan = planDependencyRegions(document, normalized, *contributions);
    // Publication freshness (issue #9): capture revision + generation at
    // request start; computed results publish only while both hold.
    const EvaluationTicket ticket = reuse != nullptr ? reuse->beginTicket(document) : EvaluationTicket{};

    std::map<EvaluationNodeId, std::shared_ptr<const CpuImage>> images;
    std::map<EvaluationNodeId, ImageIdentity> identities;
    std::map<EvaluationNodeId, ResultKey> contentKeyByIdentity;
    // Actual coverage per expanded node: the planned request, a whole-domain
    // escalation reported by the planner, or the resident rectangle a cache hit
    // served. Every consumer reads an input through this geometry, so a raster
    // is never assumed to start at the consumer's own origin.
    std::map<EvaluationNodeId, EvaluationRequest> coverage;
    EvaluationPlan plan;
    plan.request = normalized;

    for (const ExpandedNode& expandedNode : regionPlan.order) {
        const NodeInstance& node = *expandedNode.node;
        PlanStep step;
        step.network = expandedNode.id.network;
        step.instance = expandedNode.id.instance;
        step.node = node.id;
        step.outputPort = expandedNode.id.outputPort;
        step.path = expandedNode.id.path;
        step.type = node.type;
        step.name = node.name;
        std::optional<NodeInstance> resolvedNode;
        const NodeInstance* effectiveNode =
            resolveEffectiveNode(document, expandedNode, resolvedNode, static_cast<double>(normalized.localTime));
        step.effectiveParams = effectiveNode->params;
        const EvaluationRequest nodeRequest = regionPlan.requests.at(expandedNode.id);
        const NodeCatalog& scopedCatalog = document.network(expandedNode.id.network).graph().catalog();

        // Registration compatibility precedes every cache, alias and
        // implementation decision: a node this registration does not cover, or
        // one whose declared schema the registered implementation no longer
        // matches, is reported before a cached result could be reused for it.
        const NodeContribution* contribution = nullptr;
        if (!expandedNode.alias) {
            contribution = contributions->find(effectiveNode->type);
            if (contribution == nullptr) {
                if (scopedCatalog.find(effectiveNode->type) != nullptr) {
                    failNode(*effectiveNode,
                             "declared node type has no CPU reference implementation (executor unavailable)");
                }
                failNode(*effectiveNode, "unknown node type has no CPU reference implementation");
            }
            contributions->validate(scopedCatalog, *effectiveNode);
            const bool producesPixels = contribution->role == NodeRole::Image || contribution->role == NodeRole::Source;
            if (producesPixels && !contribution->cpu) {
                // A GPU-only (or otherwise unavailable) contribution is honest
                // about it: report the node and its reason instead of inventing
                // a fallback image.
                failNode(*effectiveNode, contribution->cpuUnavailableReason.empty()
                                             ? "no CPU reference implementation is registered for this node type"
                                             : contribution->cpuUnavailableReason);
            }
        }

        std::vector<std::uint64_t> inputContentHashes;
        inputContentHashes.reserve(expandedNode.inputs.size());
        for (const EvaluationNodeId& producer : expandedNode.inputs) {
            if (producer.node == kInvalidNode) {
                // Absent optional input: the slot keeps its declared position
                // with a fixed sentinel token, so a missing mask and a
                // connected mask (even with maskChannel none) never share a
                // key while real producer hashes stay in port order.
                inputContentHashes.push_back(kAbsentInputKeyHash);
                step.inputs.push_back(kInvalidNode);
                step.inputImages.push_back(ImageIdentity{});
                step.scopedInputs.push_back(ScopedPlanInput{});
                continue;
            }
            const auto content = contentKeyByIdentity.find(producer);
            if (content == contentKeyByIdentity.end())
                throw EvaluationException("expanded evaluation plan has an unresolved input dependency");
            inputContentHashes.push_back(content->second.hash);
            step.inputs.push_back(producer.node);
            step.inputImages.push_back(identities.at(producer));
            step.scopedInputs.push_back(ScopedPlanInput{producer.network, producer.instance, producer.node,
                                                        producer.outputPort, producer.path});
        }
        // Hash exactly the parameter map execution consumes. In particular,
        // animated values must affect this node's identity and all dependent
        // identities, while equal effective values remain reusable across
        // unrelated history revisions. A source node's key additionally carries
        // the content identity of the color configuration the provider resolves
        // media color against, so a changed configuration can never serve a
        // result produced under the previous one (issue #75).
        KeyContext keyContext;
        if (sources != nullptr && contribution != nullptr && contribution->role == NodeRole::Source)
            keyContext.colorConfigIdentity = sources->colorConfigIdentity();
        // Content identity is deliberately independent of coverage, and inputs
        // contribute their CONTENT hashes: which rectangle currently backs an
        // upstream image must never change what this node's pixels mean
        // (issue #85), or a pan would invalidate the whole graph.
        const ResultKey contentKey =
            nodeContentKey(document, *effectiveNode, inputContentHashes, nodeRequest, keyContext);
        contentKeyByIdentity.emplace(expandedNode.id, contentKey);

        // The registration is retained by `contributions` for the whole call,
        // so an in-flight evaluation never observes a replaced snapshot.
        std::shared_ptr<const CpuImage> image;
        EvaluationRequest actualRequest = nodeRequest;
        if (reuse != nullptr) {
            if (const std::optional<ResultCache<CpuImage>::Entry> hit = reuse->findRegion(contentKey, nodeRequest)) {
                image = hit->image;
                step.produced = hit->identity;
                step.cacheReused = true;
                // A resident rectangle that covers the demand is consumed as it
                // is; the planner's padding makes an overlapping pan land on
                // the same coverage.
                actualRequest = hit->request;
            }
        }
        if (!image && expandedNode.alias) {
            image = images.at(*expandedNode.alias);
            step.produced = identities.at(*expandedNode.alias);
            actualRequest = coverage.at(*expandedNode.alias);
        }

        if (!image) {
            std::vector<const CpuImage*> inputs;
            std::vector<EvaluationRequest> inputRequests;
            inputs.reserve(expandedNode.inputs.size());
            inputRequests.reserve(expandedNode.inputs.size());
            for (const EvaluationNodeId& producer : expandedNode.inputs) {
                inputs.push_back(producer.node == kInvalidNode ? nullptr : images.at(producer).get());
                inputRequests.push_back(producer.node == kInvalidNode ? EvaluationRequest{} : coverage.at(producer));
            }
            const std::span<const CpuImage* const> contextInputs(inputs.data(), inputs.size());
            const std::span<const EvaluationRequest> contextRequests(inputRequests.data(), inputRequests.size());
            const CpuNodeContext context{document,    scopedCatalog,        *effectiveNode,
                                         nodeRequest, step.effectiveParams, contextInputs,
                                         sources,     contextRequests};

            if (contribution->role == NodeRole::Output) {
                if (inputs.empty() || inputs[0] == nullptr)
                    failNode(*effectiveNode, "output requires a connected color input");
                const CpuImage& source = *inputs[0];
                const ImageLayout layout = effectRasterLayout(nodeRequest, &source);
                const InputAnchor anchor = anchorInput(context, 0, source);
                const EvaluationRequest& sourceRequest = inputRequests[0];
                if (sourceRequest.region == nodeRequest.region &&
                    sourceRequest.samplingScale == nodeRequest.samplingScale && source.layout() == layout) {
                    // Identical raster and origin: delivery really is the input.
                    image = images.at(expandedNode.inputs[0]);
                    step.produced = identities.at(expandedNode.inputs[0]);
                } else {
                    // Preserve the existing Output interpretation contract;
                    // do not relabel a shared upstream/cache image in place.
                    // The input may cover more (a whole-domain escalation or a
                    // wider resident rectangle), so the delivered raster is the
                    // requested window at its real origin.
                    auto fresh = std::make_shared<CpuImage>(windowOf(source, anchor, layout));
                    step.produced = identityOf(*fresh, Residency::HostCpuReference);
                    image = std::move(fresh);
                }
            } else if (contribution->role == NodeRole::Viewer) {
                // Unreachable through a valid request (the viewer is not a
                // network output), but never silently rendered: a Viewer has no
                // pixel implementation.
                failNode(*effectiveNode, contribution->cpuUnavailableReason.empty()
                                             ? "the Viewer role has no CPU pixel implementation"
                                             : contribution->cpuUnavailableReason);
            } else {
                auto fresh = std::make_shared<CpuImage>(contribution->cpu->execute(context));
                step.produced = identityOf(*fresh, Residency::HostCpuReference);
                image = std::move(fresh);
            }
            if (reuse != nullptr)
                reuse->publishRegion(document, ticket, contentKey, actualRequest, image, step.produced);
        }

        coverage[expandedNode.id] = actualRequest;
        step.region = actualRequest.region;
        identities.emplace(expandedNode.id, step.produced);
        images.emplace(expandedNode.id, std::move(image));
        plan.steps.push_back(std::move(step));
    }

    const EvaluationNodeId outputKey{request.network, kInvalidNetworkInstance, request.output, kEvaluationWholeNode};
    const EvaluationRequest& delivered = coverage.at(outputKey);
    CpuEvaluation evaluation;
    evaluation.plan = std::move(plan);
    if (delivered.region == normalized.region && delivered.samplingScale == normalized.samplingScale) {
        evaluation.image = *images.at(outputKey);
        evaluation.plan.result = identities.at(outputKey);
    } else {
        // The delivered raster is exactly the normalized request, even when the
        // executor computed (or reused) more for the sake of reuse.
        evaluation.image = windowOf(*images.at(outputKey), requiredAnchor(*images.at(outputKey), delivered, normalized),
                                    effectRasterLayout(normalized, images.at(outputKey).get()));
        evaluation.plan.result = identityOf(evaluation.image, Residency::HostCpuReference);
    }
    return evaluation;
}

}  // namespace nemo
