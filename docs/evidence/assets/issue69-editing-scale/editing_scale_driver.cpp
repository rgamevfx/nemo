// Throwaway measurement driver for issue #69 (editing, preview and undo/redo
// history scaling on Linux). NOT product code: it is not a CLI subcommand, an
// installed tool, a public API or a permanent test suite. It is compiled
// directly against the shipped nemo::core headers/library and driven from
// docs/evidence/issue69-editing-scale.md.
//
// Every workload is populated through submitted commands on a real
// ProjectSession; the driver never mutates Document/Graph internals.
//
// Modes:
//   timings     per-operation cold/warm distributions for each declared workload
//   retention   glibc in-use-heap attribution for history/preview/save snapshots
//   correctness real-path observable outcomes (exit code 1 on any failure)
//   seed <p> <n> write a valid .nemo project for the nemo-cli real-path run
//
// Output: one JSON object per line on stdout; progress goes to stderr.

#include <malloc.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "nemo/core/commands/AnimationCommands.hpp"
#include "nemo/core/commands/MediaCatalogCommands.hpp"
#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Serialization.hpp"
#include "nemo/core/session/ProjectFile.hpp"
#include "nemo/core/session/ProjectSession.hpp"

namespace {

using namespace nemo;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// ProjectSession's default history capacity (src/nemo/core/session/ProjectSession.hpp).
constexpr std::size_t kDefaultHistoryCapacity = 256;
constexpr std::size_t kColdSamples = 20;
constexpr std::size_t kWarmupSamples = 50;
constexpr std::size_t kWarmSamples = 200;

#ifdef NDEBUG
constexpr const char* kBuildName = "release";
#else
constexpr const char* kBuildName = "debug";
#endif

[[nodiscard]] double microsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

void emit(const Json& record) {
    std::cout << record.dump() << '\n';
    std::cout.flush();
}

void note(const std::string& text) {
    std::cerr << "issue69-driver: " << text << '\n';
}

[[nodiscard]] unsigned long long heapInUse() {
    return static_cast<unsigned long long>(mallinfo2().uordblks);
}

// ---------------------------------------------------------------- statistics

struct Stats {
    std::size_t count{};
    double minUs{};
    double p50Us{};
    double p95Us{};
    double maxUs{};
    double meanUs{};
};

// Nearest-rank percentile over an ascending sample vector:
// rank = ceil(p * n), clamped to [1, n]; the reported value is the sample at
// rank-1. p50 of an even count is therefore the lower-middle order statistic.
[[nodiscard]] double percentileAt(const std::vector<double>& sorted, double p) {
    if (sorted.empty())
        return 0.0;
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    const std::size_t index = std::min(std::max<std::size_t>(rank, 1), sorted.size()) - 1;
    return sorted[index];
}

[[nodiscard]] Stats statsOf(const std::vector<double>& samples) {
    Stats stats;
    if (samples.empty())
        return stats;
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (const double value : sorted)
        sum += value;
    stats.count = sorted.size();
    stats.minUs = sorted.front();
    stats.maxUs = sorted.back();
    stats.p50Us = percentileAt(sorted, 0.50);
    stats.p95Us = percentileAt(sorted, 0.95);
    stats.meanUs = sum / static_cast<double>(sorted.size());
    return stats;
}

[[nodiscard]] Json statsJson(const Stats& stats) {
    return Json{{"count", stats.count},
                {"min_us", stats.minUs},
                {"p50_us", stats.p50Us},
                {"p95_us", stats.p95Us},
                {"max_us", stats.maxUs},
                {"mean_us", stats.meanUs}};
}

// Runs `run` verbatim; the timer covers only that call. `setup` (once before
// sampling), `after` (after every timed call) and `teardown` (once at the end)
// run untimed and are harness, not measured scope. Cold samples are the first
// kColdSamples executions of the operation immediately after its setup, with no
// warm-up for that operation; warm samples follow kWarmupSamples untimed
// executions on the same session.
void measureOp(const std::string& variant, const std::string& op, const Json& notes, const std::size_t coldSamples,
               const std::size_t warmupSamples, const std::size_t warmSamples, const std::function<void()>& setup,
               const std::function<void()>& run, const std::function<void()>& after = {},
               const std::function<void()>& teardown = {}) {
    if (setup)
        setup();
    std::vector<double> cold;
    cold.reserve(coldSamples);
    for (std::size_t i = 0; i < coldSamples; ++i) {
        const auto start = Clock::now();
        run();
        cold.push_back(microsSince(start));
        if (after)
            after();
    }
    for (std::size_t i = 0; i < warmupSamples; ++i) {
        run();
        if (after)
            after();
    }
    std::vector<double> warm;
    warm.reserve(warmSamples);
    for (std::size_t i = 0; i < warmSamples; ++i) {
        const auto start = Clock::now();
        run();
        warm.push_back(microsSince(start));
        if (after)
            after();
    }
    if (teardown)
        teardown();
    emit(Json{{"record", "op"},
              {"variant", variant},
              {"op", op},
              {"notes", notes},
              {"cold", statsJson(statsOf(cold))},
              {"warm", statsJson(statsOf(warm))}});
}

// ------------------------------------------------------------- session usage

[[nodiscard]] std::unique_ptr<ProjectSession> makeSession(std::size_t historyCapacity = kDefaultHistoryCapacity) {
    return std::make_unique<ProjectSession>(Document{}, historyCapacity);
}

EditResult submitOk(ProjectSession& session, Command command, std::string requestId = {}) {
    EditResult result = session.submit(std::move(command), EditOptions{session.revision(), std::move(requestId)});
    if (!result.committed)
        throw std::runtime_error("workload submit rejected: " +
                                 (result.error ? result.error->message : std::string{"unknown reason"}));
    return result;
}

// Retained undo depth observed only through the external contract: undo to
// exhaustion, then restore. Bounded by the session capacity. Untimed harness
// use only.
[[nodiscard]] std::size_t retainedDepth(ProjectSession& session) {
    std::size_t count = 0;
    while (session.canUndo()) {
        if (!session.undo(EditOptions{session.revision(), {}}).committed)
            break;
        ++count;
        if (count > 100000)
            break;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!session.redo(EditOptions{session.revision(), {}}).committed)
            throw std::runtime_error("retainedDepth restore redo failed");
    }
    return count;
}

[[nodiscard]] std::shared_ptr<NodeId> addNode(ProjectSession& session, NetworkId network, const std::string& type,
                                              const std::string& name) {
    auto created = std::make_shared<NodeId>(kInvalidNode);
    submitOk(session, addNodeCommand(network, type, name, created));
    return created;
}

// Chain of `totalNodes` nodes: one constcolor head then blur nodes, each
// connected from the previous node's image output to input port 0.
[[nodiscard]] NodeId buildChain(ProjectSession& session, NetworkId network, std::size_t totalNodes,
                                const std::string& prefix) {
    if (totalNodes == 0)
        return kInvalidNode;
    NodeId previous = *addNode(session, network, "constcolor", prefix + "0");
    for (std::size_t i = 1; i < totalNodes; ++i) {
        const NodeId current = *addNode(session, network, "blur", prefix + std::to_string(i));
        submitOk(session, connectCommand(network, PortRef{previous, 0}, PortRef{current, 0}));
        previous = current;
    }
    return previous;
}

// Balanced fan-in tree of `leaves` constcolor sources feeding merge nodes.
void buildFanIn(ProjectSession& session, NetworkId network, std::size_t leaves) {
    std::vector<NodeId> level;
    level.reserve(leaves);
    for (std::size_t i = 0; i < leaves; ++i)
        level.push_back(*addNode(session, network, "constcolor", "leaf" + std::to_string(i)));
    std::size_t generation = 0;
    while (level.size() > 1) {
        std::vector<NodeId> next;
        for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
            const NodeId merge =
                *addNode(session, network, "merge", "merge" + std::to_string(generation) + "_" + std::to_string(i / 2));
            submitOk(session, connectCommand(network, PortRef{level[i], 0}, PortRef{merge, 0}));
            submitOk(session, connectCommand(network, PortRef{level[i + 1], 0}, PortRef{merge, 1}));
            next.push_back(merge);
        }
        if (level.size() % 2 == 1)
            next.push_back(level.back());
        level = std::move(next);
        ++generation;
    }
}

[[nodiscard]] std::vector<NodeId> buildGradeSurface(ProjectSession& session, NetworkId network, std::size_t count,
                                                    const std::string& prefix) {
    std::vector<NodeId> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        ids.push_back(*addNode(session, network, "grade", prefix + std::to_string(i)));
    return ids;
}

