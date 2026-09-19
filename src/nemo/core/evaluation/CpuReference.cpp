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
    // Channel count, then each name length-prefixed: named channels (issue #90)
    // must not alias a different set by mere concatenation.
    const auto channelCount = static_cast<std::uint64_t>(layout.channels.size());
    hashBytes(hash, &channelCount, sizeof(channelCount));
    for (const auto& channel : layout.channels) {
        const auto size = static_cast<std::uint64_t>(channel.size());
        hashBytes(hash, &size, sizeof(size));
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
    hashBytes(hash, image.data(), pixelCount * image.channelCount() * sizeof(float));
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

// Coverage a node must actually produce for one demand (issue #88): the demand
// rounded outward to whole coverage-padding blocks, with that padding limited to
// the node's useful domain, and the demanded rectangle itself always retained.
//
// Padding keeps overlapping pans inside coverage that is already resident, so
// limiting it to the domain avoids manufacturing black pixels outside the image
// that nobody can pan onto. The demand is not padding: a demand outside the
// format is exactly the transparent black the caller asked for, and neither
// discarding it nor escalating it to the whole domain would answer the request.
[[nodiscard]] Region plannedCoverage(const Region& demand, int scale, const Region& usefulDomain) {
    const int block = kCoveragePaddingRasterPixels > 0 ? kCoveragePaddingRasterPixels * scale : scale;
    const Region demanded = regionOnLattice(demand, scale);
    const Region padded = regionOnLattice(demand, block);
    const Region domain = regionOnLattice(usefulDomain, scale);
    return regionUnion(regionIntersection(padded, domain), demanded);
}

// The domain a described image can produce: its format unioned with its data
// window. An empty data window (a described image whose every pixel is
// transparent) therefore produces exactly its format.
[[nodiscard]] Region describedDomain(const ImageDescription& description) {
    return regionUnion(description.format, description.dataBounds);
}

// The canvas description a generator falls back to (issue #96): the owning
// network's authored format, fully covered by data.
[[nodiscard]] ImageDescription canvasDescription(const ImageFormat& format) {
    const Region region{0, 0, format.width, format.height};
    ImageDescription description;
    description.format = region;
    description.dataBounds = region;
    description.pixelAspect = format.pixelAspect;
    return description;
}

// Admissibility of one described image. A described image is always
// geometrically complete: it states a positive format, a finite positive pixel
// aspect and named stored channels. Unknown geometry has no representation
// here — the seam that could not obtain it fails with the offending node and
// source instead — so a consumer never has to interpret a zero as "unknown".
[[nodiscard]] bool representableImageRegion(const Region& bounds) {
    const std::int64_t right = static_cast<std::int64_t>(bounds.x) + bounds.width;
    const std::int64_t bottom = static_cast<std::int64_t>(bounds.y) + bounds.height;
    return bounds.width >= 0 && bounds.height >= 0 && bounds.x >= -kMaxDescribedCoordinate &&
           bounds.y >= -kMaxDescribedCoordinate && bounds.x <= kMaxDescribedCoordinate &&
           bounds.y <= kMaxDescribedCoordinate && right <= kMaxDescribedCoordinate && bottom <= kMaxDescribedCoordinate;
}

void validateDescription(const NodeInstance& node, const ImageDescription& description) {
    if (hasNoImageFormat(description)) {
        failNode(node, "node description has no image format: every produced image states its format "
                       "(a source that cannot be inspected reports that failure instead of an empty one)");
    }
    if (!std::isfinite(description.pixelAspect) || description.pixelAspect <= 0.0F) {
        failNode(node,
                 "node description reports an invalid pixel aspect (" + std::to_string(description.pixelAspect) + ")");
    }
    if (description.format.x != 0 || description.format.y != 0) {
        failNode(node, "node description format must be normalized to origin (0,0); data bounds may be signed");
    }
    const auto validateBounds = [&node](const Region& bounds, const char* field) {
        if (!representableImageRegion(bounds))
            failNode(node, std::string("node description has unrepresentable ") + field);
    };
    validateBounds(description.format, "format");
    validateBounds(description.dataBounds, "data bounds");
    if (description.channels.empty()) {
        failNode(node, "node description declares no image channels");
    }
    std::set<std::string> names;
    for (const std::string& channel : description.channels) {
        if (channel.empty()) {
            failNode(node, "node description declares an unnamed image channel");
        }
        // A stored image names each channel once: a duplicate would make the
        // named reads, the demand and the storage order ambiguous.
        if (!names.insert(channel).second) {
            failNode(node, "node description declares channel '" + channel + "' more than once");
        }
    }
}

// Channel-vocabulary coverage (issue #90): a declared capability admits a
// demanded channel name when the declaration carries every named channel
// (an empty list or the "*" wildcard), names it exactly, or — for a legacy
// concatenated single-character set such as "RGBA" — contains that channel as
// one of its letters. Anything else is genuinely undeclared: an auxiliary
// channel an image carries is never demanded from a node whose vocabulary
// excludes it, so this stays a real declaration check rather than a passing
// formality.
[[nodiscard]] bool capabilitiesCoverChannels(const std::vector<std::string>& declared,
                                             const std::vector<std::string>& demand) {
    if (declared.empty()) {
        return true;
    }
    const auto entryCovers = [](const std::string& entry, const std::string& name) {
        if (std::string_view{entry} == kAnyChannelCapability) {
            return true;
        }
        if (entry == name) {
            return true;
        }
        if (name.size() != 1) {
            return false;
        }
        return std::any_of(entry.begin(), entry.end(), [&name](char value) {
            return channelsDetail::lowerAscii(value) == channelsDetail::lowerAscii(name.front());
        });
    };
    return std::all_of(demand.begin(), demand.end(), [&declared, &entryCovers](const std::string& name) {
        return std::any_of(declared.begin(), declared.end(),
                           [&entryCovers, &name](const std::string& entry) { return entryCovers(entry, name); });
    });
}

// Union of two channel demands, preserving the order each name was first
// demanded in. Names are never reordered or collapsed: an auxiliary demand
// keeps its identity through the whole plan (issue #90).
[[nodiscard]] std::vector<std::string> unionChannels(const std::vector<std::string>& left,
                                                     const std::vector<std::string>& right) {
    std::vector<std::string> result = left;
    for (const std::string& name : right) {
        if (std::find(result.begin(), result.end(), name) == result.end()) {
            result.push_back(name);
        }
    }
    return result;
}

// Channel names rendered for a diagnostic message, in the order they were
// demanded. Names are quoted so an empty or whitespace name is visible.
[[nodiscard]] std::string channelListText(const std::vector<std::string>& channels) {
    std::string text;
    for (const std::string& name : channels) {
        if (!text.empty()) {
            text += ", ";
        }
        text += "'" + name + "'";
    }
    return text;
}

// The demand placed on a producer's inherited (non-explicit) channel demand:
// the names the producer's described image actually carries. An inherited
// demand says "whatever this node needs"; a name the producer does not hold is
// served as zero by the frozen zero-fill policy and must not be demanded as if
// it existed. An EXPLICIT contribution requirement is not filtered — that is a
// declaration and an unsupported name is a real error, reported against the
// producer that the check runs on.
[[nodiscard]] std::vector<std::string> inheritedChannels(const std::vector<std::string>& demanded,
                                                         const ImageDescription& producer) {
    std::vector<std::string> result;
    result.reserve(demanded.size());
    for (const std::string& name : demanded) {
        if (hasChannel(producer.channels, name)) {
            result.push_back(name);
        }
    }
    return result;
}

// Describes one source node through the media description seam (issue #88). The
// resolver's own policy decision is raised first — it is a metadata failure and
// belongs before any provider call — and a provider failure is wrapped with the
// offending node, source key, path and frame, exactly like the frame seam, so
// both seams name the same relationship. A source whose media cannot be
// inspected fails HERE: there is no empty or sentinel description, and the
// executor never invents geometry for it.
[[nodiscard]] ImageDescription describeSource(const Document& document, const NodeInstance& node,
                                              const EffectiveSourceRequest& source,
                                              SourceDescriptionProvider* sources) {
    if (source.policyError) {
        failNode(node, sourcePolicyProblem(source));
    }
    const std::string subject = "source '" + source.sourceKey + "' (" + source.path + ")";
    if (sources == nullptr) {
        failNode(node,
                 subject + " requires a source description provider; this executor never invents source geometry");
    }
    try {
        return sources->describe(document, source);
    } catch (const EvaluationException& error) {
        if (error.hasNode()) {
            throw;
        }
        failNode(node, subject + " could not be described at frame " + std::to_string(source.readFrame) + ": " +
                           error.what());
    } catch (const std::exception& error) {
        failNode(node, subject + " could not be described at frame " + std::to_string(source.readFrame) + ": " +
                           error.what());
    }
}

// The described data window is the support of a produced image (issue #88):
// every sample outside it is transparent black, whichever node produced the
// raster — a generator cannot fabricate samples beyond its canvas, a grade with
// an offset cannot leak nonzero samples outside the described window, and an
// empty data window is a fully transparent image rather than anything else.
//
// One central guard, applied where a node's own implementation returns pixels,
// keeps that policy out of every effect's pixel math and out of every
// executor's kernels; a contribution states what its image IS (its description)
// and the executor makes the raster agree with it.
//
// An explicitly EXTENDED description (issue #92) is the one exception, and it
// is a claim about the producer, not about a consumer: the finite data bounds
// are the retained edge domain and the effect itself answered the coordinates
// outside them, so clearing would destroy data the effect just produced. An
// empty data window stays fully transparent whatever the flag says
// (`hasEdgeExtension`), because there is no edge to extend.
//
// Geometry: the raster covers `request.region` at `request.samplingScale`, so
// raster sample (x, y) is anchored at the full-resolution coordinate
// (region.x + x*scale, region.y + y*scale) — the same sampling-lattice
// convention every executor and shader uses. A sample whose anchor lies inside
// `dataBounds` is kept even when it lies outside the format (overscan IS data),
// and one whose anchor lies outside is cleared. The guard never changes the
// raster's extent, region or scale, so anchors, coverage and identities of the
// delivered geometry are untouched; it runs before the produced image's
// identity is computed, so a cached result is always the guarded one.
void enforceDataWindow(CpuImage& image, const EvaluationRequest& request, const ImageDescription& description) {
    if (hasEdgeExtension(description)) {
        return;
    }
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    const Region& bounds = description.dataBounds;
    // Raster columns whose anchor is inside the data window, as a half-open
    // index range: the lattice helpers keep signed origins and non-multiple
    // extents exact at every sampling scale.
    const int firstX = std::max(0, latticeCeil(bounds.x - request.region.x, scale) / scale);
    const int firstY = std::max(0, latticeCeil(bounds.y - request.region.y, scale) / scale);
    const int lastX = std::min(image.width(), latticeCeil(bounds.x + bounds.width - request.region.x, scale) / scale);
    const int lastY = std::min(image.height(), latticeCeil(bounds.y + bounds.height - request.region.y, scale) / scale);
    if (firstX <= 0 && firstY <= 0 && lastX >= image.width() && lastY >= image.height()) {
        return;  // the whole raster is inside the data window; nothing to clear
    }
    // Clear only the four bands outside the window: the common case (a raster
    // inside its own data window) costs four comparisons, and a padded raster
    // pays for the padding it actually has rather than for a full scan. The
    // clear covers EVERY stored channel (issue #90): an auxiliary or data
    // channel outside the window is exactly as transparent as RGB(A).
    const auto clear = [&image](int yBegin, int yEnd, int xBegin, int xEnd) {
        for (int y = std::max(0, yBegin); y < std::min(image.height(), yEnd); ++y) {
            for (int x = std::max(0, xBegin); x < std::min(image.width(), xEnd); ++x) {
                image.clearPixel(x, y);
            }
        }
    };
    clear(0, firstY, 0, image.width());
    clear(lastY, image.height(), 0, image.width());
    clear(firstY, lastY, 0, firstX);
    clear(firstY, lastY, lastX, image.width());
}

}  // namespace

