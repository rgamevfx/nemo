// Explicit delivery jobs (issue #94, stories 74-86) — the native orchestration
// seam.
//
// Ownership boundary (issue #94 continuation, #97 under #14): producing pixels
// is NOT this file's job. A job evaluates its frames through `evaluateGpu` over
// a retained `SourceSession` — the same native decode/effect path the desktop
// viewer runs, at full quality (`Quality::Full` has no reduced equivalent on the
// native path, and the viewer cache is deliberately never consulted: it is
// permitted to be lossy, so it must never satisfy a delivery — story 79) — and
// the final device-to-host transfer goes through `gpu::ExportStaging`, charged
// to the application's own allocator and retained by its own submission queue.
// There is no CPU evaluation fallback, no second decoder, no private device and
// no diagnostic readback.
//
// Atomic finalization (story 82): an EXR frame is written to a hidden temporary
// beside its final path (same directory, same extension, unique across jobs and
// processes) and only then published — with a hard link when overwriting was not
// authorized, so a file that appeared while the job ran can never be replaced,
// and with an atomic rename when the caller did authorize replacement. A movie
// has ONE output path: its container is written to a hidden temporary and
// published only after the whole range encoded successfully, so a cancelled or
// failed movie leaves nothing at its final path.
//
// Threading contract: `jobs()`, `status()`, `cancel()`, `forget()` and the
// `DeliveryJobInfo` copies they return are safe from any thread, including the
// UI event thread; the worker mutates job records only under the queue mutex, so
// a caller never observes a half-updated frame. `waitForIdle()` is for tests and
// one-shot CLI use — the UI never waits on the event thread.

#include "nemo/eval/DeliveryJob.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/CpuReference.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/core/evaluation/NodeContributions.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/gpu/ExportStaging.hpp"
#include "nemo/media/DeliveryOutput.hpp"
#include "nemo/media/ImageIO.hpp"

namespace nemo::eval {

namespace {

// The largest frame range ONE job may request. A delivery rasterizes one frame
// at a time and keeps one result record per frame, so this bound keeps a single
// accepted job's retained state — and the report a caller reads back — finite.
// It is a refusal with an explicit message, never a truncation.
constexpr std::int64_t kMaxFramesPerJob = 10000;

// Native evaluation and the export staging transfer are worker-side and
// off-thread, so their timeouts are generous: they bound a wedged device, not
// interaction latency.
constexpr std::uint64_t kEvaluationTimeoutNs = 60'000'000'000ULL;
constexpr std::uint64_t kTransferTimeoutNs = 60'000'000'000ULL;

[[nodiscard]] const char* roleName(const NodeRole role) {
    switch (role) {
    case NodeRole::Image:
        return "image";
    case NodeRole::Source:
        return "source";
    case NodeRole::Output:
        return "output";
    case NodeRole::Viewer:
        return "viewer";
    case NodeRole::Delivery:
        return "delivery";
    }
    return "image";
}

[[nodiscard]] bool equalsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lowered = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lowered(left[index]) != lowered(right[index])) {
            return false;
        }
    }
    return true;
}

// A movie format delivers ONE container for the whole range.
[[nodiscard]] bool isMovieType(const std::string& fileType) {
    return equalsIgnoreCase(fileType, "mov") || equalsIgnoreCase(fileType, "mp4");
}

// The choices a parameter declares, as message text: the registered Write
// descriptor owns the delivery inventory (its parameter choices), so a refusal
// names the declared values instead of keeping a second copy of the list.
[[nodiscard]] std::string declaredChoices(const NodeCatalog& catalog, const std::string& type, const char* key) {
    const ParameterSpec* spec = catalog.parameterSpec(type, key);
    if (spec == nullptr) {
        return {};
    }
    std::string text;
    for (const std::string& choice : spec->choices) {
        if (!text.empty()) {
            text += ", ";
        }
        text += choice;
    }
    return text;
}

[[nodiscard]] bool isDeclaredChoice(const NodeCatalog& catalog, const std::string& type, const char* key,
                                    const std::string& value) {
    const ParameterSpec* spec = catalog.parameterSpec(type, key);
    return spec != nullptr && std::find(spec->choices.begin(), spec->choices.end(), value) != spec->choices.end();
}

// The frame's own identity in every per-frame message: the document frame it
// evaluates, the file frame it numbers and the file it would produce. An error
// that names only "a frame" is not actionable (stories 75, 81).
[[nodiscard]] std::string frameText(const DeliveryFrame& frame) {
    return "frame " + std::to_string(frame.documentFrame) + " (file frame " + std::to_string(frame.fileFrame) + ", '" +
           frame.path + "')";
}

[[nodiscard]] std::string pathListText(const std::vector<std::string>& paths) {
    std::string text;
    for (const std::string& path : paths) {
        if (!text.empty()) {
            text += ", ";
        }
        text += "'" + path + "'";
    }
    return text;
}

[[nodiscard]] std::string channelListText(const std::vector<std::string>& channels) {
    std::string text;
    for (const std::string& channel : channels) {
        if (!text.empty()) {
            text += ", ";
        }
        text += channel;
    }
    return text;
}

// The primary roles one authored channel selection names (issue #102), or
// nothing when the value is outside the inventory: the node's declared choices
// are the authority, exactly as they are for precision and compression.
// `all` names no role explicitly — an empty set means every channel the
// delivered image carries, which is exactly what a delivery wrote before the
// selection existed, so the default is behavior-preserving.
[[nodiscard]] std::optional<std::vector<std::size_t>> channelRolesFor(const std::string& selection) {
    if (selection == "all")
        return std::vector<std::size_t>{};
    if (selection == "rgb")
        return std::vector<std::size_t>{0, 1, 2};
    if (selection == "rgba")
        return std::vector<std::size_t>{0, 1, 2, 3};
    if (selection == "alpha")
        return std::vector<std::size_t>{3};
    return std::nullopt;
}

// The channels one selection delivers, by the target's OWN names (issue #102),
// plus the selected roles the image carries no channel for. A stored channel
// name is the media's own — "R", or "rgba.R" inside a layer — so the selection
// FILTERS the described list through the shared primary-role rule instead of
// inventing a spelling, and both the delivered names and their order stay the
// described ones, which is the order the evaluated raster carries them in.
struct ChannelSelection {
    std::vector<std::string> names;
    std::vector<std::size_t> missing;
};

[[nodiscard]] ChannelSelection selectDeliveredChannels(const std::vector<std::string>& described,
                                                       const std::vector<std::size_t>& roles) {
    ChannelSelection selection;
    const auto indices = rgbaChannelIndices(described);
    for (std::size_t index = 0; index < described.size(); ++index) {
        for (const std::size_t role : roles) {
            if (indices[role] == static_cast<int>(index)) {
                selection.names.push_back(described[index]);
                break;
            }
        }
    }
    for (const std::size_t role : roles) {
        if (indices[role] < 0)
            selection.missing.push_back(role);
    }
    return selection;
}

// The primary role's own name, for a refusal that states what the image does
// not carry.
[[nodiscard]] std::string primaryRoleName(const std::size_t role) {
    if (role == 0)
        return "R";
    if (role == 1)
        return "G";
    if (role == 2)
        return "B";
    return "alpha";
}

// True when a selection includes at least one primary COLOR role (R/G/B): a
// movie container carries its primary channels, so an alpha-only selection
// cannot be encoded as one.
[[nodiscard]] bool selectsPrimaryColor(const std::vector<std::size_t>& roles) {
    for (const std::size_t role : roles) {
        if (role < 3) {
            return true;
        }
    }
    return false;
}

// The delivered raster with exactly the selected channels (issue #102),
// gathered BY NAME out of the evaluated frame. The evaluator produces every
// channel its description names, so the selection is a projection of that frame
// — exactly like the viewer's own channel projection — and no sample value is
// recomputed: the frozen delivered order becomes the storage order, one
// interleaved float per channel, and the geometry, precision, pixel aspect and
// interpretation travel through untouched. A frame that already IS the delivered
// image is returned without a copy.
[[nodiscard]] CpuImage gatherDeliveredChannels(CpuImage image, const std::vector<std::string>& delivered) {
    const auto& evaluated = image.layout().channels;
    if (evaluated == delivered) {
        return image;
    }
    std::vector<std::size_t> source;
    source.reserve(delivered.size());
    for (const std::string& name : delivered) {
        const int index = channelIndex(evaluated, name);
        if (index < 0) {
            // The selection was resolved from the SAME description the raster is
            // produced for, so this is an invariant, never a user error: state it
            // instead of writing zeros under a name the frame does not carry.
            throw DeliveryException("the evaluated frame does not carry the selected channel '" + name + "'");
        }
        source.push_back(static_cast<std::size_t>(index));
    }
    ImageLayout layout = image.layout();
    layout.channels = delivered;
    CpuImage projected(std::move(layout));
    const std::size_t evaluatedStride = evaluated.size();
    const std::size_t deliveredStride = delivered.size();
    const float* from = image.data();
    float* to = projected.data();
    const std::size_t samples = static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height());
    for (std::size_t sample = 0; sample < samples; ++sample) {
        for (std::size_t channel = 0; channel < deliveredStride; ++channel) {
            to[sample * deliveredStride + channel] = from[sample * evaluatedStride + source[channel]];
        }
    }
    return projected;
}