// Authors typed parameter payload on grade nodes through one submitted
// setParametersCommand batch. `all` covers every grade parameter (7 color, 2
// choice, 4 boolean, 1 float); otherwise the four-parameter subset keeps the
// edit surface small. Values are valid catalog values.
void authorGradeParameters(ProjectSession& session, NetworkId network, const std::vector<NodeId>& gradeIds, bool all) {
    if (gradeIds.empty())
        return;
    std::vector<ParameterEdit> edits;
    edits.reserve(gradeIds.size() * (all ? 14 : 4));
    const auto colorAt = [](double unit) {
        const float channel = static_cast<float>(0.2 + 0.1 * unit);
        return ParameterValue{ColorValue{{channel, channel * 0.5F, 1.0F - channel, 1.0F}}};
    };
    for (std::size_t i = 0; i < gradeIds.size(); ++i) {
        const auto address = [&](const char* key) { return ParameterAddress{network, gradeIds[i], key}; };
        const double unit = static_cast<double>(i % 5);
        edits.push_back(ParameterEdit{address("gain"), colorAt(unit)});
        edits.push_back(ParameterEdit{address("mix"), ParameterValue{0.2 + 0.1 * unit}});
        edits.push_back(ParameterEdit{address("channels"), ParameterValue{ChoiceValue{"RGB"}}});
        edits.push_back(ParameterEdit{address("reverse"), ParameterValue{i % 2 == 0}});
        if (!all)
            continue;
        for (const char* key : {"blackpoint", "whitepoint", "lift", "multiply", "offset", "gamma"})
            edits.push_back(ParameterEdit{address(key), colorAt(unit)});
        edits.push_back(ParameterEdit{address("maskChannel"), ParameterValue{ChoiceValue{"A"}}});
        edits.push_back(ParameterEdit{address("invertMask"), ParameterValue{i % 3 == 0}});
        edits.push_back(ParameterEdit{address("clampBlack"), ParameterValue{true}});
        edits.push_back(ParameterEdit{address("clampWhite"), ParameterValue{false}});
    }
    submitOk(session, setParametersCommand(std::move(edits)));
}

// ------------------------------------------------------------------ workload

enum class EditKind { Float, Color, String };

struct EditTarget {
    NetworkId network{kInvalidNetwork};
    NodeId node{kInvalidNode};
    std::string key;
    EditKind kind{EditKind::Float};
};

struct Workload {
    std::string name;
    std::unique_ptr<ProjectSession> session;
    std::vector<EditTarget> targets;  // nodes carrying typed parameters
    bool graphOps{false};
    NetworkId growNetwork{kInvalidNetwork};
    NodeId growTail{kInvalidNode};
    std::shared_ptr<NodeId> growCreated;
    NodeId renameTarget{kInvalidNode};
    NodeId layoutTarget{kInvalidNode};
    bool animationOps{false};
    ParameterAddress animationAddress;
    bool mediaOps{false};
    MediaSourceId mediaEntry{kInvalidMediaSource};
    MediaBinId mediaBinA{kInvalidMediaBin};
    MediaBinId mediaBinB{kInvalidMediaBin};
};

[[nodiscard]] ParameterValue cycleValue(EditKind kind, std::size_t counter) {
    const double unit = static_cast<double>(counter % 64) / 64.0;
    switch (kind) {
    case EditKind::Float:
        return ParameterValue{unit * 99.0};
    case EditKind::Color: {
        const float channel = static_cast<float>(0.1 + 0.8 * unit);
        return ParameterValue{ColorValue{{channel, channel, channel, 1.0F}}};
    }
    case EditKind::String:
        return ParameterValue{std::string{"media/plate_"} + std::to_string(counter % 32) + ".exr"};
    }
    return ParameterValue{0.0};
}

void collectTargets(Workload& workload) {
    workload.targets.clear();
    const Document& document = workload.session->document();
    for (const auto& network : document.networks()) {
        for (const auto& node : network.graph().nodes()) {
            if (node.type == "blur")
                workload.targets.push_back(EditTarget{network.id(), node.id, "size", EditKind::Float});
            else if (node.type == "grade")
                workload.targets.push_back(EditTarget{network.id(), node.id, "gain", EditKind::Color});
            else if (node.type == "constcolor")
                workload.targets.push_back(EditTarget{network.id(), node.id, "color", EditKind::Color});
            else if (node.type == "source")
                workload.targets.push_back(EditTarget{network.id(), node.id, "source", EditKind::String});
        }
    }
}

[[nodiscard]] std::vector<ParameterEdit> makeBatch(const Workload& workload, std::size_t start, std::size_t count) {
    std::vector<ParameterEdit> edits;
    edits.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t index = (start + i) % workload.targets.size();
        const EditTarget& target = workload.targets[index];
        edits.push_back(
            ParameterEdit{ParameterAddress{target.network, target.node, target.key}, cycleValue(target.kind, start + i)});
    }
    return edits;
}

// ------------------------------------------------------------------ builders

[[nodiscard]] Workload buildEditSurface(const std::string& name, std::size_t gradeSurface,
                                        std::size_t historyCapacity = kDefaultHistoryCapacity) {
    Workload workload;
    workload.name = name;
    workload.session = makeSession(historyCapacity);
    const NetworkId network = workload.session->document().rootNetworkId();
    const std::vector<NodeId> grades = buildGradeSurface(*workload.session, network, gradeSurface, "grade");
    authorGradeParameters(*workload.session, network, grades, /*all=*/false);
    // One source node carries the string parameter type.
    auto sourceNode = std::make_shared<NodeId>(kInvalidNode);
    submitOk(*workload.session, addNodeCommand(network, "source", "source0", sourceNode));
    submitOk(*workload.session, setParamCommand(network, *sourceNode, "source",
                                                ParameterValue{std::string{"media/plate_0.exr"}}));
    collectTargets(workload);
    return workload;
}

[[nodiscard]] Workload buildChainWorkload(const std::string& name, std::size_t gradeSurface, std::size_t chainNodes,
                                          std::size_t historyCapacity = kDefaultHistoryCapacity) {
    Workload workload = buildEditSurface(name, gradeSurface, historyCapacity);
    ProjectSession& session = *workload.session;
    const NetworkId network = session.document().rootNetworkId();
    workload.growTail = buildChain(session, network, chainNodes, "plate");
    workload.growNetwork = network;
    workload.growCreated = std::make_shared<NodeId>(kInvalidNode);
    workload.graphOps = true;
    workload.renameTarget = workload.growTail;
    workload.layoutTarget = workload.growTail;
    collectTargets(workload);
    return workload;
}

[[nodiscard]] Workload buildFanInWorkload(const std::string& name, std::size_t gradeSurface, std::size_t leaves) {
    Workload workload = buildEditSurface(name, gradeSurface);
    ProjectSession& session = *workload.session;
    const NetworkId network = session.document().rootNetworkId();
    buildFanIn(session, network, leaves);
    for (const auto& node : session.document().network(network).graph().nodes()) {
        if (node.type == "constcolor") {
            workload.renameTarget = node.id;
            break;
        }
    }
    workload.layoutTarget = workload.renameTarget;
    workload.growNetwork = network;
    workload.growTail = workload.renameTarget;
    workload.growCreated = std::make_shared<NodeId>(kInvalidNode);
    workload.graphOps = true;
    collectTargets(workload);
    return workload;
}

[[nodiscard]] Workload buildMultiNetworkWorkload(const std::string& name, std::size_t gradeSurface,
                                                 std::size_t networks, std::size_t nodesPerNetwork) {
    Workload workload = buildEditSurface(name, gradeSurface);
    ProjectSession& session = *workload.session;
    for (std::size_t i = 0; i < networks; ++i) {
        auto created = std::make_shared<NetworkId>(kInvalidNetwork);
        submitOk(session, addNetworkCommand("net" + std::to_string(i), created));
        const NodeId tail = buildChain(session, *created, nodesPerNetwork, "n" + std::to_string(i) + "_");
        if (i == 0) {
            workload.growTail = tail;
            workload.growNetwork = *created;
            workload.renameTarget = tail;
            workload.layoutTarget = tail;
        }
    }
    workload.growCreated = std::make_shared<NodeId>(kInvalidNode);
    workload.graphOps = true;
    collectTargets(workload);
    return workload;
}

[[nodiscard]] Workload buildParameterWorkload(const std::string& name, std::size_t gradeSurface,
                                              std::size_t extraGrade) {
    Workload workload = buildEditSurface(name, gradeSurface);
    const NetworkId network = workload.session->document().rootNetworkId();
    const std::vector<NodeId> payload = buildGradeSurface(*workload.session, network, extraGrade, "payload");
    authorGradeParameters(*workload.session, network, payload, /*all=*/true);
    collectTargets(workload);
    return workload;
}