ImageDescriptionPlan describeDependencies(const Document& document, const EvaluationRequest& request,
                                          const NodeContributions& contributions, SourceDescriptionProvider* sources) {
    ImageDescriptionPlan plan;
    // Origin identity (issue #98): what this plan's descriptions are resolved
    // from, so a caller that hands the plan back to planResolvedRegions can be
    // verified instead of trusted.
    plan.document = &document;
    plan.documentRevision = document.stateRevision();
    plan.contributions = &contributions;
    plan.query = request;
    plan.order = expandDependencies(document, request.network, request.output);
    if (plan.order.empty()) {
        throw EvaluationException("evaluation plan has no scheduled nodes");
    }

    for (const ExpandedNode& expanded : plan.order) {
        ResolvedImageNode resolved;
        std::optional<NodeInstance> localNode;
        const NodeInstance* effective =
            resolveEffectiveNode(document, expanded, localNode, static_cast<double>(request.localTime));
        resolved.node = localNode ? std::move(*localNode) : *effective;

        // The declared inputs' descriptions, in declared-port order with a null
        // for an absent optional slot: what a node's own description rule and
        // the planner both read, in one shape.
        std::vector<const ImageDescription*> inputDescriptions;
        inputDescriptions.reserve(expanded.inputs.size());
        for (const EvaluationNodeId& producer : expanded.inputs) {
            inputDescriptions.push_back(producer.node == kInvalidNode ? nullptr : &plan.nodes.at(producer).description);
        }
        const std::span<const ImageDescription* const> inputs(inputDescriptions.data(), inputDescriptions.size());

        if (expanded.alias) {
            // A nested network instance is routing: it produces the image its
            // selected internal producer produces, descriptions included.
            const ResolvedImageNode& target = plan.nodes.at(*expanded.alias);
            resolved.description = target.description;
            resolved.source = target.source;
            plan.nodes.emplace(expanded.id, std::move(resolved));
            continue;
        }

        const NodeCatalog& catalog = document.network(expanded.id.network).graph().catalog();
        const NodeContribution* contribution = contributions.find(resolved.node.type);
        if (contribution == nullptr) {
            failNode(resolved.node, catalog.find(resolved.node.type) != nullptr
                                        ? "declared node type has no registered implementation (executor unavailable)"
                                        : "unknown node type has no registered implementation");
        }
        contributions.validate(catalog, resolved.node);
        for (const ParameterSpec& parameter : contribution->descriptor.parameters) {
            resolved.node.params.try_emplace(parameter.name, parameter.defaultValue);
        }
        // The node's typed parameter interpretation is metadata too: it is
        // validated here, from the resolved state the executor will consume, so
        // an inadmissible authored value is reported before any node's pixels
        // are produced. The message is the contribution's own (the node-local
        // helper the executor also reads through), with the node identified.
        if (const std::optional<std::string> problem =
                contributions.validateParameters(catalog, resolved.node, resolved.node.params)) {
            throw EvaluationException(*problem, resolved.node.id, resolved.node.name);
        }

        // The shared default an ordinary node keeps without a rule of its own.
        // The owning network's authored canvas (issue #96) is also the frame a
        // node states its own authored values in (issue #92), so the same
        // resolved reference travels to the rule.
        const ImageFormat& owningFormat = document.network(expanded.id.network).format();
        const auto mainPort = contribution->descriptor.mainInput;
        const ImageDescription* mainInput = mainPort < inputDescriptions.size() ? inputDescriptions[mainPort] : nullptr;
        const ImageDescription inherited = mainInput != nullptr ? *mainInput : canvasDescription(owningFormat);

        if (contribution->role == NodeRole::Source) {
            const EffectiveSourceRequest source = resolveSourceRequest(document, resolved.node, request.localTime);
            resolved.source = source;
            resolved.description = describeSource(document, resolved.node, source, sources);
        } else if (contribution->describe) {
            resolved.description =
                contribution->describe(NodeDescriptionContext{document, catalog, resolved.node, request.localTime,
                                                              inputs, inherited, &owningFormat, expanded.id.network});
        } else {
            resolved.description = inherited;
        }
        validateDescription(resolved.node, resolved.description);
        plan.nodes.emplace(expanded.id, std::move(resolved));
    }
    return plan;
}