// The node one delivery job aims at, with instance overrides and request-local
// animation already resolved through the shared effective-parameter seam.
//
// The effective node is OWNED here when an override or animation had to be
// materialized (a NodeInstance the caller would otherwise have to keep alive),
// and merely BORROWED from the document when it was not — `borrowed` points into
// the immutable snapshot the caller passed in, which outlives every use. The
// resident pointer therefore never points into this value's own storage, so the
// value is safely returnable and copyable; readers go through `node()`.
struct ResolvedDeliveryNode {
    std::optional<NodeInstance> local;
    const NodeInstance* borrowed{nullptr};
    const NodeContribution* contribution{nullptr};
    std::string networkName;

    [[nodiscard]] const NodeInstance& node() const { return local ? *local : *borrowed; }
};

// Resolves the addressed node and refuses a missing node or one whose registered
// role is not Delivery, naming the node and the network in both cases (story 72:
// a delivery can only aim at a Write node).
[[nodiscard]] ResolvedDeliveryNode resolveDeliveryNode(const Document& document, const NetworkId network,
                                                       const NodeId node, const std::int64_t localTime) {
    std::vector<ExpandedNode> expanded;
    try {
        expanded = expandDependencies(document, network, node);
    } catch (const std::exception& error) {
        // A missing network or node is reported by the core seam in its own
        // vocabulary; delivery reports it in its own, with the address it was
        // asked for.
        throw DeliveryException("network " + std::to_string(network) + " node " + std::to_string(node) + ": " +
                                error.what());
    }
    if (expanded.empty() || expanded.back().node == nullptr) {
        throw DeliveryException("network " + std::to_string(network) + " has no node " + std::to_string(node) +
                                " to deliver from");
    }
    const ExpandedNode& target = expanded.back();
    ResolvedDeliveryNode resolved;
    std::optional<NodeInstance> localNode;
    const NodeInstance* effective = resolveEffectiveNode(document, target, localNode, static_cast<double>(localTime));
    if (localNode) {
        // The materialized node is owned here; the pointer is re-resolved through
        // `node()` everywhere below so it can never go stale.
        resolved.local = std::move(localNode);
    } else {
        resolved.borrowed = effective;
    }
    resolved.networkName = document.network(network).name();
    const NodeInstance& effectiveNode = resolved.node();

    // A nested network occurrence is routing, not a pixel node: a delivery aims
    // at the Write node inside the network that owns it, which is also the state
    // its authored settings and its evaluation resolve from.
    if (effectiveNode.instance != kInvalidNetworkInstance) {
        std::string hint = "deliver the Write node inside its own network instead";
        if (const NetworkInstance* occurrence = document.instance(effectiveNode.instance); occurrence != nullptr) {
            hint = "deliver the Write node inside network '" + document.network(occurrence->definition).name() +
                   "' instead";
        }
        throw DeliveryException(describeNode(effectiveNode) + " in network '" + resolved.networkName +
                                "' is a nested network occurrence; " + hint);
    }

    const std::shared_ptr<const NodeContributions> contributions = builtinNodeContributions();
    resolved.contribution = contributions->find(effectiveNode.type);
    if (resolved.contribution == nullptr) {
        throw DeliveryException(describeNode(effectiveNode) + " has no registered implementation in network '" +
                                resolved.networkName + "'; delivery requires a Write node");
    }
    if (resolved.contribution->role != NodeRole::Delivery) {
        throw DeliveryException(describeNode(effectiveNode) + " in network '" + resolved.networkName +
                                "' is not a Write node: its registered role is '" +
                                roleName(resolved.contribution->role) + "', and delivery requires " +
                                "NodeRole::Delivery (a Write node, which is neither the network's Output nor a " +
                                "display-only Viewer)");
    }
    return resolved;
}

// Resolves every authored delivery setting through the shared typed readers: the
// effective parameters (instance overrides and animation included) are the only
// source of these values, so the frozen copy a job is accepted with is exactly
// what the node says at submit (story 83).
[[nodiscard]] DeliverySettings resolveSettings(const Document& document, const NetworkId network,
                                               const ResolvedDeliveryNode& target) {
    const NodeCatalog& catalog = document.network(network).graph().catalog();
    const NodeInstance& effective = target.node();
    const ParameterValues& params = effective.params;

    const auto integer = [&](const char* key) {
        const ParameterValue& value = effectiveParameter(catalog, effective, params, key);
        const auto* number = std::get_if<std::int64_t>(&value);
        if (number == nullptr) {
            throw DeliveryException(describeNode(effective) + ": parameter '" + key + "' must be an integer, got '" +
                                    parameterValueText(value) + "'");
        }
        return *number;
    };

    try {
        DeliverySettings settings;
        settings.file = effectiveText(catalog, effective, params, "file");
        settings.createDirectories = effectiveFlag(catalog, effective, params, "createDirectories");
        settings.overwrite = effectiveFlag(catalog, effective, params, "overwrite");
        settings.frameFirst = integer("frameFirst");
        settings.frameLast = integer("frameLast");
        settings.frameOffset = integer("frameOffset");

        media::DeliveryOutputOptions& output = settings.output;
        output.fileType = effectiveChoice(catalog, effective, params, "fileType");
        const std::string precision = effectiveChoice(catalog, effective, params, "precision");
        if (precision == "half") {
            output.precision = media::OutputPrecision::Half;
        } else if (precision == "float") {
            output.precision = media::OutputPrecision::Float32;
        } else {
            throw DeliveryException(describeNode(effective) + ": precision '" + precision +
                                    "' is not a supported output precision (supported: " +
                                    declaredChoices(catalog, effective.type, "precision") + ")");
        }
        output.compression = effectiveChoice(catalog, effective, params, "compression");
        output.profile = effectiveChoice(catalog, effective, params, "profile");
        output.frameRate = static_cast<double>(effectiveNumber(catalog, effective, params, "frameRate"));
        const std::int64_t bitrate = integer("bitrateKbps");
        if (bitrate < 0 || bitrate > std::numeric_limits<int>::max()) {
            throw DeliveryException(describeNode(effective) + ": bitrate " + std::to_string(bitrate) +
                                    " is outside the supported positive range");
        }
        output.bitrateKbps = static_cast<int>(bitrate);
        output.colorMode = effectiveChoice(catalog, effective, params, "colorMode");
        output.outputTransform = effectiveText(catalog, effective, params, "outputTransform");
        output.lutFile = effectiveText(catalog, effective, params, "lutFile");
        // The authored channel selection (issue #102), resolved into the seam's
        // own primary roles. The value is the node's, the inventory is the
        // descriptor's, and an unsupported value is refused here naming both —
        // never silently mapped onto a default.
        const std::string selection = effectiveChoice(catalog, effective, params, "channels");
        auto roles = channelRolesFor(selection);
        if (!roles) {
            throw DeliveryException(describeNode(effective) + ": channel selection '" + selection +
                                    "' is not a supported delivery selection (supported: " +
                                    declaredChoices(catalog, effective.type, "channels") + ")");
        }
        settings.channelRoles = std::move(*roles);
        return settings;
    } catch (const EvaluationException& error) {
        // The shared readers' failures are already node-identifying; delivery
        // reports them in its own vocabulary so a caller has one exception type.
        throw DeliveryException(error.what());
    }
}