void addChannelKeys(ProjectSession& session, const ParameterAddress& address, std::size_t keys) {
    std::vector<KeyframeEdit> edits;
    edits.reserve(keys);
    for (std::size_t i = 0; i < keys; ++i) {
        Keyframe key;
        key.id = kInvalidKeyframe;  // upsert/create
        key.time = static_cast<double>(i);
        const float value = static_cast<float>(0.05 * static_cast<double>(i + 1));
        key.value = ParameterValue{ColorValue{{value, value * 0.5F, 1.0F - value, 1.0F}}};
        key.interpolation = static_cast<KeyInterpolation>(i % 3);
        key.tangentMode = (i % 2 == 0) ? TangentMode::Smooth : TangentMode::Broken;
        // Smooth tangents require equal incoming and outgoing slopes; broken
        // tangents may differ.
        if (key.tangentMode == TangentMode::Smooth) {
            key.inSlope = {0.1, 0.2, 0.3, 0.4};
            key.outSlope = key.inSlope;
        } else {
            key.inSlope = {0.1, 0.2, 0.3, 0.4};
            key.outSlope = {0.4, 0.3, 0.2, 0.1};
        }
        edits.push_back(KeyframeEdit{address, std::move(key)});
    }
    submitOk(session, setKeyframesCommand(std::move(edits)));
}

[[nodiscard]] Workload buildAnimationWorkload(const std::string& name, std::size_t gradeSurface, std::size_t channels,
                                              std::size_t keysPerChannel) {
    Workload workload = buildEditSurface(name, gradeSurface);
    ProjectSession& session = *workload.session;
    const NetworkId network = session.document().rootNetworkId();
    // Copy ids of parameter-carrying grade nodes: submitting commands replaces
    // the session Document's network storage, so a reference into
    // graph().nodes() would dangle across submits.
    std::vector<NodeId> nodeIds;
    for (const auto& node : session.document().network(network).graph().nodes()) {
        if (node.type == "grade")
            nodeIds.push_back(node.id);
    }
    if (nodeIds.empty())
        throw std::runtime_error("animation workload requires grade nodes");
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const ParameterAddress address{network, nodeIds[channel % nodeIds.size()], "gain"};
        addChannelKeys(session, address, keysPerChannel);
        if (channel == 0)
            workload.animationAddress = address;
    }
    workload.animationOps = true;
    collectTargets(workload);
    return workload;
}

void addMediaEntry(Workload& workload, std::size_t index, MediaBinId bin) {
    ProjectSession& session = *workload.session;
    const std::string sourceKey = "media/plate_" + std::to_string(index) + ".exr";
    SourceReference reference;
    reference.path = sourceKey;
    reference.frameOffset = 1;
    reference.frameStep = 1;
    reference.revision = 1;
    reference.interpretation["colorspace"] = "ACEScg";
    submitOk(session, setSourceCommand(sourceKey, reference));
    auto created = std::make_shared<MediaSourceId>(kInvalidMediaSource);
    submitOk(session, importMediaReferenceCommand(sourceKey, bin, MediaMetadata{}, created));
    MediaMetadata metadata;
    metadata.userName = "plate_" + std::to_string(index);
    metadata.description = "declared media catalog entry for issue 69 scaling evidence";
    metadata.tags = {"plates", "issue69", "scale"};
    metadata.label = "Plate " + std::to_string(index);
    metadata.kind = MediaKind::Sequence;
    submitOk(session, setMediaMetadataCommand(*created, metadata));
    submitOk(session, setMediaMarksCommand(*created, {MediaMarkRange{0, 48, {}}, MediaMarkRange{96, 144, {}}}));
    MediaProbeMetadata probe;
    probe.width = 1920;
    probe.height = 1080;
    probe.duration = 144;
    probe.codec = "prores4444";
    probe.colorPrimaries = "ACEScg";
    probe.colorTransfer = "linear";
    probe.colorMatrix = "identity";
    probe.provenance = "issue69-editing-scale-driver";
    probe.status = MediaProbeStatus::Ready;
    submitOk(session, commitMediaProbeCommand(*created, reference, probe));
    if (index == 0)
        workload.mediaEntry = *created;
}

[[nodiscard]] Workload buildMediaWorkload(const std::string& name, std::size_t gradeSurface, std::size_t bins,
                                          std::size_t entries) {
    Workload workload = buildEditSurface(name, gradeSurface);
    MediaBinId parent = kInvalidMediaBin;
    for (std::size_t i = 0; i < bins; ++i) {
        auto created = std::make_shared<MediaBinId>(kInvalidMediaBin);
        submitOk(*workload.session, createBinCommand("bin" + std::to_string(i), parent, created));
        parent = *created;
        if (i == 0)
            workload.mediaBinA = *created;
        if (i + 1 == bins)
            workload.mediaBinB = *created;
    }
    for (std::size_t i = 0; i < entries; ++i)
        addMediaEntry(workload, i, (i % 2 == 0) ? workload.mediaBinA : workload.mediaBinB);
    workload.mediaOps = true;
    collectTargets(workload);
    return workload;
}

// ------------------------------------------------------------- doc summaries

[[nodiscard]] const char* valueTypeName(const ParameterValue& value) {
    switch (value.index()) {
    case 0:
        return "boolean";
    case 1:
        return "integer";
    case 2:
        return "float";
    case 3:
        return "string";
    case 4:
        return "choice";
    case 5:
        return "vector2";
    case 6:
        return "vector3";
    case 7:
        return "color";
    default:
        return "unknown";
    }
}

[[nodiscard]] Json compositionOf(const ProjectSession& session) {
    const Document& document = session.document();
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::map<std::string, std::size_t> paramsByType;
    std::size_t params = 0;
    for (const auto& network : document.networks()) {
        nodes += network.graph().nodes().size();
        edges += network.graph().edges().size();
        for (const auto& node : network.graph().nodes()) {
            for (const auto& [key, value] : node.params) {
                static_cast<void>(key);
                ++params;
                ++paramsByType[valueTypeName(value)];
            }
        }
    }
    std::size_t keys = 0;
    std::map<std::string, std::size_t> interpolation;
    for (const auto& channel : document.animationChannels()) {
        keys += channel.keys.size();
        for (const auto& key : channel.keys) {
            switch (key.interpolation) {
            case KeyInterpolation::Hold:
                ++interpolation["hold"];
                break;
            case KeyInterpolation::Linear:
                ++interpolation["linear"];
                break;
            case KeyInterpolation::Bezier:
                ++interpolation["bezier"];
                break;
            }
        }
    }
    std::size_t marks = 0;
    for (const auto& entry : document.mediaCatalog().entries())
        marks += entry.marks.size();
    return Json{{"networks", document.networks().size()},
                {"nodes", nodes},
                {"edges", edges},
                {"parameters", params},
                {"parameters_by_type", paramsByType},
                {"sources", document.sources.size()},
                {"media_entries", document.mediaCatalog().entries().size()},
                {"media_bins", document.mediaCatalog().bins().size()},
                {"media_marks", marks},
                {"animation_channels", document.animationChannels().size()},
                {"animation_keys", keys},
                {"animation_interpolation", interpolation},
                {"serialized_bytes", ProjectFile::serializeContent(document, Json{}, {}).size()}};
}

void emitWorkload(const Workload& workload, std::size_t historyCapacity) {
    emit(Json{{"record", "workload"},
              {"variant", workload.name},
              {"history_capacity", historyCapacity},
              {"edit_targets", workload.targets.size()},
              {"composition", compositionOf(*workload.session)}});
}

// -------------------------------------------------------------- common timers

void runCommonOps(const Workload& workload) {
    ProjectSession& session = *workload.session;
    const std::string& name = workload.name;
    const Json notes{{"history_capacity", kDefaultHistoryCapacity}, {"edit_targets", workload.targets.size()}};

    measureOp(name, "snapshot", notes, kColdSamples, kWarmupSamples, kWarmSamples, {},
              [&] { static_cast<void>(session.snapshot()); });

    measureOp(name, "prepare_save", notes, kColdSamples, 10, 60, {}, [&] {
        static_cast<void>(session.prepareSave("/tmp/issue69-driver/out.nemo", PathPolicy::KeepStored, false));
    });

    measureOp(name, "serialize_content", notes, kColdSamples, 10, 60, {}, [&] {
        static_cast<void>(ProjectFile::serializeContent(session.document(), Json{}, {}));
    });

    // Undo/redo of a declared, state-neutral set of committed edits.
    const std::size_t historyRun = std::min<std::size_t>(64, kDefaultHistoryCapacity);
    std::size_t counter = 0;
    for (std::size_t i = 0; i < historyRun; ++i) {
        const EditTarget& target = workload.targets[i % workload.targets.size()];
        submitOk(session, setParamCommand(target.network, target.node, target.key, cycleValue(target.kind, counter++)));
    }
    std::vector<double> undoSamples;
    undoSamples.reserve(historyRun);
    for (std::size_t i = 0; i < historyRun; ++i) {
        const auto start = Clock::now();
        const EditResult result = session.undo(EditOptions{session.revision(), {}});
        undoSamples.push_back(microsSince(start));
        if (!result.committed)
            throw std::runtime_error("undo failed during timing");
    }
    std::vector<double> redoSamples;
    redoSamples.reserve(historyRun);
    for (std::size_t i = 0; i < historyRun; ++i) {
        const auto start = Clock::now();
        const EditResult result = session.redo(EditOptions{session.revision(), {}});
        redoSamples.push_back(microsSince(start));
        if (!result.committed)
            throw std::runtime_error("redo failed during timing");
    }
    const Json sequenceNotes{{"history_capacity", kDefaultHistoryCapacity},
                             {"entries", historyRun},
                             {"history_depth_before", historyRun},
                             {"cold_warm_split", "not applicable: one ordered sequence"}};
    emit(Json{{"record", "op"},
              {"variant", name},
              {"op", "undo_sequence"},
              {"notes", sequenceNotes},
              {"cold", statsJson(statsOf(undoSamples))},
              {"warm", Json::object()}});
    emit(Json{{"record", "op"},
              {"variant", name},
              {"op", "redo_sequence"},
              {"notes", sequenceNotes},
              {"cold", statsJson(statsOf(redoSamples))},
              {"warm", Json::object()}});
}