RegionPlan planResolvedRegions(const Document& document, const EvaluationRequest& request,
                               const NodeContributions& contributions, ImageDescriptionPlan described) {
    // A supplied description plan is trusted only when it provably describes
    // THIS call's target and time: the same document snapshot, the same
    // registration, and the network/output/local time it identified. A
    // mismatch is refused, never silently re-planned — a caller that hands over
    // a description and a demand that disagree has a bug, and resolving a
    // second authored state would hide it while making the caller's key and the
    // executed pixels describe different things.
    const auto refuse = [](const std::string& detail) {
        throw EvaluationException("supplied image description plan does not describe this request: " + detail);
    };
    if (described.document != &document)
        refuse("it was described from a different document object");
    if (described.documentRevision != document.stateRevision())
        refuse("the document changed since it was described (revision " + std::to_string(described.documentRevision) +
               " vs " + std::to_string(document.stateRevision()) + ")");
    if (described.contributions != &contributions)
        refuse("it was described with a different node registration");
    if (described.query.network != request.network || described.query.output != request.output ||
        described.query.localTime != request.localTime)
        refuse("it describes another target or local time");

    RegionPlan plan;
    plan.images = std::move(described);

    // The described target defines the request's logical format, so the caller's
    // demand is re-based on it: a caller asks for a region and the graph says
    // what image that region belongs to. The described format is authoritative,
    // so a caller need not state one at all.
    const ImageDescription& target = plan.images.nodes.at(plan.images.order.back().id).description;
    const int scale = isSamplingScale(request.samplingScale) ? request.samplingScale : 1;
    EvaluationRequest demanded = request;
    demanded.fullWidth = target.format.width;
    demanded.fullHeight = target.format.height;
    plan.request = canonicalizeRequest(demanded);
    validateRequestDomain(plan.request);
    // Origin identity: the immutable snapshot, the registration and the exact
    // canonical demand this plan resolves. A consumer handed this plan (the
    // viewer shares one between its cache key and its execution) compares these
    // instead of re-deriving them.
    plan.document = &document;
    plan.documentRevision = document.stateRevision();
    plan.contributions = &contributions;
    plan.demand = canonicalizeRequest(request);

    // Coverage travels backwards from the requested output, so a node is only
    // visited after every consumer has contributed its demand (the order is
    // dependency-first, hence its reverse is consumer-first).
    std::map<EvaluationNodeId, Region> producerDemand;
    std::map<EvaluationNodeId, std::vector<std::string>> channelDemand;
    const EvaluationNodeId& targetId = plan.images.order.back().id;
    producerDemand.emplace(targetId, plan.request.region);
    // An empty request demand means every channel the requested image names
    // (issue #90), so the target's own described channels become the explicit
    // demand every key and per-node request carries. An explicit request names
    // channels exactly and is validated against the target's description below.
    std::vector<std::string> targetChannels = plan.request.channels;
    if (targetChannels.empty()) {
        targetChannels = target.channels;
        plan.request.channels = targetChannels;
    }
    channelDemand.emplace(targetId, std::move(targetChannels));

    for (auto entry = plan.images.order.rbegin(); entry != plan.images.order.rend(); ++entry) {
        const ExpandedNode& expanded = *entry;
        const ResolvedImageNode& resolved = plan.images.nodes.at(expanded.id);
        const NodeInstance& node = resolved.node;
        const ImageDescription& description = resolved.description;
        const NodeContribution* contribution = expanded.alias ? nullptr : contributions.find(node.type);
        // A contribution that declares no regional support processes whole
        // images internally: its own coverage and every input's coverage
        // escalate to its whole useful domain, and consumers read the sub-region
        // they asked for from it. An explicitly EXTENDED producer (issue #92)
        // has no finite whole domain to escalate to — its retained bounds are
        // only the edge it extends from — so its coverage is the retained
        // domain UNIONED with the demand, which keeps the demanded coverage
        // (the sample-evaluation discipline of a whole-frame node stays intact)
        // without clipping the request the consumer actually made.
        const bool wholeFrameOnly = contribution != nullptr && !contribution->descriptor.capabilities.supportsRegion;
        const Region domain = regionOnLattice(describedDomain(description), scale);
        const Region coverage =
            wholeFrameOnly
                ? (hasEdgeExtension(description) ? regionUnion(domain, producerDemand.at(expanded.id)) : domain)
                : plannedCoverage(producerDemand.at(expanded.id), scale, domain);

        // Every per-node request states this node's OWN format as its domain:
        // its coverage is in absolute image coordinates, so a consumer's own
        // canvas must never be imposed on it.
        EvaluationRequest nodeRequest = plan.request;
        nodeRequest.network = expanded.id.network;
        nodeRequest.region = coverage;
        nodeRequest.fullWidth = description.format.width;
        nodeRequest.fullHeight = description.format.height;
        nodeRequest.channels = channelDemand.at(expanded.id);
        validateRequestDomain(nodeRequest);
        if (contribution != nullptr) {
            // The demand this node received must be a name set its described
            // image really carries. An inherited demand is already filtered to
            // the producer's channels, so this fires exactly where a
            // declaration was dishonest: an explicit requirement naming a
            // channel its producer cannot carry, or an explicit request for a
            // channel the target's described image does not hold. Both are real
            // errors, reported against this node before any pixel work; a
            // channel a *source* merely lacks is not one of them — the frozen
            // zero-fill policy serves that as zero.
            const bool carried =
                std::all_of(nodeRequest.channels.begin(), nodeRequest.channels.end(),
                            [&description](const std::string& name) { return hasChannel(description.channels, name); });
            if (!carried) {
                failNode(node, "the requested channels '" + channelListText(nodeRequest.channels) +
                                   "' are not part of this node's described image channels");
            }
            if (!capabilitiesCoverChannels(contribution->descriptor.capabilities.channels, nodeRequest.channels)) {
                failNode(node, "the requested channels '" + channelListText(nodeRequest.channels) +
                                   "' are not declared by node type '" + node.type + "'");
            }
        }
        plan.requests[expanded.id] = nodeRequest;

        // The node's own requirement rule states what it reads per declared
        // input port. Ports it does not cover are read at the node's own
        // coverage; a node with no rule reads every input at its own coverage
        // (the pointwise default).
        std::vector<const ImageDescription*> inputDescriptions;
        inputDescriptions.reserve(expanded.inputs.size());
        for (const EvaluationNodeId& producer : expanded.inputs) {
            inputDescriptions.push_back(producer.node == kInvalidNode ? nullptr
                                                                      : &plan.images.nodes.at(producer).description);
        }
        std::vector<InputRequirement> requirements;
        if (!wholeFrameOnly && contribution != nullptr && contribution->inputRequirements) {
            const NodeCatalog& catalog = document.network(expanded.id.network).graph().catalog();
            const ParameterValues& effectiveParams = node.params;
            const std::span<const ImageDescription* const> inputs(inputDescriptions.data(), inputDescriptions.size());
            const ImageFormat& owningFormat = document.network(expanded.id.network).format();
            const NodeRegionContext regionContext{
                catalog,     node,   nodeRequest,  effectiveParams, description.pixelAspect,
                description, inputs, &owningFormat};
            requirements = contribution->inputRequirements(regionContext);
        }
        if (requirements.size() > expanded.inputs.size()) {
            failNode(node, "input requirements declare a port beyond this node's input contract");
        }
        for (std::size_t port = 0; port < requirements.size(); ++port) {
            if (!representableImageRegion(requirements[port].region)) {
                failNode(node, "input requirement for port " + std::to_string(port) + " has unrepresentable bounds");
            }
        }
        for (std::size_t port = 0; port < expanded.inputs.size(); ++port) {
            const EvaluationNodeId& producer = expanded.inputs[port];
            if (producer.node == kInvalidNode)
                continue;  // absent optional slot: no source, no demand
            // A port the rule does not cover (or covers with an empty region)
            // inherits this node's own coverage. Its channels inherit too, but
            // filtered to what the producer really carries: that default is a
            // hint, not a declaration, and the zero-fill policy serves the rest.
            // An explicit requirement's channels are the contribution's own
            // declaration and travel unfiltered, so a name the producer cannot
            // carry fails there instead of silently becoming nothing.
            const ImageDescription& producerDescription = plan.images.nodes.at(producer).description;
            Region read = coverage;
            const bool explicitChannels = port < requirements.size() && !requirements[port].channels.empty();
            std::vector<std::string> channels = explicitChannels
                                                    ? requirements[port].channels
                                                    : inheritedChannels(nodeRequest.channels, producerDescription);
            if (port < requirements.size()) {
                const InputRequirement& requirement = requirements[port];
                if (requirement.region.width > 0 && requirement.region.height > 0) {
                    read = requirement.region;
                }
            }
            const Region projected = plannedCoverage(read, scale, describedDomain(producerDescription));
            const auto found = producerDemand.find(producer);
            if (found == producerDemand.end()) {
                producerDemand.emplace(producer, projected);
            } else {
                found->second = regionUnion(found->second, projected);
            }
            const auto demandedChannels = channelDemand.find(producer);
            if (demandedChannels == channelDemand.end()) {
                channelDemand.emplace(producer, std::move(channels));
            } else {
                demandedChannels->second = unionChannels(demandedChannels->second, channels);
            }
        }
    }
    return plan;
}