// Refuses a resolved setting this build cannot honor, naming the value. The
// registered descriptor owns the inventories, and an unknown value must be
// refused here rather than handed to an encoder that silently substitutes its
// own default (stories 76-77). Cross-field rules (a profile on an EXR, a
// transform for a mode that has none, the LUT file) are the media owner's
// `validateDeliveryOutput`; this seam only checks that every authored choice is
// one the node actually declares.
[[nodiscard]] std::string settingsProblem(const NodeCatalog& catalog, const NodeInstance& node,
                                          const DeliverySettings& settings) {
    const media::DeliveryOutputOptions& output = settings.output;
    const auto reject = [&](const char* key, const std::string& value) {
        return describeNode(node) + ": " + key + " '" + value +
               "' is not a supported delivery setting (supported: " + declaredChoices(catalog, node.type, key) + ")";
    };
    if (!isDeclaredChoice(catalog, node.type, "fileType", output.fileType)) {
        return reject("file type", output.fileType);
    }
    const bool movie = isMovieType(output.fileType);
    if (!movie && settings.output.compression.empty()) {
        return describeNode(node) +
               ": no compression was chosen; the delivery writes an explicit EXR compression "
               "(supported: " +
               declaredChoices(catalog, node.type, "compression") + ")";
    }
    if (!movie && !isDeclaredChoice(catalog, node.type, "compression", output.compression)) {
        return reject("compression", output.compression);
    }
    if (equalsIgnoreCase(output.fileType, "mov") && !isDeclaredChoice(catalog, node.type, "profile", output.profile)) {
        return reject("MOV profile", output.profile);
    }
    if (!isDeclaredChoice(catalog, node.type, "colorMode", output.colorMode)) {
        return reject("color mode", output.colorMode);
    }
    // The channel selection is the seam's own vocabulary of primary roles
    // (issue #102): a caller that states one directly states a real, strictly
    // ascending set, and a movie — whose container carries its primary RGB
    // channels — cannot deliver an alpha-only selection.
    for (std::size_t index = 0; index < settings.channelRoles.size(); ++index) {
        if (settings.channelRoles[index] >= kImageChannels ||
            (index > 0 && settings.channelRoles[index] <= settings.channelRoles[index - 1])) {
            return describeNode(node) +
                   ": the delivery channel selection is not a set of distinct primary roles (R, G, B, alpha)";
        }
    }
    if (movie && !settings.channelRoles.empty() && !selectsPrimaryColor(settings.channelRoles)) {
        return describeNode(node) + ": a " + output.fileType +
               " movie carries its primary RGB channels, so an alpha-only channel selection cannot be delivered";
    }
    if (movie && !settings.channelRoles.empty() && settings.channelRoles.back() == 3 &&
        !media::deliveryStoresAlpha(output)) {
        return describeNode(node) + ": the selected " + output.fileType +
               " encoding does not store alpha; choose RGB or an alpha-capable format/profile";
    }
    return media::validateDeliveryOutput(output);
}

// The delivery raster one accepted job writes: the target's described format
// unioned with the described data bounds, so an off-format or negative data
// window is delivered where the description says it is instead of being cropped
// to the format.
[[nodiscard]] Region deliveryWindow(const ImageDescription& description) {
    return regionUnion(description.format, description.dataBounds);
}

// Describes the delivery target through the shared description seam, consumed by
// the SAME native source descriptions execution uses: `sources` is the job's own
// `SourceSession`, so a source's geometry, channel naming and frame mapping come
// from its own media owner for exactly the frame the request will read, and no
// second (CPU) decode path can describe the raster differently from what is
// delivered. `contributions` is the queue's OWN inventory (issue #37), never an
// implicit built-in assembly: an installed package's node upstream of the target
// is described by the very registration whose callback the executor renders it
// with, so a described chain and a delivered one are the same list.
[[nodiscard]] ImageDescription describeTarget(const Document& document, const NetworkId network, const NodeId node,
                                              const std::int64_t localTime, const NodeContributions& contributions,
                                              SourceDescriptionProvider* sources) {
    EvaluationRequest request;
    request.network = network;
    request.output = node;
    request.localTime = localTime;
    // Description is domain-independent: the query identifies the target and the
    // frame it describes and nothing else.
    request.region = {0, 0, 1, 1};
    request.fullWidth = 1;
    request.fullHeight = 1;
    const ImageDescriptionPlan described = describeDependencies(document, request, contributions, sources);
    if (described.order.empty()) {
        throw DeliveryException("network " + std::to_string(network) + " node " + std::to_string(node) +
                                " has no describable image to deliver");
    }
    return described.nodes.at(described.order.back().id).description;
}

// The frames one job delivers, resolved from the authored range without ever
// overflowing an int64 endpoint: the span is computed in unsigned arithmetic
// (exact for an ordered pair), the file-frame offset is range-checked before it
// is added, and iteration stops at `last` instead of incrementing past
// INT64_MAX.
struct FrameSet {
    std::vector<DeliveryFrame> frames;
    std::string problem;
};

[[nodiscard]] FrameSet resolveFrameSet(const DeliverySettings& settings, const bool movie) {
    FrameSet set;
    const std::int64_t first = settings.frameFirst;
    const std::int64_t last = settings.frameLast;
    // File-numbering offset is an EXR SEQUENCE setting: a movie is one container
    // named by the authored path, so a stale offset left on the node (the UI
    // hides it for a movie) neither renames anything nor can overflow.
    const std::int64_t offset = movie ? 0 : settings.frameOffset;
    if (last < first) {
        set.problem = "the frame range is inverted: Last (" + std::to_string(last) + ") precedes First (" +
                      std::to_string(first) + ")";
        return set;
    }
    // The unsigned difference of an ordered pair is exact even for the extremal
    // range (INT64_MIN..INT64_MAX), and the bound is applied to that distance
    // BEFORE a count is formed: adding one to the wrapped distance would be zero
    // and admit an unbounded loop.
    const std::uint64_t distance = static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first);
    if (distance >= static_cast<std::uint64_t>(kMaxFramesPerJob)) {
        set.problem = "the frame range " + std::to_string(first) + ".." + std::to_string(last) +
                      " covers more than the " + std::to_string(kMaxFramesPerJob) +
                      " frames one delivery job may request";
        return set;
    }
    set.frames.reserve(static_cast<std::size_t>(distance + 1));
    for (std::int64_t frame = first;; ++frame) {
        const bool overflows = offset > 0 ? frame > std::numeric_limits<std::int64_t>::max() - offset
                                          : frame < std::numeric_limits<std::int64_t>::min() - offset;
        if (overflows) {
            set.frames.clear();
            set.problem = "file frame numbering overflows for frame " + std::to_string(frame) + " with file offset " +
                          std::to_string(offset);
            return set;
        }
        DeliveryFrame resolved;
        resolved.documentFrame = frame;
        resolved.fileFrame = frame + offset;
        resolved.path = media::resolveFramePath(settings.file, resolved.fileFrame);
        set.frames.push_back(std::move(resolved));
        if (frame == last) {
            break;
        }
    }
    return set;
}

// One job's complete preflight: the report a caller reads plus the frozen raster
// state an accepted job executes with.
struct JobPlan {
    DeliveryPlan plan;
    Region raster;
    Region format;
    ImageDescription description;
    bool movie{false};
};