void runParameterOps(const Workload& workload) {
    ProjectSession& session = *workload.session;
    const std::string& name = workload.name;
    const Json notes{{"history_capacity", kDefaultHistoryCapacity}, {"edit_targets", workload.targets.size()}};
    const auto undoLast = [&] { static_cast<void>(session.undo(EditOptions{session.revision(), {}})); };
    const auto neutralNotes = [&](Json base) {
        base["history_depth_at_start"] = retainedDepth(session);
        base["samples_undone_between_runs"] = true;
        return base;
    };
    std::size_t counter = 0;

    measureOp(name, "submit.set_param", neutralNotes(notes), kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
        const EditTarget& target = workload.targets[counter % workload.targets.size()];
        submitOk(session, setParamCommand(target.network, target.node, target.key, cycleValue(target.kind, counter)));
        ++counter;
    },
              undoLast);

    for (const std::size_t batch : {std::size_t{8}, std::size_t{64}}) {
        if (batch > workload.targets.size())
            continue;
        std::size_t batchCounter = 0;
        measureOp(name, "submit.set_parameters." + std::to_string(batch),
                  neutralNotes(Json{{"history_capacity", kDefaultHistoryCapacity},
                                    {"batch_edits", batch},
                                    {"distinct_nodes", batch}}),
                  kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
                      submitOk(session, setParametersCommand(makeBatch(workload, batchCounter, batch)));
                      batchCounter += batch;
                  },
                  undoLast);
    }

    const std::size_t transactionSize = std::min<std::size_t>(8, workload.targets.size());
    std::size_t transactionCounter = 0;
    measureOp(name, "submit.transaction." + std::to_string(transactionSize),
              neutralNotes(Json{{"history_capacity", kDefaultHistoryCapacity},
                                {"transaction_commands", transactionSize}}),
              kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
                  std::vector<Command> commands;
                  commands.reserve(transactionSize);
                  for (std::size_t i = 0; i < transactionSize; ++i) {
                      const EditTarget& target = workload.targets[(transactionCounter + i) % workload.targets.size()];
                      commands.push_back(setParamCommand(target.network, target.node, target.key,
                                                         cycleValue(target.kind, transactionCounter + i)));
                  }
                  transactionCounter += transactionSize;
                  submitOk(session, transactionCommand("batch edit", std::move(commands)));
              },
              undoLast);

    const EditTarget& gestureTarget = workload.targets.front();
    const std::vector<ParameterEdit> gestureEdit{ParameterEdit{
        ParameterAddress{gestureTarget.network, gestureTarget.node, gestureTarget.key}, cycleValue(gestureTarget.kind, 7)}};
    ParameterGestureToken token = 0;

    measureOp(name, "gesture.begin", Json{{"edits", 1}, {"publishes", false}}, kColdSamples, kWarmupSamples, kWarmSamples,
              {}, [&] {
                  const ParameterGestureResult result =
                      session.beginParameterGesture(gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("gesture begin failed during timing");
                  token = result.token;
              },
              [&] { static_cast<void>(session.cancelParameterGesture(token)); });

    measureOp(name, "gesture.update", Json{{"edits", 1}, {"publishes", false}}, kColdSamples, kWarmupSamples,
              kWarmSamples,
              [&] {
                  const ParameterGestureResult result =
                      session.beginParameterGesture(gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("gesture begin failed before update timing");
                  token = result.token;
              },
              [&] {
                  const ParameterGestureResult result = session.updateParameterGesture(token, gestureEdit);
                  if (!result.snapshot)
                      throw std::runtime_error("gesture update failed during timing");
              },
              {}, [&] { static_cast<void>(session.cancelParameterGesture(token)); });

    measureOp(name, "gesture.cancel", Json{{"edits", 1}, {"publishes", false}}, kColdSamples, kWarmupSamples,
              kWarmSamples,
              [&] {
                  const ParameterGestureResult result =
                      session.beginParameterGesture(gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("gesture begin failed before cancel timing");
                  token = result.token;
              },
              [&] {
                  const EditResult result = session.cancelParameterGesture(token);
                  if (result.error)
                      throw std::runtime_error("gesture cancel failed during timing");
              },
              [&] {
                  const ParameterGestureResult result =
                      session.beginParameterGesture(gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("gesture begin failed before cancel timing");
                  token = result.token;
              },
              [&] { static_cast<void>(session.cancelParameterGesture(token)); });

    measureOp(name, "gesture.commit",
              Json{{"edits", 1},
                   {"publishes", true},
                   {"history_entries", 1},
                   {"history_depth_at_start", retainedDepth(session)},
                   {"committed_entry_undone_between_runs", true}},
              kColdSamples, kWarmupSamples, kWarmSamples,
              [&] {
                  token = session.beginParameterGesture(gestureEdit, EditOptions{session.revision(), {}}).token;
              },
              [&] {
                  const EditResult result = session.commitParameterGesture(token, EditOptions{session.revision(), {}});
                  if (!result.committed)
                      throw std::runtime_error("gesture commit failed during timing");
              },
              [&] {
                  undoLast();
                  token = session.beginParameterGesture(gestureEdit, EditOptions{session.revision(), {}}).token;
              },
              [&] { static_cast<void>(session.cancelParameterGesture(token)); });
}

void runGraphOps(Workload& workload) {
    ProjectSession& session = *workload.session;
    const std::string& name = workload.name;
    std::size_t renames = 0;
    std::size_t layouts = 0;
    const std::size_t declaredNodes = session.document().network(workload.growNetwork).graph().nodes().size();
    const std::size_t declaredEdges = session.document().network(workload.growNetwork).graph().edges().size();
    const auto undoLast = [&] { static_cast<void>(session.undo(EditOptions{session.revision(), {}})); };
    const Json neutralGraphNotes{{"history_capacity", kDefaultHistoryCapacity},
                                 {"history_depth_at_start", retainedDepth(session)},
                                 {"samples_undone_between_runs", true}};

    measureOp(name, "submit.rename", neutralGraphNotes, kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
        submitOk(session, renameNodeCommand(workload.growNetwork, workload.renameTarget,
                                            "renamed" + std::to_string(renames++)));
    },
              undoLast);

    measureOp(name, "submit.set_layout", neutralGraphNotes, kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
        const double offset = static_cast<double>(layouts++ % 64);
        submitOk(session, setLayoutCommand(workload.growNetwork, workload.layoutTarget,
                                           LayoutPosition{offset, offset}));
    },
              undoLast);

    // Growth operations: each sample adds one node/edge to the declared
    // document and the untimed harness undoes it, so every sample starts from
    // the declared composition and cold/warm are comparable.
    std::size_t nodesAdded = 0;
    measureOp(name, "submit.add_node",
              Json{{"history_capacity", kDefaultHistoryCapacity},
                   {"declared_nodes", declaredNodes},
                   {"history_depth_at_start", retainedDepth(session)},
                   {"samples_undone_between_runs", true}},
              kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
                  submitOk(session, addNodeCommand(workload.growNetwork, "blur",
                                                   "grow" + std::to_string(nodesAdded++)));
              },
              undoLast);

    const NodeId connectTarget = *addNode(session, workload.growNetwork, "blur", "connect_target");
    measureOp(name, "submit.connect",
              Json{{"history_capacity", kDefaultHistoryCapacity},
                   {"declared_nodes", declaredNodes},
                   {"declared_edges", declaredEdges},
                   {"history_depth_at_start", retainedDepth(session)},
                   {"samples_undone_between_runs", true}},
              kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
                  submitOk(session, connectCommand(workload.growNetwork, PortRef{workload.growTail, 0},
                                                   PortRef{connectTarget, 0}));
              },
              undoLast);
}