RegionPlan planDependencyRegions(const Document& document, const EvaluationRequest& request,
                                 const NodeContributions& contributions, SourceDescriptionProvider* sources) {
    return planResolvedRegions(document, request, contributions,
                               describeDependencies(document, request, contributions, sources));
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
void validateRequestDomain(const EvaluationRequest& request) {
    // Executor limits are not persistent-format limits. Presentation can check
    // these before pixel/ROI arithmetic without walking the dependency graph.
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
        throw EvaluationException("full image domain exceeds the " + std::to_string(kMaxDimension) +
                                  " pixel reference limit");
    if (request.fullWidth < 0 || request.fullHeight < 0)
        throw EvaluationException("image domain must be nonnegative");
    if ((request.fullWidth == 0) != (request.fullHeight == 0))
        throw EvaluationException("an explicit image domain requires both fullWidth and fullHeight");
    // The region is signed and may extend outside the domain, and a caller need
    // not state the domain at all (issue #88): planning re-bases the demand on
    // the described target's format, which is the authoritative image format.
    // Only the raster the region needs and the coordinate range a signed origin
    // may reach are bounded, so lattice arithmetic stays exact.
    if (request.region.x < -kMaxDimension || request.region.x > kMaxDimension || request.region.y < -kMaxDimension ||
        request.region.y > kMaxDimension)
        throw EvaluationException("request region origin is outside the " + std::to_string(kMaxDimension) +
                                  " pixel coordinate range");
}

void validateRequest(const Document& document, const EvaluationRequest& request) {
    if (request.network == kInvalidNetwork)
        throw EvaluationException("evaluation request must identify a network");
    validateRequestDomain(request);
    const NodeInstance* output = findNode(document, request.network, request.output);
    if (output == nullptr) {
        throw EvaluationException("request output node " + std::to_string(request.output) +
                                  " does not exist in network " + std::to_string(request.network));
    }
    const auto& graph = document.network(request.network).graph();
    const auto* outputSchema = graph.descriptor(output->type);
    // Processors with declared image outputs are valid evaluation targets too:
    // the interactive viewer renders the attached upstream node directly.
    //
    // A delivery sink is a legal target for that same reason, but its port list
    // cannot say so: it declares NO output ports, because it is the point an
    // explicit delivery job aims at rather than a producer feeding the graph.
    // Its schema states the fact instead (issue #94, story 72). It is still not
    // the network's result — `resolveOutput` only ever selects an Output node —
    // and the display-only Viewer role stays rejected here.
    if (outputSchema == nullptr ||
        (!outputSchema->isOutput && outputSchema->outputs.empty() && !outputSchema->isDeliverySink)) {
        failNode(*output, "evaluation request must target an Output node, a delivery node or a node type with "
                          "declared outputs");
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
        // An empty demand means every channel the image names, which is
        // admissible by definition (issue #90); an explicit demand must be part
        // of the node type's declared channel vocabulary.
        if (!request.channels.empty() && !capabilitiesCoverChannels(capabilities.channels, request.channels)) {
            failNode(node, "channels '" + channelListText(request.channels) + "' are not declared by node type '" +
                               node.type + "'");
        }
        if (std::find(capabilities.samplingScales.begin(), capabilities.samplingScales.end(), request.samplingScale) ==
            capabilities.samplingScales.end()) {
            failNode(node, "sampling scale " + std::to_string(request.samplingScale) +
                               " is not declared by node type '" + node.type + "'");
        }
        // Declared region support is a planner fact, not a rejection
        // (issue #85): a contribution that declares supportsRegion=false is
        // processed over its whole described domain internally and its consumers
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
    // The reference executor serves any named channel set: a raster carries
    // exactly the channels its description names (issue #90), so a named demand
    // is a request for names — never a request this executor cannot honour.
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

    // Descriptions, coverage and coverage channels are resolved once, before
    // any pixel work: every metadata, declaration, parameter and source
    // geometry failure is reported with its offending node here, and this
    // call's delivered region is the plan's normalized request.
    const RegionPlan regionPlan = planDependencyRegions(document, request, *contributions, sources);
    const EvaluationRequest& normalized = regionPlan.request;
    // Publication freshness (issue #9): capture revision + generation at
    // request start; computed results publish only while both hold.
    const EvaluationTicket ticket = reuse != nullptr ? reuse->beginTicket(document) : EvaluationTicket{};

    // Executor-specific availability is a declaration failure too, so it is
    // reported for every scheduled node before the first raster is allocated.
    for (const ExpandedNode& expandedNode : regionPlan.images.order) {
        if (expandedNode.alias)
            continue;
        const NodeInstance& node = regionPlan.images.nodes.at(expandedNode.id).node;
        const NodeContribution* contribution = contributions->find(node.type);
        // Delivery is a pixel role too (issue #94): it must report a missing
        // CPU adapter instead of reaching an absent callback.
        const bool producesPixels = contribution->role == NodeRole::Image || contribution->role == NodeRole::Source ||
                                    contribution->role == NodeRole::Delivery;
        if (producesPixels && !contribution->cpu) {
            // A GPU-only (or otherwise unavailable) contribution is honest
            // about it: report the node and its reason instead of inventing
            // a fallback image.
            failNode(node, contribution->cpuUnavailableReason.empty()
                               ? "no CPU reference implementation is registered for this node type"
                               : contribution->cpuUnavailableReason);
        }
    }

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
    plan.description = regionPlan.images.nodes.at(regionPlan.images.order.back().id).description;

    for (const ExpandedNode& expandedNode : regionPlan.images.order) {
        const ResolvedImageNode& resolved = regionPlan.images.nodes.at(expandedNode.id);
        const NodeInstance& node = resolved.node;
        PlanStep step;
        step.network = expandedNode.id.network;
        step.instance = expandedNode.id.instance;
        step.node = node.id;
        step.outputPort = expandedNode.id.outputPort;
        step.path = expandedNode.id.path;
        step.type = node.type;
        step.name = node.name;
        // The resolution happened once, in the plan: execution consumes that
        // immutable state and never re-derives animation or source mapping.
        step.effectiveParams = node.params;
        step.description = resolved.description;
        const EvaluationRequest nodeRequest = regionPlan.requests.at(expandedNode.id);
        const NodeCatalog& scopedCatalog = document.network(expandedNode.id.network).graph().catalog();
        const NodeContribution* contribution = expandedNode.alias ? nullptr : contributions->find(node.type);

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
        // Hash exactly the parameter map execution consumes, plus the described
        // meaning of the image this node produces. In particular, animated
        // values must affect this node's identity and all dependent identities,
        // while equal effective values remain reusable across unrelated history
        // revisions. A source node's key additionally carries its pre-resolved
        // effective request (the one thing that decides which frame is read) and
        // the content identity of the color configuration the provider resolves
        // media color against, so a changed configuration can never serve a
        // result produced under the previous one (issue #75).
        KeyContext keyContext;
        keyContext.description = &resolved.description;
        keyContext.source = resolved.source ? &*resolved.source : nullptr;
        if (resolved.source && contribution != nullptr && contribution->role == NodeRole::Source) {
            step.effectiveParams["sourcePath"] = resolved.source->path;
            step.effectiveParams["frame"] = resolved.source->sourceFrame;
        }
        if (sources != nullptr && contribution != nullptr && contribution->role == NodeRole::Source)
            keyContext.colorConfigIdentity = sources->colorConfigIdentity();
        // Content identity is deliberately independent of coverage, and inputs
        // contribute their CONTENT hashes: which rectangle currently backs an
        // upstream image must never change what this node's pixels mean
        // (issue #85), or a pan would invalidate the whole graph.
        const ResultKey contentKey = nodeContentKey(document, node, inputContentHashes, nodeRequest, keyContext);
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
            std::vector<const ImageDescription*> inputDescriptions;
            inputs.reserve(expandedNode.inputs.size());
            inputRequests.reserve(expandedNode.inputs.size());
            inputDescriptions.reserve(expandedNode.inputs.size());
            for (const EvaluationNodeId& producer : expandedNode.inputs) {
                inputs.push_back(producer.node == kInvalidNode ? nullptr : images.at(producer).get());
                inputRequests.push_back(producer.node == kInvalidNode ? EvaluationRequest{} : coverage.at(producer));
                inputDescriptions.push_back(
                    producer.node == kInvalidNode ? nullptr : &regionPlan.images.nodes.at(producer).description);
            }
            const std::span<const CpuImage* const> contextInputs(inputs.data(), inputs.size());
            const std::span<const EvaluationRequest> contextRequests(inputRequests.data(), inputRequests.size());
            const std::span<const ImageDescription* const> contextDescriptions(inputDescriptions.data(),
                                                                               inputDescriptions.size());
            const CpuNodeContext context{document,
                                         scopedCatalog,
                                         node,
                                         nodeRequest,
                                         node.params,
                                         contextInputs,
                                         sources,
                                         contextRequests,
                                         resolved.description,
                                         resolved.source ? &*resolved.source : nullptr,
                                         contextDescriptions,
                                         &document.network(expandedNode.id.network).format(),
                                         expandedNode.id.network};

            if (contribution->role == NodeRole::Output) {
                if (inputs.empty() || inputs[0] == nullptr)
                    failNode(node, "output requires a connected color input");
                const CpuImage& source = *inputs[0];
                // The delivered raster carries the described image's
                // interpretation, not a default one: an inherited Data or
                // premultiplied interpretation survives delivery.
                const ImageLayout layout = imageLayoutOf(
                    resolved.description, scaledDimension(nodeRequest.region.width, nodeRequest.samplingScale),
                    scaledDimension(nodeRequest.region.height, nodeRequest.samplingScale));
                const InputAnchor anchor = anchorInput(context, 0, source);
                const EvaluationRequest& sourceRequest = inputRequests[0];
                const ImageDescription* sourceDescription = inputDescriptions[0];
                // Identical raster, origin AND described image: delivery really
                // is the input, whose support the producing node already
                // enforced. A different description is a different image, so it
                // is copied (and guarded) rather than relabelled in place.
                if (sourceRequest.region == nodeRequest.region &&
                    sourceRequest.samplingScale == nodeRequest.samplingScale && source.layout() == layout &&
                    sourceDescription != nullptr && *sourceDescription == resolved.description) {
                    image = images.at(expandedNode.inputs[0]);
                    step.produced = identities.at(expandedNode.inputs[0]);
                } else {
                    // Preserve the existing Output interpretation contract;
                    // do not relabel a shared upstream/cache image in place.
                    // The input may cover more (a whole-domain escalation or a
                    // wider resident rectangle), so the delivered raster is the
                    // requested window at its real origin.
                    auto fresh = std::make_shared<CpuImage>(windowOf(source, anchor, layout));
                    enforceDataWindow(*fresh, nodeRequest, resolved.description);
                    step.produced = identityOf(*fresh, Residency::HostCpuReference);
                    image = std::move(fresh);
                }
            } else if (contribution->role == NodeRole::Viewer) {
                // Unreachable through a valid request (the viewer is not a
                // network output), but never silently rendered: a Viewer has no
                // pixel implementation.
                failNode(node, contribution->cpuUnavailableReason.empty()
                                   ? "the Viewer role has no CPU pixel implementation"
                                   : contribution->cpuUnavailableReason);
            } else {
                auto fresh = std::make_shared<CpuImage>(contribution->cpu->execute(context));
                // Shared auxiliary preservation (issue #90): an ordinary effect
                // addresses its main input's RGBA projection and inherits the
                // meaning of everything else, so the named channels it does not
                // address are carried over unchanged. A node that owns its
                // channel layout (Shuffle) states every channel itself and is
                // never overlaid.
                if (!contribution->ownsChannelLayout) {
                    *fresh = preserveAuxiliaryChannels(context, std::move(*fresh), contribution->descriptor.mainInput);
                }
                // The one central support guard: whatever a node's own pixel
                // math produced, the raster agrees with the description it
                // declared (transparent black outside the data window).
                enforceDataWindow(*fresh, nodeRequest, resolved.description);
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
        // executor computed (or reused) more for the sake of reuse, and it
        // carries the described target's interpretation.
        const ImageDescription& description = evaluation.plan.description;
        const ImageLayout layout =
            imageLayoutOf(description, scaledDimension(normalized.region.width, normalized.samplingScale),
                          scaledDimension(normalized.region.height, normalized.samplingScale));
        evaluation.image =
            windowOf(*images.at(outputKey), requiredAnchor(*images.at(outputKey), delivered, normalized), layout);
        evaluation.plan.result = identityOf(evaluation.image, Residency::HostCpuReference);
    }
    return evaluation;
}

}  // namespace nemo