// Preflights one job without writing anything. Everything expensive or
// filesystem-visible happens here, which is why the queue runs it on its worker
// thread and reports a refusal as a failed job instead of blocking a submitter.
// `contributions` is the queue's own immutable inventory (issue #37): the raster
// this plan reports is described through exactly the registrations the job's
// frames are rendered with.
[[nodiscard]] JobPlan planJob(const Document& document, const NetworkId network, const NodeId node,
                              const DeliverySettings& settings, const std::int64_t localTime,
                              const NodeContributions& contributions, SourceDescriptionProvider* sources) {
    JobPlan preflight;
    preflight.plan.settings = settings;
    preflight.movie = isMovieType(settings.output.fileType);
    preflight.plan.movie = preflight.movie;

    const ResolvedDeliveryNode target = resolveDeliveryNode(document, network, node, localTime);
    const NodeCatalog& catalog = document.network(network).graph().catalog();
    const auto refuse = [&preflight](std::string reason) {
        if (preflight.plan.problem.empty()) {
            preflight.plan.problem = std::move(reason);
        }
    };
    refuse(settingsProblem(catalog, target.node(), settings));
    if (!preflight.plan.problem.empty())
        return preflight;

    // The frame range is resolved first: it is what the paths, the collisions
    // and the delivery window are stated for.
    FrameSet frames = resolveFrameSet(settings, preflight.movie);
    refuse(std::move(frames.problem));

    const bool patternIsEmpty = settings.file.empty();
    const bool patternNamesSequence =
        settings.file.find('#') != std::string::npos || settings.file.find('@') != std::string::npos;
    if (patternIsEmpty) {
        refuse(describeNode(target.node()) + ": no output file pattern is authored; there is nothing to deliver to");
    } else if (preflight.movie && patternNamesSequence) {
        refuse(describeNode(target.node()) + ": output pattern '" + settings.file +
               "' names a sequence, but a movie delivers ONE file; remove the '#'/'@' frame tokens");
    } else if (!preflight.movie && frames.frames.size() > 1 && !patternNamesSequence) {
        refuse(describeNode(target.node()) + ": output pattern '" + settings.file +
               "' names one still file, but the job delivers " + std::to_string(frames.frames.size()) +
               " frames; a sequence pattern needs '#' (zero-padded frame digits) or '@'");
    } else {
        // The encoder is selected from the path's extension, so a pattern naming
        // another format would deliver something other than the resolved
        // `fileType` claims.
        const std::string extension = std::filesystem::path(settings.file).extension().string();
        if (!equalsIgnoreCase(extension, "." + settings.output.fileType)) {
            refuse(describeNode(target.node()) + ": output pattern '" + settings.file + "' does not name a '." +
                   settings.output.fileType +
                   "' file; the delivery format comes from the file type and the encoder is selected from the file "
                   "extension");
        }
    }

    if (!frames.frames.empty()) {
        // A movie delivers one file, so its collision list is that one path.
        const std::size_t collisionsChecked = preflight.movie ? 1U : frames.frames.size();
        for (std::size_t index = 0; index < collisionsChecked; ++index) {
            const std::string& path = frames.frames[index].path;
            if (!path.empty() && std::filesystem::exists(std::filesystem::path(path))) {
                // Cold path by construction: a collision is the reason the plan
                // is refused, never a reason to write.
                preflight.plan.collisions.push_back(path);
            }
        }
        preflight.plan.frames = std::move(frames.frames);
    }

    // The destination directory is checked before any frame is touched: an
    // existing non-directory and a missing directory without authorization are
    // both refused by name (story 80).
    if (preflight.plan.problem.empty() && !patternIsEmpty) {
        const std::filesystem::path parent = std::filesystem::path(settings.file).parent_path();
        if (!parent.empty()) {
            const bool exists = std::filesystem::exists(parent);
            if (exists && !std::filesystem::is_directory(parent)) {
                preflight.plan.problem = "the output directory '" + parent.string() +
                                         "' exists and is not a directory; the delivered files cannot be placed there";
            } else if (!exists && !settings.createDirectories) {
                preflight.plan.problem = "the output directory '" + parent.string() +
                                         "' does not exist and creating directories was not authorized";
            }
        }
    }

    // The raster is always resolved, so a caller learns the geometry the job
    // would deliver even when a setting refuses it. It is described at the job's
    // first document frame — the frame a range starts delivering — and then
    // frozen for the whole job (a later frame whose graph describes a different
    // raster is reported per frame, never silently written at a different size).
    try {
        const ImageDescription described =
            describeTarget(document, network, node, settings.frameFirst, contributions, sources);
        preflight.description = described;
        preflight.format = described.format;
        preflight.raster = deliveryWindow(described);
        preflight.plan.width = preflight.raster.width;
        preflight.plan.height = preflight.raster.height;
        // The channels this job DELIVERS (issue #102): the authored selection
        // resolved against the target's OWN described names, or every described
        // channel when no selection is authored. The description itself stays the
        // EVALUATED frame's contract — the evaluator produces every channel it
        // names — and the delivered image is that frame projected onto these
        // channels by the worker.
        std::vector<std::string> delivered = described.channels;
        if (!settings.channelRoles.empty()) {
            const ChannelSelection selection = selectDeliveredChannels(described.channels, settings.channelRoles);
            delivered = selection.names;
            if (!selection.missing.empty()) {
                std::string missing;
                for (const std::size_t role : selection.missing) {
                    if (!missing.empty()) {
                        missing += ", ";
                    }
                    missing += primaryRoleName(role);
                }
                // A selected role the image carries no channel for is refused
                // rather than delivered as transparent black under a name
                // nothing carries, or silently dropped from the selection.
                refuse(describeNode(target.node()) + ": the delivered image carries no " + missing + " channel (it " +
                       (described.channels.empty() ? std::string{"carries none at all"}
                                                   : "carries " + channelListText(described.channels)) +
                       "); the selected channels cannot be delivered");
            }
        }
        preflight.plan.channels = delivered;
    } catch (const std::exception& error) {
        if (preflight.plan.problem.empty()) {
            preflight.plan.problem = error.what();
        }
    }
    return preflight;
}

// A random candidate name for one private temporary directory. There is no
// process-global counter and no shared mutable state: the CLAIM below is what
// makes ownership exclusive, so a collision is resolved by drawing again.
[[nodiscard]] std::string temporaryCandidate() {
    std::random_device device;
    std::ostringstream candidate;
    candidate << std::hex << ((static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device()))
              << '-' << static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return candidate.str();
}

// The private temporary directory of ONE output, acquired exclusively and
// released by ownership.
//
// Ownership is the whole point (story 82): a random candidate is claimed by
// `create_directory`, so a name that already exists is another delivery's and is
// retried instead of reused, and removing the directory can therefore never
// delete a path this job did not create. The delivered file lives inside it
// under its exact final name (an encoder is selected from the extension), and
// destruction — on success, on a failed frame, on cancellation, on an exception —
// removes the directory and anything still inside it. "No hidden temporary
// survives" is thus a property of the type rather than bookkeeping a caller can
// forget.
class DeliveryTemporary {
public:
    DeliveryTemporary() = default;
    ~DeliveryTemporary() { discard(); }
    DeliveryTemporary(const DeliveryTemporary&) = delete;
    DeliveryTemporary& operator=(const DeliveryTemporary&) = delete;
    DeliveryTemporary(DeliveryTemporary&& other) noexcept
        : directory_(std::move(other.directory_)), file_(std::move(other.file_)), owned_(other.owned_) {
        other.owned_ = false;
    }
    DeliveryTemporary& operator=(DeliveryTemporary&& other) noexcept {
        if (this != &other) {
            discard();
            directory_ = std::move(other.directory_);
            file_ = std::move(other.file_);
            owned_ = other.owned_;
            other.owned_ = false;
        }
        return *this;
    }

    // Claims one private directory beside `finalPath`. The caller resolves the
    // destination policy first (its parent directory exists by then); a parent
    // that cannot hold the directory is reported with the offending path.
    [[nodiscard]] static DeliveryTemporary claim(const std::string& finalPath);

    // The file to write inside the private directory: `finalPath`'s own file
    // name, so the encoder chosen from the extension is the authored one.
    [[nodiscard]] const std::string& file() const { return file_; }

    // Removes the private directory and whatever is still inside it. Idempotent.
    void discard() {
        if (!owned_) {
            return;
        }
        owned_ = false;
        std::error_code ignored;
        std::filesystem::remove_all(std::filesystem::path(directory_), ignored);
    }

private:
    std::string directory_;
    std::string file_;
    bool owned_{false};
};

DeliveryTemporary DeliveryTemporary::claim(const std::string& finalPath) {
    const std::filesystem::path target(finalPath);
    const std::filesystem::path parent =
        target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::filesystem::path directory = parent / (".nemo-delivery-" + temporaryCandidate());
        std::error_code failure;
        if (std::filesystem::create_directory(directory, failure)) {
            DeliveryTemporary claimed;
            claimed.directory_ = directory.string();
            claimed.file_ = (directory / target.filename()).string();
            claimed.owned_ = true;
            return claimed;
        }
        if (failure && failure != std::errc::file_exists) {
            throw DeliveryException("a private temporary directory could not be created: " + failure.message(),
                                    finalPath);
        }
    }
    throw DeliveryException("a private temporary directory could not be claimed beside the output", finalPath);
}

// The destination identity the SUBMITTER can state without touching the
// filesystem: separators, "." and ".." are resolved lexically, so `./shot.exr`,
// `a/../shot.exr` and `shot.exr` name the same delivery. It runs on the caller's
// thread (the UI event thread included) and therefore never stats, reads or
// resolves a link.
[[nodiscard]] std::string destinationKey(const std::string& path) {
    return std::filesystem::path(path).lexically_normal().string();
}

// The destination identity the FILESYSTEM states: absolute, with every existing
// symlinked prefix resolved, so two spellings of one file agree. Worker-only —
// it is filesystem I/O, and the queue deliberately keeps that off `submit`.
[[nodiscard]] std::string canonicalDestinationKey(const std::string& path) {
    std::error_code failure;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(std::filesystem::path(path), failure);
    if (failure) {
        return destinationKey(path);
    }
    return resolved.lexically_normal().string();
}

// Publishes a completed temporary at its final path. Without explicit overwrite
// authorization the publication must never replace a file that appeared while
// the job ran, so it links the temporary into place (which fails when the path
// exists) and drops the temporary; with authorization it is one atomic rename.
void publishFile(const std::string& temporary, const std::string& finalPath, const bool overwrite) {
    std::error_code failure;
    if (overwrite) {
        std::filesystem::rename(temporary, finalPath, failure);
    } else {
        std::filesystem::create_hard_link(temporary, finalPath, failure);
        if (!failure) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
    }
    if (failure) {
        throw DeliveryException("the output could not be finalized at its output path: " + failure.message(),
                                finalPath);
    }
}