void runAnimationOps(const Workload& workload) {
    ProjectSession& session = *workload.session;
    const std::string& name = workload.name;
    std::size_t keys = 0;
    for (const auto& channel : session.document().animationChannels())
        keys += channel.keys.size();

    // insert + undo keeps the document size constant across samples; the timed
    // scope is the insert submit only.
    measureOp(name, "submit.insert_keyframe",
              Json{{"history_capacity", kDefaultHistoryCapacity}, {"keys_before", keys}},
              kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
                  submitOk(session, insertKeyframeCommand(workload.animationAddress, 1000.0));
              },
              [&] { static_cast<void>(session.undo(EditOptions{session.revision(), {}})); });

    const std::vector<ParameterEdit> gestureEdit{
        ParameterEdit{workload.animationAddress, ParameterValue{ColorValue{{0.25F, 0.5F, 0.75F, 1.0F}}}}};
    ParameterGestureToken token = 0;
    measureOp(name, "gesture.keyed_begin", Json{{"edits", 1}, {"publishes", false}}, kColdSamples, kWarmupSamples,
              kWarmSamples, {}, [&] {
                  const ParameterGestureResult result =
                      session.beginKeyedParameterGesture(500.0, gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("keyed gesture begin failed during timing");
                  token = result.token;
              },
              [&] { static_cast<void>(session.cancelParameterGesture(token)); });
    measureOp(name, "gesture.keyed_update", Json{{"edits", 1}, {"publishes", false}}, kColdSamples, kWarmupSamples,
              kWarmSamples,
              [&] {
                  const ParameterGestureResult result =
                      session.beginKeyedParameterGesture(500.0, gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("keyed gesture begin failed before update timing");
                  token = result.token;
              },
              [&] {
                  const ParameterGestureResult result = session.updateKeyedParameterGesture(token, gestureEdit);
                  if (!result.snapshot)
                      throw std::runtime_error("keyed gesture update failed during timing");
              },
              {}, [&] { static_cast<void>(session.cancelParameterGesture(token)); });
    measureOp(name, "gesture.keyed_commit",
              Json{{"edits", 1},
                   {"publishes", true},
                   {"history_entries", 1},
                   {"history_depth_at_start", retainedDepth(session)},
                   {"committed_entry_undone_between_runs", true}},
              kColdSamples, kWarmupSamples, kWarmSamples,
              [&] {
                  const ParameterGestureResult result =
                      session.beginKeyedParameterGesture(500.0, gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("keyed gesture begin failed before commit timing");
                  token = result.token;
              },
              [&] {
                  const EditResult result =
                      session.commitParameterGesture(token, EditOptions{session.revision(), {}});
                  if (!result.committed)
                      throw std::runtime_error("keyed gesture commit failed during timing");
              },
              [&] {
                  static_cast<void>(session.undo(EditOptions{session.revision(), {}}));
                  const ParameterGestureResult result =
                      session.beginKeyedParameterGesture(500.0, gestureEdit, EditOptions{session.revision(), {}});
                  if (!result.snapshot)
                      throw std::runtime_error("keyed gesture begin failed before commit timing");
                  token = result.token;
              },
              [&] { static_cast<void>(session.cancelParameterGesture(token)); });
}

void runMediaOps(const Workload& workload) {
    ProjectSession& session = *workload.session;
    const std::string& name = workload.name;
    const Json notes{{"history_capacity", kDefaultHistoryCapacity},
                     {"media_entries", session.document().mediaCatalog().entries().size()},
                     {"media_bins", session.document().mediaCatalog().bins().size()},
                     {"history_depth_at_start", retainedDepth(session)},
                     {"samples_undone_between_runs", true}};
    const auto undoLast = [&] { static_cast<void>(session.undo(EditOptions{session.revision(), {}})); };

    std::size_t labelCounter = 0;
    measureOp(name, "submit.set_media_metadata", notes, kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
        MediaMetadata metadata = session.document().mediaCatalog().entry(workload.mediaEntry)->metadata;
        // Committed probe data is owned by commitMediaProbeCommand; the generic
        // metadata command rejects it.
        metadata.committedProbe.reset();
        metadata.label = "label-" + std::to_string(labelCounter++);
        submitOk(session, setMediaMetadataCommand(workload.mediaEntry, metadata));
    },
              undoLast);

    std::size_t markCounter = 0;
    measureOp(name, "submit.set_media_marks", notes, kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
        const std::int64_t shift = static_cast<std::int64_t>(markCounter++ % 2);
        submitOk(session, setMediaMarksCommand(workload.mediaEntry, {MediaMarkRange{shift, 48 + shift, {}},
                                                                     MediaMarkRange{96 + shift, 144 + shift, {}}}));
    },
              undoLast);

    std::size_t moveCounter = 0;
    measureOp(name, "submit.move_media", notes, kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
        const MediaBinId target = (moveCounter++ % 2 == 0) ? workload.mediaBinA : workload.mediaBinB;
        submitOk(session, moveMediaCommand(workload.mediaEntry, target));
    },
              undoLast);

    // Import one catalog entry from an already-registered source; the untimed
    // harness undoes it so every sample starts from the declared catalog.
    const std::string importSource = "media/import_target.exr";
    {
        SourceReference reference;
        reference.path = importSource;
        submitOk(session, setSourceCommand(importSource, reference));
    }
    measureOp(name, "submit.import_media_reference",
              Json{{"history_capacity", kDefaultHistoryCapacity},
                   {"source_registered_outside_timer", true},
                   {"history_depth_at_start", retainedDepth(session)},
                   {"samples_undone_between_runs", true}},
              kColdSamples, kWarmupSamples, kWarmSamples, {}, [&] {
                  submitOk(session, importMediaReferenceCommand(importSource, workload.mediaBinA, MediaMetadata{},
                                                                std::make_shared<MediaSourceId>(kInvalidMediaSource)));
              },
              [&] { static_cast<void>(session.undo(EditOptions{session.revision(), {}})); });
}

// Builds a workload through submitted commands, then re-seats the session on
// the identical document with the requested history capacity, so the retained
// history baseline is empty and phase deltas are attributable to the measured
// entries. Population itself stays on the real command path.
[[nodiscard]] Workload seatWorkload(const std::string& name, std::size_t historyCapacity, Workload built) {
    Document document = built.session->snapshot();
    built.session = std::make_unique<ProjectSession>(std::move(document), historyCapacity);
    built.name = name;
    return built;
}

// -------------------------------------------------------------- workload runs

void runTimingsWorkload(Workload workload) {
    note("timings: " + workload.name);
    // Re-seat on the identical document with an empty history baseline, so each
    // measured submit starts from a declared document and a declared history
    // depth; accumulated-history behaviour is measured by the history variants.
    const std::string workloadName = workload.name;
    workload = seatWorkload(workloadName, kDefaultHistoryCapacity, std::move(workload));
    emitWorkload(workload, kDefaultHistoryCapacity);
    runCommonOps(workload);
    if (workload.graphOps)
        runGraphOps(workload);
    if (!workload.targets.empty())
        runParameterOps(workload);
    if (workload.animationOps)
        runAnimationOps(workload);
    if (workload.mediaOps)
        runMediaOps(workload);
}

void runHistoryWorkload(const std::string& name, std::size_t historyCapacity, std::size_t commits) {
    note("timings: " + name);
    // Re-seat on the identical document so the depth window starts at depth 1.
    Workload workload = seatWorkload(name, historyCapacity, buildChainWorkload(name + ".populate", 16, 64));
    ProjectSession& session = *workload.session;
    emitWorkload(workload, historyCapacity);

    std::vector<double> samples;
    samples.reserve(commits);
    std::vector<std::size_t> depths;
    depths.reserve(commits);
    std::size_t counter = 0;
    for (std::size_t i = 0; i < commits; ++i) {
        const EditTarget& target = workload.targets[i % workload.targets.size()];
        const auto start = Clock::now();
        submitOk(session, setParamCommand(target.network, target.node, target.key, cycleValue(target.kind, counter++)));
        samples.push_back(microsSince(start));
        depths.push_back(std::min<std::size_t>(i + 1, historyCapacity));
    }
    const std::size_t window = std::min<std::size_t>(64, commits);
    std::vector<double> windowSamples(samples.end() - static_cast<std::ptrdiff_t>(window), samples.end());
    emit(Json{{"record", "op"},
              {"variant", name},
              {"op", "submit.set_param"},
              {"notes", Json{{"history_capacity", historyCapacity},
                             {"commits", commits},
                             {"depth_start", depths.front()},
                             {"depth_end", depths.back()},
                             {"cold_warm_split", "not applicable: accumulation is confounded with history depth"}}},
              {"cold", statsJson(statsOf(samples))},
              {"warm", Json::object()}});
    emit(Json{{"record", "op"},
              {"variant", name},
              {"op", "submit.set_param.depth_window"},
              {"notes", Json{{"history_capacity", historyCapacity},
                             {"window_commits", window},
                             {"depth_start", depths[commits - window]},
                             {"depth_end", depths.back()}}},
              {"cold", statsJson(statsOf(windowSamples))},
              {"warm", Json::object()}});

    // Observe the retained depth (untimed), restore it, then time the undo and
    // redo sequences separately.
    std::size_t observed = 0;
    while (session.canUndo()) {
        if (!session.undo(EditOptions{session.revision(), {}}).committed)
            break;
        ++observed;
        if (observed > historyCapacity + commits + 16)
            break;
    }
    for (std::size_t i = 0; i < observed; ++i) {
        if (!session.redo(EditOptions{session.revision(), {}}).committed)
            throw std::runtime_error("history restore redo failed");
    }
    const std::size_t undoRun = observed;
    std::vector<double> undoSamples;
    undoSamples.reserve(undoRun);
    for (std::size_t i = 0; i < undoRun; ++i) {
        const auto start = Clock::now();
        const EditResult result = session.undo(EditOptions{session.revision(), {}});
        undoSamples.push_back(microsSince(start));
        if (!result.committed)
            throw std::runtime_error("history undo failed during timing");
    }
    std::vector<double> redoSamples;
    redoSamples.reserve(undoRun);
    for (std::size_t i = 0; i < undoRun; ++i) {
        const auto start = Clock::now();
        const EditResult result = session.redo(EditOptions{session.revision(), {}});
        redoSamples.push_back(microsSince(start));
        if (!result.committed)
            throw std::runtime_error("history redo failed during timing");
    }
    emit(Json{{"record", "op"},
              {"variant", name},
              {"op", "undo_sequence"},
              {"notes", Json{{"history_capacity", historyCapacity},
                             {"observed_retained_entries", observed},
                             {"timed_entries", undoRun},
                             {"cold_warm_split", "not applicable: one ordered sequence"}}},
              {"cold", statsJson(statsOf(undoSamples))},
              {"warm", Json::object()}});
    emit(Json{{"record", "op"},
              {"variant", name},
              {"op", "redo_sequence"},
              {"notes", Json{{"history_capacity", historyCapacity}, {"timed_entries", undoRun}}},
              {"cold", statsJson(statsOf(redoSamples))},
              {"warm", Json::object()}});
    emit(Json{{"record", "history_observation"},
              {"variant", name},
              {"history_capacity", historyCapacity},
              {"commits", commits},
              {"observed_retained_entries", observed}});
}