// The destination checks one frame repeats at write time: preflight authorized
// it, but a file that appeared while the job ran is still a collision the caller
// never authorized (story 80), and a missing directory is still resolved by the
// authored policy.
void prepareDestination(const DeliverySettings& settings, const std::string& path) {
    if (!settings.overwrite && std::filesystem::exists(std::filesystem::path(path))) {
        throw DeliveryException("the output file already exists and overwriting was not authorized", path);
    }
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        return;
    }
    if (settings.createDirectories) {
        std::error_code failure;
        std::filesystem::create_directories(parent, failure);
        if (failure) {
            throw DeliveryException("the output directory could not be created: " + failure.message(), path);
        }
    } else if (!std::filesystem::is_directory(parent)) {
        throw DeliveryException("the output directory does not exist and creating directories was not authorized",
                                path);
    }
}

}  // namespace

const char* deliveryStateName(const DeliveryState state) {
    switch (state) {
    case DeliveryState::Queued:
        return "queued";
    case DeliveryState::Running:
        return "running";
    case DeliveryState::Completed:
        return "completed";
    case DeliveryState::Cancelled:
        return "cancelled";
    case DeliveryState::Failed:
        return "failed";
    }
    return "queued";
}

DeliverySettings deliverySettings(const Document& document, const NetworkId network, const NodeId node,
                                  const std::int64_t localTime) {
    const ResolvedDeliveryNode target = resolveDeliveryNode(document, network, node, localTime);
    const DeliverySettings settings = resolveSettings(document, network, target);
    const NodeCatalog& catalog = document.network(network).graph().catalog();
    if (const std::string problem = settingsProblem(catalog, target.node(), settings); !problem.empty()) {
        throw DeliveryException(problem);
    }
    return settings;
}

// ---------------------------------------------------------------------------
// The bounded native queue.
// ---------------------------------------------------------------------------

struct DeliveryQueue::Impl {
    // One accepted job: its frozen snapshot, its frozen settings, the frames and
    // the raster it delivers, and the observable record a caller reads. `info`
    // is the only part the worker mutates, and only under `mutex_`.
    struct Job {
        DeliveryJobInfo info;
        Document document;
        NetworkId network{kInvalidNetwork};
        NodeId node{kInvalidNode};
        DeliverySettings settings;
        Region raster;
        Region format;
        ImageDescription description;
        std::vector<DeliveryFrame> frames;
        std::string configPath;
        bool movie{false};
        // The final paths this active job reserves: the keys it registered at
        // submit (lexical) are what a second submission is refused on, and the
        // canonical keys it registers in its worker preflight are what catches a
        // symlinked or relative alias before it writes anything.
        std::vector<std::string> destinations;
        // Immutable destination owners active when this job was accepted. The
        // single worker may finish them before resolving this job's aliases.
        std::vector<std::shared_ptr<const Job>> predecessors;
        bool cancelRequested{false};
    };

    Impl(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator, std::filesystem::path shaders,
         const std::size_t maxAcceptedJobs, std::vector<GpuNodeContribution> contributions)
        : instance_(&instance), device_(&device), allocator_(&allocator), shaders_(std::move(shaders)),
          maxAcceptedJobs_(maxAcceptedJobs), contributions_(std::move(contributions)) {
        // The description inventory is assembled ONCE, here, from the very list
        // this queue will build its native library from (issue #37): a pure
        // descriptor projection that loads and compiles nothing, so a preflight
        // that needs it — the blocking CLI query, or the worker's own preflight
        // before its first frame — never forces the native effect library to be
        // built earlier than the existing lifecycle demands, and never falls
        // back to an implicit built-in assembly either.
        std::vector<NodeContribution> descriptors;
        descriptors.reserve(contributions_.size());
        for (const GpuNodeContribution& contribution : contributions_) {
            descriptors.push_back(contribution.node);
        }
        registrations_ = std::make_shared<const NodeContributions>(std::move(descriptors));
        worker_ = std::thread([this] { run(); });
    }

    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // Stops the worker. A job that is still running is asked to stop at its next
    // frame boundary instead of being abandoned mid-frame, so destroying the
    // queue can never leave a hidden temporary or a partially written file
    // behind; EXR frames already published stay on disk and a movie publishes
    // nothing. No caller can observe the cancellation afterwards — the queue is
    // going away — but the delivered files are exactly the ones that completed.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            if (const std::shared_ptr<Job> running = nextRunningLocked(); running != nullptr) {
                running->cancelRequested = true;
            }
        }
        wake_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::shared_ptr<Job> nextQueuedLocked() const {
        for (const std::shared_ptr<Job>& job : jobs_) {
            if (job->info.state == DeliveryState::Queued) {
                return job;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::shared_ptr<Job> nextRunningLocked() const {
        for (const std::shared_ptr<Job>& job : jobs_) {
            if (job->info.state == DeliveryState::Running) {
                return job;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::shared_ptr<Job> findLocked(const std::uint64_t id) const {
        for (const std::shared_ptr<Job>& job : jobs_) {
            if (job->info.id == id) {
                return job;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool cancellationRequested(const Job& job) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return job.cancelRequested || stopping_;
    }

    // Registers the destination keys an accepted job reserves, refusing the
    // submission when an active job already holds one: two deliveries must never
    // write the same file at the same time, even with overwrite authorization.
    // The check is one map lookup per destination — never a comparison against
    // every other job's frames. Callers hold `mutex_`.
    void reserveLocked(Job& job) {
        // Every destination is checked before ANY of them is registered: a
        // refused submission must leave no reservation behind, or a job that was
        // never accepted would keep refusing that path forever.
        for (const std::string& path : job.destinations) {
            const auto taken = reservations_.find(destinationKey(path));
            if (taken != reservations_.end() && taken->second != job.info.id) {
                throw DeliveryException("output path '" + path + "' is already being delivered by job " +
                                            std::to_string(taken->second) +
                                            "; overlapping active deliveries are refused",
                                        path);
            }
        }
        for (const std::string& path : job.destinations) {
            reservations_[destinationKey(path)] = job.info.id;
        }
    }

    // Resolves each destination as the filesystem sees it and registers that
    // identity too, so a spelling the submitter could not compare — a relative
    // path beside an absolute one, a symlinked directory — is settled BEFORE this
    // job writes anything. Worker-only: it is real filesystem I/O, and that is
    // exactly why it lives here and not on the submitting thread.
    //
    // The OLDER accepted job keeps its destination: a conflict with an older
    // active job refuses THIS job, while a conflict with a NEWER queued job
    // refuses that one in place (it has written nothing and its claim is the
    // later one), which is the same rule the submitter applies lexically.
    void reserveCanonical(const std::shared_ptr<Job>& job) {
        std::vector<std::pair<std::string, std::string>> keys;
        keys.reserve(job->destinations.size());
        for (const std::string& path : job->destinations) {
            keys.emplace_back(path, canonicalDestinationKey(path));
        }
        std::sort(keys.begin(), keys.end(),
                  [](const auto& left, const auto& right) { return left.second < right.second; });
        const auto predecessors = std::move(job->predecessors);
        for (const auto& previous : predecessors) {
            for (const auto& path : previous->destinations) {
                const auto canonical = canonicalDestinationKey(path);
                const auto overlap =
                    std::lower_bound(keys.begin(), keys.end(), canonical,
                                     [](const auto& entry, const auto& key) { return entry.second < key; });
                if (overlap != keys.end() && overlap->second == canonical) {
                    throw DeliveryException("output path '" + overlap->first + "' overlaps delivery job " +
                                                std::to_string(previous->info.id) + " accepted earlier at '" + path +
                                                "'; overlapping active deliveries are refused",
                                            overlap->first);
                }
            }
        }
        std::vector<std::shared_ptr<Job>> yielded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [path, key] : keys) {
                const auto taken = reservations_.find(key);
                if (taken == reservations_.end() || taken->second == job->info.id) {
                    continue;
                }
                const std::shared_ptr<Job> owner = findLocked(taken->second);
                if (owner == nullptr) {
                    continue;
                }
                const std::string reason = "output path '" + path + "' is the file already reserved as '" + key +
                                           "' by active delivery job " + std::to_string(owner->info.id) +
                                           "; overlapping active deliveries are refused";
                if (owner->info.id > job->info.id) {
                    // The newer claim yields; it becomes a failed job that never
                    // wrote anything, which is what its own submitter would have
                    // been told had the two spellings been comparable.
                    failLocked(*owner, reason);
                    yielded.push_back(owner);
                    continue;
                }
                throw DeliveryException(reason, path);
            }
            for (const auto& [path, key] : keys) {
                static_cast<void>(path);
                reservations_[key] = job->info.id;
            }
        }
        if (!yielded.empty()) {
            // The refused jobs are settled now: a waiter on the queue must be
            // able to observe that without waiting for this job as well.
            idle_.notify_all();
        }
    }

    // Releases every destination key a job registered. Requires `mutex_`.
    void releaseLocked(Job& job) {
        job.predecessors.clear();
        for (auto entry = reservations_.begin(); entry != reservations_.end();) {
            entry = entry->second == job.info.id ? reservations_.erase(entry) : std::next(entry);
        }
    }

    // Marks a job failed and releases its reservations. Callers hold `mutex_`.
    void failLocked(Job& job, std::string reason) {
        job.info.state = DeliveryState::Failed;
        job.info.error = std::move(reason);
        job.info.writtenFrames = 0;
        releaseLocked(job);
    }

    void recordFrameResult(Job& job, DeliveryFileResult result);
    // Advances the encoded-frame counter of a movie (progress only: nothing is
    // published until the container is complete).
    void recordEncodedFrame(Job& job);
    // Records the staging evidence of one frame: the peak bytes its final
    // transfer charged to the shared allocator.
    void recordStaging(Job& job, std::uint64_t bytes);

    // Loads the native effect library once, on the worker: compilation is CPU
    // preparation and never runs on the UI event thread.
    void requireEffects();

    void execute(const std::shared_ptr<Job>& job);
    void runFrame(Job& job, const DeliveryFrame& frame, SourceSession& sources, media::DeliveryColorProcessor& color);
    // Renders one frame through the native executor and the GPU staging owner,
    // then applies the job's delivery color. Throws on any failure.
    [[nodiscard]] CpuImage renderFrame(Job& job, const DeliveryFrame& frame, SourceSession& sources,
                                       media::DeliveryColorProcessor& color);
    void runMovie(Job& job, SourceSession& sources, media::DeliveryColorProcessor& color);
    void settle(const std::shared_ptr<Job>& job, bool cancelled);
    void fail(const std::shared_ptr<Job>& job, const std::string& reason);
    void run();

    [[nodiscard]] bool forgot(const std::uint64_t id);
    [[nodiscard]] bool cancelJob(const std::uint64_t id);

    // Borrowed application owners: never created, never destroyed here.
    gpu::Instance* instance_ = nullptr;
    gpu::Device* device_ = nullptr;
    gpu::Allocator* allocator_ = nullptr;
    std::filesystem::path shaders_;
    std::size_t maxAcceptedJobs_{8};
    // The complete immutable native inventory this queue was created with
    // (issue #37). It is retained — not re-derived and not registered anywhere
    // globally — until the worker builds the native effect library below, so
    // every job of this queue evaluates through exactly the caller's list.
    std::vector<GpuNodeContribution> contributions_;
    // The SAME list as an immutable registration snapshot (issue #37), assembled
    // once when the queue is created and never replaced: every delivery
    // description — the blocking CLI preflight included — resolves its nodes
    // through this one, so a described chain and the executor's own inventory can
    // never disagree. It is a value snapshot of descriptors only; the native
    // library below still compiles lazily, on the worker.
    std::shared_ptr<const NodeContributions> registrations_;
    // Worker-owned: the native effect library, loaded on first use.
    EffectLibrary effects_;
    bool effectsReady_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::vector<std::shared_ptr<Job>> jobs_;
    // Destination keys held by ACTIVE jobs (both the submitter's lexical keys and
    // the worker's canonical ones), keyed to the claiming job.
    std::map<std::string, std::uint64_t> reservations_;
    std::uint64_t nextId_{1};
    bool stopping_{false};
    std::thread worker_;
};

void DeliveryQueue::Impl::recordFrameResult(Job& job, DeliveryFileResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result.written) {
        ++job.info.writtenFrames;
    } else {
        ++job.info.failedFrames;
    }
    job.info.files.push_back(std::move(result));
}

void DeliveryQueue::Impl::recordEncodedFrame(Job& job) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++job.info.writtenFrames;
}

void DeliveryQueue::Impl::recordStaging(Job& job, const std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    job.info.nativeStaging = true;
    job.info.stagingBytes = std::max(job.info.stagingBytes, bytes);
}

void DeliveryQueue::Impl::requireEffects() {
    if (effectsReady_) {
        return;
    }
    if (shaders_.empty()) {
        throw DeliveryException(
            "delivery needs the compiled native shader directory; the queue was constructed without one");
    }
    // The library is built from THIS queue's retained inventory (issue #37)
    // rather than from an implicit built-in assembly, so an installed package's
    // callbacks and metadata reach every delivered frame and no job can evaluate
    // through a different inventory than the queue was created with. Assembly
    // validates the whole declaration before it publishes anything and throws
    // with the offending relationship; the retained list is therefore only
    // released once an assembly actually succeeded, so a refused inventory keeps
    // reporting the same refusal to every later job instead of degrading into an
    // empty library.
    effects_ = EffectLibrary(contributions_, EffectBackend::Slang, shaders_);
    contributions_.clear();
    effectsReady_ = true;
}

CpuImage DeliveryQueue::Impl::renderFrame(Job& job, const DeliveryFrame& frame, SourceSession& sources,
                                          media::DeliveryColorProcessor& color) {
    EvaluationRequest request;
    request.network = job.network;
    request.output = job.node;
    // Every delivered frame is evaluated at its own document frame; only the
    // settings and the raster are frozen, which is what makes a range a range
    // instead of the same frame written N times.
    request.localTime = frame.documentFrame;
    request.region = job.raster;
    request.fullWidth = job.format.width;
    request.fullHeight = job.format.height;
    // No channel demand is set here, deliberately: a request's channel list
    // prunes what upstream nodes compute but never narrows the raster the target
    // produces (the executor builds the produced layout from the target's own
    // description), so the authored selection is applied to the evaluated frame
    // below instead of being asked for twice with two different meanings.
    // Full quality through the native executor, no reuse cache: a delivery
    // result never publishes into the interactive cache, and the viewer's own
    // (possibly lossy) representation can never serve it (story 79).
    GpuEvaluation evaluation = evaluateGpu(job.document, request, effects_, *device_, *allocator_, kEvaluationTimeoutNs,
                                           nullptr, &sources, sources.colorConfigIdentity());
    if (evaluation.completion) {
        if (!device_->submissions(device_->graphics_family()).wait(*evaluation.completion, kEvaluationTimeoutNs)) {
            throw DeliveryException("the frame's native evaluation did not complete within its timeout", frame.path);
        }
    }
    const auto produced = evaluation.images.find(job.node);
    if (produced == evaluation.images.end() || produced->second == nullptr) {
        throw DeliveryException("the native evaluation produced no image for the Write node", frame.path);
    }
    const GpuNodeImage& image = *produced->second;
    // The EVALUATED frame carries every channel the target's description names
    // (a request's channel demand prunes what upstream nodes compute, never the
    // delivered raster), which is the contract this check has always stated.
    if (image.layout.width != job.raster.width || image.layout.height != job.raster.height ||
        image.layout.channels != job.description.channels) {
        throw DeliveryException(
            "the produced frame does not match the frozen delivery raster: expected " +
                std::to_string(job.raster.width) + "x" + std::to_string(job.raster.height) + " pixels with channels " +
                channelListText(job.description.channels) + ", produced " + std::to_string(image.layout.width) + "x" +
                std::to_string(image.layout.height) + " pixels with channels " + channelListText(image.layout.channels),
            frame.path);
    }
    // The final production transfer: charged to the shared allocator, retained by
    // the device's own submission queue, and never the diagnostic readback.
    gpu::ExportStaging staging(*device_, *allocator_);
    gpu::StagedExport staged = staging.stage(image.image, image.layout, kTransferTimeoutNs);
    recordStaging(job, staged.stagingBytes);
    color.apply(staged.image);
    // The authored channel selection (issue #102) is a projection of the
    // evaluated frame, applied after the color transform exactly like the
    // writer's own alpha handling: the delivered image carries exactly the
    // selected channels, and the `all` default returns the frame unchanged.
    if (!job.settings.channelRoles.empty()) {
        staged.image = gatherDeliveredChannels(std::move(staged.image), job.info.channels);
    }
    return std::move(staged.image);
}

void DeliveryQueue::Impl::runFrame(Job& job, const DeliveryFrame& frame, SourceSession& sources,
                                   media::DeliveryColorProcessor& color) {
    DeliveryFileResult result;
    result.documentFrame = frame.documentFrame;
    result.fileFrame = frame.fileFrame;
    result.path = frame.path;
    try {
        prepareDestination(job.settings, frame.path);
        CpuImage staged = renderFrame(job, frame, sources, color);
        // The destination policy is resolved first, so the private directory has
        // a real parent; from here the temporary owns itself and disappears on
        // every exit path.
        DeliveryTemporary temporary = DeliveryTemporary::claim(frame.path);
        // The raster's own first-sample coordinate is the file's data-window
        // origin, so the delivered image keeps the described geometry instead of
        // being moved to the origin on the way out (issue #88, story 77), and the
        // described pixel aspect and channel naming travel with it. The channels
        // are the ones this job DELIVERS (issue #102): the authored selection, or
        // every described channel.
        ImageDescription delivered = job.description;
        delivered.dataBounds = job.raster;
        delivered.channels = job.info.channels;
        media::writeDeliveryImage(temporary.file(), staged, delivered, job.settings.output);
        publishFile(temporary.file(), frame.path, job.settings.overwrite);
        temporary.discard();
        result.written = true;
    } catch (const std::exception& error) {
        result.written = false;
        result.error = frameText(frame) + ": " + error.what();
    }
    recordFrameResult(job, std::move(result));
}

void DeliveryQueue::Impl::runMovie(Job& job, SourceSession& sources, media::DeliveryColorProcessor& color) {
    const std::string finalPath = job.settings.file;
    // The container is written inside an exclusively claimed private directory
    // and published only after the whole range encoded; abandoning the writer or
    // destroying the directory removes whatever was written, so a movie is never
    // partially delivered.
    DeliveryTemporary temporary;
    std::unique_ptr<media::DeliveryMovieWriter> writer;
    // Frames encoded so far, counted here and published as progress: the
    // container is ONE file and exists only after the whole range encoded.
    std::size_t encoded = 0;
    bool cancelled = false;
    bool published = false;
    std::string failure;
    try {
        prepareDestination(job.settings, finalPath);
        temporary = DeliveryTemporary::claim(finalPath);
        // The writer is created from the frozen frame contract; a movie whose
        // frames do not match it is refused per frame instead of silently
        // resized. A movie frame IS the delivered raster, so the contract's
        // format is that raster (unlike a still, whose display window is the
        // authored format and whose data window is the delivered raster).
        ImageDescription contract = job.description;
        contract.format = job.raster;
        contract.dataBounds = job.raster;
        // The channels this job DELIVERS (issue #102): the authored selection, or
        // every described channel. Every encoded frame is projected onto them.
        contract.channels = job.info.channels;
        writer = std::make_unique<media::DeliveryMovieWriter>(temporary.file(), contract, job.settings.output);
        for (const DeliveryFrame& frame : job.frames) {
            if (cancellationRequested(job)) {
                cancelled = true;
                break;
            }
            CpuImage staged = renderFrame(job, frame, sources, color);
            writer->append(staged);
            ++encoded;
            recordEncodedFrame(job);
        }
        if (!cancelled && encoded != job.info.totalFrames) {
            failure = "the movie range ended after " + std::to_string(encoded) + " of " +
                      std::to_string(job.info.totalFrames) + " frames";
        }
        if (failure.empty() && !cancelled) {
            // The container is complete at the temporary path; publishing it is
            // this seam's own atomic step, and only then is it a delivered file.
            writer->finish();
            writer.reset();
            // A cancel that arrived while the encoder was flushing is still a
            // cancel: a movie the caller asked to stop is never published.
            if (cancellationRequested(job)) {
                cancelled = true;
            } else {
                publishFile(temporary.file(), finalPath, job.settings.overwrite);
                published = true;
            }
        }
    } catch (const std::exception& error) {
        failure = error.what();
    }
    if (writer) {
        // Destroying the writer without finish() removes the partial container,
        // so nothing is ever left where a finished movie would be.
        writer.reset();
    }
    // The private directory goes away on every outcome: after a rename it is
    // empty, after a hard-link publication it still holds the temporary copy, and
    // after a failure it holds the unfinished container. Ownership — not a
    // bookkeeping flag — is what makes this safe.
    temporary.discard();
    // What the destination holds after a movie that was NOT published: a file the
    // caller already had is untouched, and a destination that did not exist still
    // does not. Saying "no file at the path" would be false for the first case.
    const bool retained = !published && std::filesystem::exists(std::filesystem::path(finalPath));
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled) {
        job.info.state = DeliveryState::Cancelled;
        job.info.error = "cancelled after encoding " + std::to_string(encoded) + " of " +
                         std::to_string(job.info.totalFrames) + " frames; no new movie was published" +
                         (retained ? " and the file already at '" + finalPath + "' is unchanged"
                                   : " and no file was created at '" + finalPath + "'");
        // A movie that never landed delivered no file: the encoded frames are
        // progress, not output, so the delivered count is restated honestly.
        job.info.writtenFrames = 0;
    } else if (!failure.empty()) {
        job.info.state = DeliveryState::Failed;
        job.info.error = "the movie was not published: " + failure +
                         (retained ? " (the file already at '" + finalPath + "' is unchanged)"
                                   : " (no file was created at '" + finalPath + "')");
        job.info.writtenFrames = 0;
        job.info.failedFrames = job.info.totalFrames;
    } else {
        job.info.state = DeliveryState::Completed;
        DeliveryFileResult result;
        result.documentFrame = job.frames.empty() ? job.settings.frameFirst : job.frames.front().documentFrame;
        result.fileFrame = job.frames.empty() ? job.settings.frameOffset : job.frames.front().fileFrame;
        result.path = finalPath;
        result.written = true;
        job.info.files.push_back(std::move(result));
    }
    releaseLocked(job);
    idle_.notify_all();
}

void DeliveryQueue::Impl::execute(const std::shared_ptr<Job>& job) {
    // ONE retained native source session per accepted job: the decode state, the
    // color configuration and its retained processors belong to the job, so every
    // frame of a range resolves the same media against the same generation and
    // the accepted job is independent of later viewer activity.
    std::unique_ptr<SourceSession> sources;
    try {
        if (shaders_.empty()) {
            throw DeliveryException(
                "delivery needs the compiled native shader directory; the queue was constructed without one");
        }
        sources = std::make_unique<SourceSession>(*instance_, *device_, *allocator_, shaders_ / "mediaConvert.spv",
                                                  job->configPath);
        requireEffects();
    } catch (const std::exception& error) {
        fail(job, error.what());
        return;
    }

    // Preflight on the worker, through the SAME native source descriptions
    // execution uses: a refusal (a missing directory, a collision, an unsupported
    // setting, media that cannot be described) is reported as a failed job and
    // nothing is written.
    JobPlan planned;
    try {
        planned = planJob(job->document, job->network, job->node, job->settings, job->settings.frameFirst,
                          *registrations_, sources.get());
    } catch (const std::exception& error) {
        fail(job, error.what());
        return;
    }
    if (!planned.plan.problem.empty()) {
        fail(job, planned.plan.problem);
        return;
    }
    // The destinations are resolved as the filesystem sees them BEFORE anything is
    // written, so an alias of another active job's output (a symlinked directory,
    // a relative spelling) is refused here rather than silently writing a file two
    // deliveries both believe they own. The older accepted job keeps its claim.
    try {
        reserveCanonical(job);
    } catch (const std::exception& error) {
        fail(job, error.what());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!planned.plan.collisions.empty() && !job->settings.overwrite) {
            failLocked(*job, "the job would overwrite " + std::to_string(planned.plan.collisions.size()) +
                                 " existing file(s) and overwriting was not authorized: " +
                                 pathListText(planned.plan.collisions));
            idle_.notify_all();
            return;
        }
        job->raster = planned.raster;
        job->format = planned.format;
        job->description = planned.description;
        job->movie = planned.movie;
        job->frames = planned.plan.frames;
        job->info.width = planned.plan.width;
        job->info.height = planned.plan.height;
        job->info.channels = planned.plan.channels;
    }

    // One retained color conversion for the whole job (a range never re-opens the
    // configuration and never builds a processor per frame).
    std::unique_ptr<media::DeliveryColorProcessor> color;
    try {
        color =
            std::make_unique<media::DeliveryColorProcessor>(job->settings.output, job->document.color, job->configPath);
    } catch (const std::exception& error) {
        fail(job, error.what());
        return;
    }

    if (job->movie) {
        runMovie(*job, *sources, *color);
        return;
    }

    bool cancelled = false;
    for (const DeliveryFrame& frame : job->frames) {
        if (cancellationRequested(*job)) {
            cancelled = true;
            break;
        }
        runFrame(*job, frame, *sources, *color);
    }
    settle(job, cancelled);
}

void DeliveryQueue::Impl::settle(const std::shared_ptr<Job>& job, const bool cancelled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled) {
            job->info.state = DeliveryState::Cancelled;
            job->info.error = "cancelled after writing " + std::to_string(job->info.writtenFrames) + " of " +
                              std::to_string(job->info.totalFrames) +
                              " frames; the frames already written stay on disk and the remaining frames were not "
                              "written";
        } else if (job->info.writtenFrames == job->info.totalFrames) {
            job->info.state = DeliveryState::Completed;
        } else {
            job->info.state = DeliveryState::Failed;
            job->info.error = std::to_string(job->info.failedFrames) + " of " + std::to_string(job->info.totalFrames) +
                              " frames failed and were not written; every failed frame is reported with its output "
                              "path";
        }
        releaseLocked(*job);
    }
    idle_.notify_all();
}