// ----------------------------------------------------------------- retention

void emitRetention(const std::string& phase, const Json& notes, unsigned long long before, unsigned long long after) {
    emit(Json{{"record", "retention"},
              {"phase", phase},
              {"notes", notes},
              {"heap_before_bytes", before},
              {"heap_after_bytes", after},
              {"delta_bytes", static_cast<long long>(after) - static_cast<long long>(before)}});
}

void runRetention() {
    note("retention: glibc mallinfo2 in-use heap attribution");
    const unsigned long long start = heapInUse();
    const std::size_t edits = 384;

    {
        Workload workload =
            seatWorkload("history.below", kDefaultHistoryCapacity, buildChainWorkload("retention.base", 16, 64));
        ProjectSession& session = *workload.session;
        const Json baseNotes{{"history_capacity", kDefaultHistoryCapacity},
                             {"population_history_cleared", true},
                             {"composition", compositionOf(session)}};
        const unsigned long long base = heapInUse();
        emitRetention("populated_base_document", baseNotes, start, base);

        {
            const unsigned long long before = heapInUse();
            Document kept = session.snapshot();
            const unsigned long long retained = heapInUse();
            emitRetention("one_document_snapshot_retained", Json{{"snapshots", 1}}, before, retained);
            kept = Document{};
            const unsigned long long released = heapInUse();
            emitRetention("one_document_snapshot_released", Json{{"snapshots", 0}}, retained, released);
        }

        std::size_t counter = 0;
        const auto pushEdits = [&](std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                const EditTarget& target = workload.targets[i % workload.targets.size()];
                submitOk(session,
                         setParamCommand(target.network, target.node, target.key, cycleValue(target.kind, counter++)));
            }
        };
        // Growth curve in 64-edit blocks through accumulation and eviction.
        unsigned long long previous = base;
        for (std::size_t block = 0; block * 64 < edits; ++block) {
            pushEdits(64);
            const unsigned long long now = heapInUse();
            const std::size_t commits = (block + 1) * 64;
            emitRetention("history_block",
                          Json{{"history_capacity", kDefaultHistoryCapacity},
                               {"commits", commits},
                               {"retained", std::min<std::size_t>(commits, kDefaultHistoryCapacity)},
                               {"block_bytes", static_cast<long long>(now) - static_cast<long long>(previous)}},
                          previous, now);
            previous = now;
        }
        std::size_t undoCount = 0;
        while (session.canUndo() && undoCount < 1000) {
            if (!session.undo(EditOptions{session.revision(), {}}).committed)
                break;
            ++undoCount;
        }
        emit(Json{{"record", "history_bound_observation"},
                  {"variant", "history.below"},
                  {"history_capacity", kDefaultHistoryCapacity},
                  {"commits", edits},
                  {"observed_retained_entries", undoCount}});
    }

    // A zero-capacity session isolates the bookkeeping that is not retained
    // undo snapshots (bounded event journal; request dedup is skipped for empty
    // request ids).
    {
        Workload workload = seatWorkload("history.capacity0", 0, buildChainWorkload("retention.base", 16, 64));
        ProjectSession& session = *workload.session;
        const unsigned long long base = heapInUse();
        emitRetention("capacity0_populated",
                      Json{{"history_capacity", 0}, {"composition", compositionOf(session)}}, start, base);
        std::size_t counter = 0;
        for (std::size_t i = 0; i < 64; ++i) {
            const EditTarget& target = workload.targets[i % workload.targets.size()];
            submitOk(session,
                     setParamCommand(target.network, target.node, target.key, cycleValue(target.kind, counter++)));
        }
        const unsigned long long afterEdits = heapInUse();
        emitRetention("capacity0_depth_64_no_history", Json{{"history_capacity", 0}, {"entries", 64}}, base,
                      afterEdits);
    }

    // Transient parameter-gesture preview snapshot.
    {
        Workload workload = seatWorkload("gesture_preview", kDefaultHistoryCapacity, buildEditSurface("preview", 16));
        ProjectSession& session = *workload.session;
        const unsigned long long base = heapInUse();
        emitRetention("gesture_preview_populated", Json{{"history_capacity", kDefaultHistoryCapacity}}, start, base);
        const EditTarget& target = workload.targets.front();
        const std::vector<ParameterEdit> edit{
            ParameterEdit{ParameterAddress{target.network, target.node, target.key}, cycleValue(target.kind, 3)}};
        const ParameterGestureToken token =
            session.beginParameterGesture(edit, EditOptions{session.revision(), {}}).token;
        const unsigned long long duringBegin = heapInUse();
        emitRetention("gesture_preview_begin_retained", Json{{"snapshots", 1}}, base, duringBegin);
        for (int i = 0; i < 8; ++i)
            static_cast<void>(session.updateParameterGesture(token, edit));
        const unsigned long long duringUpdates = heapInUse();
        emitRetention("gesture_preview_after_updates", Json{{"updates", 8}}, duringBegin, duringUpdates);
        static_cast<void>(session.cancelParameterGesture(token));
        const unsigned long long afterCancel = heapInUse();
        emitRetention("gesture_preview_after_cancel", Json{{"released", true}}, duringUpdates, afterCancel);
    }

    // Prepared-save snapshot. The prepared write retains a stable document
    // version and the envelope fields; it no longer serializes the project.
    {
        Workload workload = seatWorkload("prepared_save", kDefaultHistoryCapacity, buildEditSurface("save", 16));
        ProjectSession& session = *workload.session;
        const unsigned long long base = heapInUse();
        emitRetention("prepared_save_populated", Json{{"history_capacity", kDefaultHistoryCapacity}}, start, base);
        ProjectWriteRequest request = session.prepareSave("/tmp/issue69-driver/out.nemo", PathPolicy::KeepStored, false);
        const unsigned long long prepared = heapInUse();
        emitRetention("prepared_save_snapshot", Json{{"serialized_baseline", false}}, base, prepared);
        request.snapshot.reset();
        const unsigned long long dropped = heapInUse();
        emitRetention("prepared_save_request_released", Json{{"released", true}}, prepared, dropped);
    }

    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    emit(Json{{"record", "process_memory"},
              {"max_rss_kib", static_cast<unsigned long long>(usage.ru_maxrss)},
              {"note", "whole process peak RSS; not retained history bytes"}});
}

// --------------------------------------------------------------- correctness

struct CheckRunner {
    std::size_t passed{0};
    std::size_t failed{0};

    void check(const std::string& name, bool ok, const Json& detail) {
        if (ok)
            ++passed;
        else
            ++failed;
        emit(Json{{"record", "correctness"}, {"check", name}, {"ok", ok}, {"detail", detail}});
    }
};

// Authored content with identity high-water marks removed: undo/redo restore
// content, while watermarks legitimately stay monotonic (preserved forward).
[[nodiscard]] Json contentOf(const Document& document) {
    Json json = saveDocument(document);
    for (const char* key : {"nextNetworkId", "nextInstanceId", "nextAnimationChannelId", "nextKeyframeId"})
        json.erase(key);
    if (json.contains("mediaCatalog")) {
        json["mediaCatalog"].erase("nextEntryId");
        json["mediaCatalog"].erase("nextBinId");
    }
    for (auto& network : json["networks"]) {
        network.erase("nextNodeId");
        network.erase("nextEdgeId");
        network.erase("nextInterfacePortId");
    }
    return json;
}

[[nodiscard]] Json watermarksOf(const Document& document) {
    return Json{{"nextNetworkId", document.nextNetworkId()},
                {"nextInstanceId", document.nextInstanceId()},
                {"nextMediaSourceId", document.nextMediaSourceId()},
                {"nextMediaBinId", document.nextMediaBinId()},
                {"nextAnimationChannelId", document.nextAnimationChannelId()},
                {"nextKeyframeId", document.nextKeyframeId()}};
}

[[nodiscard]] bool watermarksMonotonic(const Json& before, const Json& after) {
    for (const char* key : {"nextNetworkId", "nextInstanceId", "nextMediaSourceId", "nextMediaBinId",
                            "nextAnimationChannelId", "nextKeyframeId"}) {
        if (after.at(key).get<std::uint64_t>() < before.at(key).get<std::uint64_t>())
            return false;
    }
    return true;
}

void runCorrectness() {
    CheckRunner checks;
    const NetworkId network = 1;

    // Preview begin/update publish no revision and add no history.
    {
        Workload workload = buildParameterWorkload("correctness.preview", 2, 0);
        ProjectSession& session = *workload.session;
        const EditTarget& target = workload.targets.front();
        const std::vector<ParameterEdit> edit{
            ParameterEdit{ParameterAddress{target.network, target.node, target.key}, cycleValue(target.kind, 5)}};
        const std::size_t entriesBefore = retainedDepth(session);
        const std::uint64_t revisionBefore = session.revision();
        const bool canUndoBefore = session.canUndo();
        const Json contentBefore = contentOf(session.document());
        const ParameterGestureToken token =
            session.beginParameterGesture(edit, EditOptions{session.revision(), {}}).token;
        const bool contentUnchangedDuringBegin = contentOf(session.document()) == contentBefore;
        static_cast<void>(session.updateParameterGesture(token, edit));
        const bool publishedContentUnchanged = contentBefore == contentOf(session.document());
        checks.check("preview_begin_update_publish_nothing",
                     session.revision() == revisionBefore && session.canUndo() == canUndoBefore &&
                         contentUnchangedDuringBegin && publishedContentUnchanged,
                     Json{{"revision_before", revisionBefore},
                          {"revision_after_update", session.revision()},
                          {"can_undo_before", canUndoBefore},
                          {"can_undo_after_update", session.canUndo()},
                          {"content_unchanged", publishedContentUnchanged}});

        static_cast<void>(session.cancelParameterGesture(token));
        checks.check("preview_cancel_publishes_nothing",
                     session.revision() == revisionBefore && session.canUndo() == canUndoBefore &&
                         contentBefore == contentOf(session.document()),
                     Json{{"revision", session.revision()}, {"can_undo", session.canUndo()}});

        const ParameterGestureToken commitToken =
            session.beginParameterGesture(edit, EditOptions{session.revision(), {}}).token;
        static_cast<void>(session.updateParameterGesture(commitToken, edit));
        const EditResult committed = session.commitParameterGesture(commitToken, EditOptions{session.revision(), {}});
        const Json contentAfterCommit = contentOf(session.document());
        const std::size_t entriesAfterCommit = retainedDepth(session);
        const EditResult undoResult = session.undo(EditOptions{session.revision(), {}});
        const Json contentAfterUndo = contentOf(session.document());
        checks.check("gesture_commit_one_entry",
                     committed.committed && undoResult.committed && entriesAfterCommit == entriesBefore + 1 &&
                         contentAfterUndo == contentBefore && contentAfterCommit != contentBefore,
                     Json{{"committed", committed.committed},
                          {"changed_node_ids", committed.changedNodeIds.size()},
                          {"history_entries_before", entriesBefore},
                          {"history_entries_after_commit", entriesAfterCommit},
                          {"undo_restored_content", contentAfterUndo == contentBefore}});
        checks.check("gesture_commit_changed_identity_list",
                     committed.changedNodeIds.size() == 1 && committed.createdNodeIds.empty() &&
                         committed.changedNodeIds.front().id == target.node &&
                         committed.changedAnimationChannelIds.empty(),
                     Json{{"changed_node_ids", committed.changedNodeIds.size()},
                          {"created_node_ids", committed.createdNodeIds.size()}});
    }

    // Stale and rejected edits publish nothing and leave history untouched.
    {
        Workload workload = buildParameterWorkload("correctness.rejected", 2, 0);
        ProjectSession& session = *workload.session;
        const EditTarget& target = workload.targets.front();
        const std::size_t entriesBefore = retainedDepth(session);
        const std::uint64_t revisionBefore = session.revision();
        const Json contentBefore = contentOf(session.document());
        const EditResult stale =
            session.submit(setParamCommand(target.network, target.node, target.key, cycleValue(target.kind, 9)),
                           EditOptions{revisionBefore + 7, {}});
        const EditResult unknown =
            session.submit(setParamCommand(target.network, 9999, "gain", cycleValue(target.kind, 9)),
                           EditOptions{session.revision(), {}});
        const std::uint64_t revisionAfter = session.revision();
        const Json contentAfter = contentOf(session.document());
        const std::size_t entriesAfter = retainedDepth(session);
        checks.check("stale_and_rejected_publish_nothing",
                     !stale.committed && stale.error && stale.error->code == EditErrorCode::RevisionConflict &&
                         !unknown.committed && unknown.error && entriesAfter == entriesBefore &&
                         revisionAfter == revisionBefore && contentAfter == contentBefore,
                     Json{{"stale_code", stale.error ? static_cast<int>(stale.error->code) : -1},
                          {"stale_graph_error", stale.error && stale.error->graphError.has_value()},
                          {"unknown_code", unknown.error ? static_cast<int>(unknown.error->code) : -1},
                          {"unknown_graph_error", unknown.error && unknown.error->graphError.has_value()},
                          {"revision_before", revisionBefore},
                          {"revision_after", revisionAfter},
                          {"history_entries_before", entriesBefore},
                          {"history_entries_after", entriesAfter},
                          {"content_unchanged", contentAfter == contentBefore}});
    }

    // Undo/redo of a declared mixed edit restores authored content and keeps
    // identity high-water marks monotonic.
    {
        ProjectSession session;
        const NodeId node = *addNode(session, network, "grade", "g");
        const NodeId source = *addNode(session, network, "constcolor", "c");
        const EditResult connected =
            submitOk(session, connectCommand(network, PortRef{source, 0}, PortRef{node, 0}));
        submitOk(session,
                 setParamCommand(network, node, "gain", ParameterValue{ColorValue{{0.5F, 0.5F, 0.5F, 1.0F}}}));
        const Json contentAfter = contentOf(session.document());
        const Json watermarksBefore = watermarksOf(session.document());
        const EditResult undone = session.undo(EditOptions{session.revision(), {}});
        const Json contentUndone = contentOf(session.document());
        const Json watermarksUndone = watermarksOf(session.document());
        const EditResult redone = session.redo(EditOptions{session.revision(), {}});
        const Json contentRedone = contentOf(session.document());
        const Json watermarksRedone = watermarksOf(session.document());
        checks.check("undo_redo_restore_content_and_monotonic_watermarks",
                     undone.committed && redone.committed && contentRedone == contentAfter &&
                         contentUndone != contentAfter &&
                         watermarksMonotonic(watermarksBefore, watermarksUndone) &&
                         watermarksMonotonic(watermarksUndone, watermarksRedone),
                     Json{{"redo_matches_committed_document", contentRedone == contentAfter},
                          {"undo_differs_from_committed_document", contentUndone != contentAfter},
                          {"watermarks_after_undo", watermarksUndone},
                          {"watermarks_after_redo", watermarksRedone}});
        checks.check("connect_changed_identity_list",
                     connected.createdEdgeIds.size() == 1 && connected.createdNodeIds.empty() &&
                         connected.changedEdgeIds.size() == 1,
                     Json{{"created_edge_ids", connected.createdEdgeIds.size()},
                          {"changed_edge_ids", connected.changedEdgeIds.size()}});
    }

    // Add-node and transaction identity lists; a transaction is one entry.
    {
        ProjectSession session;
        const EditResult added = submitOk(session, addNodeCommand(network, "blur", "b"));
        const std::size_t nodesAfterAdd = session.document().network(network).graph().nodes().size();
        std::vector<Command> pair;
        pair.push_back(addNodeCommand(network, "constcolor", "c1"));
        pair.push_back(addNodeCommand(network, "constcolor", "c2"));
        const EditResult transaction = submitOk(session, transactionCommand("pair", std::move(pair)));
        const EditResult transactionUndo = session.undo(EditOptions{session.revision(), {}});
        const bool oneEntry =
            transactionUndo.committed && session.document().network(network).graph().nodes().size() == nodesAfterAdd;
        const EditResult transactionRedo = session.redo(EditOptions{session.revision(), {}});
        if (!transactionRedo.committed)
            throw std::runtime_error("transaction redo failed");
        std::vector<Command> edge;
        edge.push_back(connectCommand(network, PortRef{transaction.createdNodeIds[0].id, 0},
                                      PortRef{added.createdNodeIds.front().id, 0}));
        const EditResult edgeTransaction = submitOk(session, transactionCommand("edge", std::move(edge)));
        checks.check("changed_created_identity_lists",
                     added.createdNodeIds.size() == 1 && added.changedNodeIds.size() == 1 &&
                         added.createdNodeIds.front().id == added.changedNodeIds.front().id &&
                         transaction.createdNodeIds.size() == 2 && transaction.createdEdgeIds.empty() && oneEntry &&
                         edgeTransaction.createdEdgeIds.size() == 1 && edgeTransaction.changedEdgeIds.size() == 1,
                     Json{{"add_created", added.createdNodeIds.size()},
                          {"add_changed", added.changedNodeIds.size()},
                          {"transaction_created_nodes", transaction.createdNodeIds.size()},
                          {"transaction_one_history_entry", oneEntry},
                          {"edge_transaction_created_edges", edgeTransaction.createdEdgeIds.size()}});
    }

    // Request deduplication replays the original result without a second entry,
    // even when the retry carries a stale expected revision.
    {
        ProjectSession session;
        const NodeId grade = *addNode(session, network, "grade", "g");
        const std::size_t entriesBefore = retainedDepth(session);
        const Command command =
            setParamCommand(network, grade, "gain", ParameterValue{ColorValue{{0.25F, 0.25F, 0.25F, 1.0F}}});
        const EditResult first = session.submit(command, EditOptions{session.revision(), "edit-1"});
        const EditResult replay = session.submit(command, EditOptions{999999, "edit-1"});
        const std::size_t entriesAfter = retainedDepth(session);
        const EditResult firstUndo = session.undo(EditOptions{session.revision(), {}});
        checks.check("request_dedup_replays_without_history",
                     first.committed && replay.committed && first.revision == replay.revision && firstUndo.committed &&
                         entriesAfter == entriesBefore + 1,
                     Json{{"first_revision", first.revision},
                          {"replay_revision", replay.revision},
                          {"replay_with_stale_revision_committed", replay.committed},
                          {"history_entries_before", entriesBefore},
                          {"history_entries_after_replay", entriesAfter}});
    }

    // A keyed gesture commit adds exactly one entry; a keyed cancel adds none.
    {
        Workload workload = buildAnimationWorkload("correctness.animation", 2, 1, 4);
        ProjectSession& session = *workload.session;
        const std::uint64_t revisionBefore = session.revision();
        const auto keyCount = [&] {
            std::size_t keys = 0;
            for (const auto& channel : session.document().animationChannels())
                keys += channel.keys.size();
            return keys;
        };
        const std::size_t keysBefore = keyCount();
        const std::vector<ParameterEdit> edit{
            ParameterEdit{workload.animationAddress, ParameterValue{ColorValue{{0.3F, 0.3F, 0.3F, 1.0F}}}}};
        const ParameterGestureToken cancelToken =
            session.beginKeyedParameterGesture(900.0, edit, EditOptions{session.revision(), {}}).token;
        static_cast<void>(session.updateKeyedParameterGesture(cancelToken, edit));
        static_cast<void>(session.cancelParameterGesture(cancelToken));
        const std::size_t keysAfterCancel = keyCount();
        const ParameterGestureToken commitToken =
            session.beginKeyedParameterGesture(900.0, edit, EditOptions{session.revision(), {}}).token;
        const EditResult committed = session.commitParameterGesture(commitToken, EditOptions{session.revision(), {}});
        const std::size_t keysAfterCommit = keyCount();
        checks.check("keyed_gesture_cancel_and_commit",
                     session.revision() == revisionBefore + 1 && keysAfterCancel == keysBefore &&
                         keysAfterCommit == keysBefore + 1 && committed.committed &&
                         committed.changedAnimationChannelIds.size() == 1,
                     Json{{"keys_before", keysBefore},
                          {"keys_after_cancel", keysAfterCancel},
                          {"keys_after_commit", keysAfterCommit},
                          {"changed_channel_ids", committed.changedAnimationChannelIds.size()}});
    }

    // Media catalog import reports the created entry identity.
    {
        Workload workload = buildEditSurface("correctness.media", 1);
        ProjectSession& session = *workload.session;
        const std::string sourceKey = "media/correctness.exr";
        SourceReference reference;
        reference.path = sourceKey;
        submitOk(session, setSourceCommand(sourceKey, reference));
        auto created = std::make_shared<MediaSourceId>(kInvalidMediaSource);
        const EditResult imported =
            submitOk(session, importMediaReferenceCommand(sourceKey, MediaMetadata{}, created));
        checks.check("media_import_created_identity_list",
                     imported.createdMediaEntryIds.size() == 1 && imported.createdMediaEntryIds.front() == *created &&
                         imported.changedMediaEntryIds.size() == 1,
                     Json{{"created", imported.createdMediaEntryIds.size()},
                          {"changed", imported.changedMediaEntryIds.size()}});
    }

    emit(Json{{"record", "correctness_summary"},
              {"passed", checks.passed},
              {"failed", checks.failed},
              {"build", kBuildName}});
    if (checks.failed != 0)
        std::exit(1);
}