void DeliveryQueue::Impl::fail(const std::shared_ptr<Job>& job, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failLocked(*job, reason);
    }
    idle_.notify_all();
}

void DeliveryQueue::Impl::run() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || nextQueuedLocked() != nullptr; });
            if (stopping_) {
                return;
            }
            job = nextQueuedLocked();
            job->info.state = DeliveryState::Running;
        }
        execute(job);
    }
}

bool DeliveryQueue::Impl::forgot(const std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto entry = jobs_.begin(); entry != jobs_.end(); ++entry) {
        if ((*entry)->info.id != id) {
            continue;
        }
        if (!(*entry)->info.settled()) {
            return false;
        }
        releaseLocked(**entry);
        jobs_.erase(entry);
        return true;
    }
    return false;
}

bool DeliveryQueue::Impl::cancelJob(const std::uint64_t id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::shared_ptr<Job> job = findLocked(id);
        if (job == nullptr || job->info.settled()) {
            return false;
        }
        if (job->info.state == DeliveryState::Queued) {
            // A job that has not started stops immediately: nothing was written,
            // nothing will be, and its destination claim is released at once.
            job->info.state = DeliveryState::Cancelled;
            releaseLocked(*job);
            job->info.error =
                "cancelled before it started; 0 of " + std::to_string(job->info.totalFrames) + " frames were written";
            idle_.notify_all();
            return true;
        }
        job->cancelRequested = true;
    }
    return true;
}