// --------------------------------------------------------------------- seed

void writeSeedProject(const std::string& path, std::size_t chainNodes) {
    ProjectSession session;
    const NetworkId network = session.document().rootNetworkId();
    static_cast<void>(buildChain(session, network, chainNodes, "plate"));
    ProjectWriteRequest request = session.prepareSave(path, PathPolicy::KeepStored, false);
    const ProjectWriteResult result = ProjectFile::writeAtomic(request);
    if (!result.ok)
        throw std::runtime_error("seed write failed: " + result.error.message);
    emit(Json{{"record", "seed"}, {"path", result.target.string()}, {"nodes", chainNodes}});
}

// --------------------------------------------------------------------- main

void emitEnvironment() {
    struct timespec resolution {};
    clock_getres(CLOCK_MONOTONIC, &resolution);
    emit(Json{{"record", "environment"},
              {"build", kBuildName},
              {"compiler", __VERSION__},
              {"cplusplus", static_cast<long>(__cplusplus)},
              {"clock", "std::chrono::steady_clock"},
              {"clock_resolution_ns", static_cast<long long>(resolution.tv_sec) * 1000000000LL + resolution.tv_nsec},
              {"timer_scope", "single call of the named operation; setup/teardown and driver serialization excluded"},
              {"percentile_method", "nearest-rank p50/p95"},
              {"cold_definition",
               "first 20 executions of the operation after its setup, no warm-up for that operation"},
              {"warm_definition", "after 50 untimed executions of the same operation on the same session"},
              {"heap_attribution", "glibc mallinfo2().uordblks in-use heap bytes, phase deltas in one process"},
              {"history_capacity", kDefaultHistoryCapacity}});
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "timings") {
            emitEnvironment();
            runTimingsWorkload(buildChainWorkload("graph.chain.small", 16, 64));
            runTimingsWorkload(buildChainWorkload("graph.chain.large", 128, 512));
            runTimingsWorkload(buildFanInWorkload("graph.fanin.small", 16, 32));
            runTimingsWorkload(buildFanInWorkload("graph.fanin.large", 128, 256));
            runTimingsWorkload(buildMultiNetworkWorkload("graph.multinetwork.small", 16, 4, 64));
            runTimingsWorkload(buildMultiNetworkWorkload("graph.multinetwork.large", 128, 8, 256));
            runTimingsWorkload(buildParameterWorkload("params.small", 16, 96));
            runTimingsWorkload(buildParameterWorkload("params.large", 128, 896));
            runTimingsWorkload(buildAnimationWorkload("animation.small", 16, 16, 8));
            runTimingsWorkload(buildAnimationWorkload("animation.large", 128, 128, 32));
            runTimingsWorkload(buildMediaWorkload("media.small", 16, 4, 64));
            runTimingsWorkload(buildMediaWorkload("media.large", 128, 16, 512));
            runHistoryWorkload("history.below", 256, 64);
            runHistoryWorkload("history.at", 256, 256);
            runHistoryWorkload("history.above", 256, 384);
            return 0;
        }
        if (mode == "retention") {
            emitEnvironment();
            runRetention();
            return 0;
        }
        if (mode == "correctness") {
            runCorrectness();
            return 0;
        }
        if (mode == "seed") {
            if (argc < 4) {
                std::cerr << "usage: editing_scale_driver seed <path> <chain_nodes>\n";
                return 2;
            }
            writeSeedProject(argv[2], static_cast<std::size_t>(std::stoul(argv[3])));
            return 0;
        }
        std::cerr << "usage: editing_scale_driver <timings|retention|correctness|seed <path> <nodes>>\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "issue69-driver: fatal: " << error.what() << '\n';
        return 1;
    }
}