DeliveryQueue::DeliveryQueue(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                             const std::filesystem::path& shaders, const std::size_t maxAcceptedJobs,
                             std::vector<GpuNodeContribution> contributions)
    : impl_(std::make_unique<Impl>(instance, device, allocator, shaders, maxAcceptedJobs, std::move(contributions))) {}

DeliveryQueue::~DeliveryQueue() = default;

std::uint64_t DeliveryQueue::submit(Document document, const NetworkId network, const NodeId node,
                                    const std::int64_t localTime, std::string configPath) {
    const DeliverySettings settings = deliverySettings(document, network, node, localTime);
    return submit(std::move(document), network, node, settings, localTime, std::move(configPath));
}

std::uint64_t DeliveryQueue::submit(Document document, const NetworkId network, const NodeId node,
                                    const DeliverySettings settings, const std::int64_t localTime,
                                    std::string configPath) {
    // Everything that is a property of the authored values alone is refused HERE,
    // synchronously, with the offending value named. Nothing filesystem- or
    // media-related is touched: the worker preflights before any write, so a
    // submitter on the UI thread never blocks on a directory, a collision or a
    // decoder.
    const ResolvedDeliveryNode target = resolveDeliveryNode(document, network, node, localTime);
    const NodeCatalog& catalog = document.network(network).graph().catalog();
    if (const std::string problem = settingsProblem(catalog, target.node(), settings); !problem.empty()) {
        throw DeliveryException(problem);
    }
    const FrameSet frames = resolveFrameSet(settings, isMovieType(settings.output.fileType));
    if (!frames.problem.empty()) {
        throw DeliveryException(frames.problem);
    }

    auto job = std::make_shared<Impl::Job>();
    job->document = std::move(document);
    job->network = network;
    job->node = node;
    job->settings = settings;
    job->configPath = std::move(configPath);
    job->movie = isMovieType(settings.output.fileType);
    // The reservation is the pure path set: a movie delivers one file, an EXR
    // range one file per frame.
    job->destinations.reserve(job->movie ? 1U : frames.frames.size());
    const std::size_t reserved = job->movie ? std::min<std::size_t>(1U, frames.frames.size()) : frames.frames.size();
    for (std::size_t index = 0; index < reserved; ++index) {
        job->destinations.push_back(frames.frames[index].path);
    }
    job->info.network = network;
    job->info.node = node;
    job->info.networkName = target.networkName;
    job->info.nodeName = target.node().name;
    job->info.settings = settings;
    job->info.movie = job->movie;
    job->info.totalFrames = frames.frames.size();

    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (impl_->jobs_.size() >= impl_->maxAcceptedJobs_) {
            throw DeliveryException("the delivery queue already holds " + std::to_string(impl_->maxAcceptedJobs_) +
                                    " accepted jobs, which is its limit; forget a settled job before submitting "
                                    "another");
        }
        id = impl_->nextId_++;
        job->info.id = id;
        for (const auto& previous : impl_->jobs_) {
            if (!previous->info.settled())
                job->predecessors.push_back(previous);
        }
        impl_->reserveLocked(*job);
        impl_->jobs_.push_back(job);
    }
    impl_->wake_.notify_all();
    return id;
}

DeliveryPlan DeliveryQueue::plan(const Document& document, const NetworkId network, const NodeId node,
                                 const std::int64_t localTime, const std::string& configPath) {
    // Resolving the authored settings is part of the preflight, so a node that is
    // not a Write node, a missing node or an unsupported authored choice is a
    // reported refusal like any other — never an exception out of a query.
    DeliveryPlan refusal;
    try {
        return plan(document, network, node, deliverySettings(document, network, node, localTime), localTime,
                    configPath);
    } catch (const std::exception& error) {
        refusal.problem = error.what();
    }
    return refusal;
}

DeliveryPlan DeliveryQueue::plan(const Document& document, const NetworkId network, const NodeId node,
                                 const DeliverySettings& settings, const std::int64_t localTime,
                                 const std::string& configPath) {
    // The plan IS the report: a caller preflighting a graph gets the refusal
    // reason (and the frames, paths and raster it could resolve), never an
    // exception. It is deliberately blocking — it reads real media headers
    // through a temporary native source session — so it is a CLI preflight, never
    // a UI call.
    DeliveryPlan plan;
    plan.settings = settings;
    try {
        if (impl_->shaders_.empty()) {
            throw DeliveryException(
                "delivery needs the compiled native shader directory; the queue was constructed without one");
        }
        SourceSession sources(*impl_->instance_, *impl_->device_, *impl_->allocator_,
                              impl_->shaders_ / "mediaConvert.spv", configPath);
        plan = planJob(document, network, node, settings, localTime, *impl_->registrations_, &sources).plan;
        if (plan.problem.empty()) {
            // The delivered COLOR is resolved here too, exactly as execution
            // resolves it before its first write: a named color space or
            // display/view the project configuration does not carry, a missing
            // configuration, or a LUT that cannot be read is a refusal this
            // preflight must report instead of claiming the job would run.
            // Nothing is written — loading a LUT reads a file.
            const media::DeliveryColorProcessor color(settings.output, document.color, configPath);
            static_cast<void>(color.description());
        }
    } catch (const std::exception& error) {
        plan.settings = settings;
        plan.problem = error.what();
    }
    return plan;
}

std::vector<DeliveryJobInfo> DeliveryQueue::jobs() const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    std::vector<DeliveryJobInfo> infos;
    infos.reserve(impl_->jobs_.size());
    for (const std::shared_ptr<Impl::Job>& job : impl_->jobs_) {
        infos.push_back(job->info);
    }
    return infos;
}

DeliveryJobInfo DeliveryQueue::status(const std::uint64_t job) const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    const std::shared_ptr<Impl::Job> found = impl_->findLocked(job);
    if (found == nullptr) {
        throw DeliveryException("delivery job " + std::to_string(job) + " is not accepted by this queue");
    }
    return found->info;
}

bool DeliveryQueue::cancel(const std::uint64_t job) {
    return impl_->cancelJob(job);
}

bool DeliveryQueue::forget(const std::uint64_t job) {
    return impl_->forgot(job);
}

void DeliveryQueue::waitForIdle() {
    std::unique_lock<std::mutex> lock(impl_->mutex_);
    impl_->idle_.wait(lock, [this] {
        return std::none_of(impl_->jobs_.begin(), impl_->jobs_.end(), [](const std::shared_ptr<Impl::Job>& job) {
            return job->info.state == DeliveryState::Queued || job->info.state == DeliveryState::Running;
        });
    });
}

}  // namespace nemo::eval
