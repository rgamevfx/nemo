#include "ReadSourceController.hpp"

#include "MediaChooserSupport.hpp"
#include "MediaLibraryModel.hpp"
#include "nemo/core/commands/ReadSourceCommands.hpp"
#include "nemo/core/document/Animation.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/media/ImageIO.hpp"
#include "nemo/media/ViewingTransform.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace nemo::ui {
namespace {

std::optional<std::uint64_t> identity(const QVariant& value) {
    bool ok = false;
    const auto id = value.toString().trimmed().toULongLong(&ok);
    if (!ok || id == 0)
        return std::nullopt;
    return id;
}

// A whole number that must be present (Offset, Step, Start At, a Custom
// endpoint committed by the field's own edit).
bool parseInteger(const QString& text, std::int64_t& out) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    bool ok = false;
    const qlonglong value = trimmed.toLongLong(&ok);
    if (!ok)
        return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

QString frameText(const std::optional<std::int64_t> value) {
    return value ? QString::number(*value) : QString();
}

// Presentation name of a media kind (the panel vocabulary).
[[nodiscard]] QString kindName(const nemo::MediaKind kind) {
    switch (kind) {
    case nemo::MediaKind::Image:
        return QStringLiteral("image");
    case nemo::MediaKind::Video:
        return QStringLiteral("video");
    case nemo::MediaKind::Audio:
        return QStringLiteral("audio");
    case nemo::MediaKind::Sequence:
        return QStringLiteral("sequence");
    case nemo::MediaKind::Other:
        return QStringLiteral("other");
    case nemo::MediaKind::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

// The authored color choices plus the merged fill-only hint map. Core's
// resolver produced the hints (node scope first, else the shared reference),
// so this adapter never re-derives that precedence.
[[nodiscard]] nemo::media::InputColorChoice colorChoiceFrom(const nemo::ReadNodeOverrides& overrides,
                                                            const nemo::EffectiveSourceRequest* request) {
    nemo::media::InputColorChoice choice;
    if (request != nullptr) {
        choice.mode = request->inputTransform;
        choice.inputColorSpace = request->inputColorSpace;
        choice.alpha = request->alpha;
        choice.hints = request->interpretation;
        choice.nodeHintKeys = request->nodeInterpretationKeys;
        return choice;
    }
    choice.mode = overrides.inputTransform == "explicit" ? nemo::InputTransformMode::Explicit
                  : overrides.inputTransform == "raw"    ? nemo::InputTransformMode::Raw
                                                         : nemo::InputTransformMode::Auto;
    choice.inputColorSpace = overrides.inputColorSpace;
    choice.alpha = overrides.alphaMode == "straight"        ? nemo::AlphaMode::Straight
                   : overrides.alphaMode == "premultiplied" ? nemo::AlphaMode::Premultiplied
                                                            : nemo::AlphaMode::Auto;
    choice.nodeHintKeys = nemo::applyReadInterpretationHints(choice.hints, overrides);
    return choice;
}

// The explicit sequence-pattern form ('#'/'@'): the artist authored a
// sequence, so no Sequence/Single choice is asked for it.
// The occurrence identity an editor was hosted with. An EMPTY value or the
// literal "0" is the definition scope (kInvalidNetworkInstance); a non-empty
// value that does not name an existing occurrence is an error the caller
// refuses — an invalid identity is never silently treated as the definition.
struct OccurrenceScope {
    nemo::NetworkInstanceId id{nemo::kInvalidNetworkInstance};
    // The identity the caller actually named, kept even when it names no
    // occurrence: an error about an unusable scope is reported AT the requested
    // scope (when one was parseable), never normalised onto the definition.
    nemo::NetworkInstanceId requested{nemo::kInvalidNetworkInstance};
    bool valid{true};
};

[[nodiscard]] OccurrenceScope occurrenceScopeOf(const QVariant& value, const nemo::Document& document,
                                                const nemo::NetworkId network) {
    const QString text = value.toString().trimmed();
    if (text.isEmpty() || text == QLatin1String("0")) {
        return {};
    }
    bool ok = false;
    const auto id = text.toULongLong(&ok);
    if (!ok || id == 0) {
        return OccurrenceScope{nemo::kInvalidNetworkInstance, nemo::kInvalidNetworkInstance, false};
    }
    const auto occurrence = static_cast<nemo::NetworkInstanceId>(id);
    const nemo::NetworkInstance* instance = document.instance(occurrence);
    // An unknown occurrence, or one that does not instantiate the addressed
    // network, is refused — never silently normalised to the definition.
    if (instance == nullptr || (network != nemo::kInvalidNetwork && instance->definition != network)) {
        return OccurrenceScope{nemo::kInvalidNetworkInstance, occurrence, false};
    }
    return OccurrenceScope{occurrence, occurrence, true};
}

[[nodiscard]] bool hasImagePattern(const QString& path) {
    return path.contains(QLatin1Char('#')) || path.contains(QLatin1Char('@'));
}

[[nodiscard]] bool isSequence(const nemo::media::MediaImportResult& result) {
    return result.kind == nemo::MediaKind::Sequence ||
           result.discovery.status == nemo::media::SequenceDiscoveryStatus::Sequence;
}

// A still's probe: one image with time-independent availability. A numbered
// selection explicitly bound as a Single Image has this shape even though its
// directory holds neighbouring frames — that is what the explicit choice
// means. Deliberately no interval: a one-frame bound would fail every other
// local time.
[[nodiscard]] nemo::MediaProbeMetadata stillProbe(const nemo::MediaProbeMetadata& sequenceProbe) {
    nemo::MediaProbeMetadata probe = sequenceProbe;
    probe.duration = 1;
    probe.firstFrame.reset();
    probe.lastFrame.reset();
    probe.coverageQuality = nemo::CoverageQuality::Validated;
    probe.availableFrameCount = 1;
    probe.missingFrameCount = 0;
    probe.missingRanges.clear();
    return probe;
}

// A probe snapshot that follows the READ's effective mapping: node overrides
// replace the shared reference's mapping, so the frame probed here is the one
// this node evaluates.
[[nodiscard]] nemo::SourceReference probeReference(const nemo::SourceReference& shared,
                                                   const nemo::EffectiveSourceRequest& request) {
    nemo::SourceReference probe = shared;
    probe.frameOffset = request.mapping.frameOffset;
    probe.frameStep = request.mapping.frameStep;
    probe.firstFrame = request.mapping.firstFrame;
    probe.lastFrame = request.mapping.lastFrame;
    return probe;
}

}  // namespace

ReadSourceController::ReadSourceController(nemo::ProjectSession& session, MediaLibraryModel& media,
                                           NativeFileChooser& chooser, QObject* parent)
    : QObject(parent), session_(session), media_(media), chooser_(chooser) {
    colorProjectGeneration_ = session_.projectGeneration();
    colorPolicySnapshot_ = session_.document().color;
    colorConfigSnapshot_ = session_.colorConfigPath();
    colorSubscription_ = session_.subscribe(this, &ReadSourceController::sessionChanged);
}

ReadSourceController::~ReadSourceController() {
    // The subscription is RAII (Subscription's destructor unsubscribes); no
    // explicit reset is needed or available.
    if (pendingToken_ != 0)
        media_.cancelReferenceProbe(pendingToken_);
}

void ReadSourceController::sessionChanged(void* context) noexcept {
    auto* controller = static_cast<ReadSourceController*>(context);
    if (controller == nullptr) {
        return;
    }
    try {
        controller->onSessionChanged();
    } catch (...) {
        // A notification callback never propagates; the next boundary
        // re-synchronizes.
    }
}

void ReadSourceController::onSessionChanged() {
    // A project replacement/reopen (or a color-policy / config-reference change)
    // is the owner-controlled boundary at which the retained context must not be
    // carried over: a new project never reuses the previous project's context,
    // even when the path and stamp match. Config CONTENT freshness inside one
    // project is the media owner's stamp (below), not a second epoch here.
    const std::uint64_t generation = session_.projectGeneration();
    const nemo::ColorPolicy policy = session_.document().color;
    const std::string config = session_.colorConfigPath();
    if (generation != colorProjectGeneration_ || !(policy == colorPolicySnapshot_) || config != colorConfigSnapshot_) {
        colorProjectGeneration_ = generation;
        colorPolicySnapshot_ = policy;
        colorConfigSnapshot_ = config;
        invalidateColorState();
        emit changed();
    }
}

void ReadSourceController::invalidateColorState() {
    inputColorCache_.reset();
    inputColorIdentity_.clear();
    inputColorPolicyConfig_.clear();
    inputColorPolicyWorking_.clear();
    colorSpaceCache_.clear();
    colorSpaceCacheConfig_.clear();
    colorSpaceCacheValid_ = false;
}

// The identity of one Read binding scope.
[[nodiscard]] nemo::ParameterAddress targetAddress(const nemo::NetworkId network, const nemo::NodeId node,
                                                   const nemo::NetworkInstanceId occurrence) {
    return nemo::ParameterAddress{network, node, std::string{nemo::kReadParamSourceKey}, occurrence};
}

// Public-boundary occurrence resolution: a non-empty but unusable identity is
// an error (the offending relationship is named), while empty/"0" is the
// definition scope. The same refusal applies to queries.
bool ReadSourceController::resolveOccurrence(const QVariant& value, const nemo::NetworkId network,
                                             const nemo::NodeId node, nemo::NetworkInstanceId& occurrence,
                                             const bool report) const {
    const OccurrenceScope scope = occurrenceScopeOf(value, session_.document(), network);
    if (!scope.valid) {
        if (report) {
            const_cast<ReadSourceController*>(this)->setError(targetAddress(network, node, scope.requested),
                                                              QStringLiteral("Unknown media occurrence scope: '") +
                                                                  value.toString() + QStringLiteral("'."));
        }
        return false;
    }
    occurrence = scope.id;
    return true;
}

bool ReadSourceController::resolveNode(const QString& networkId, const QVariant& nodeIdValue, nemo::NetworkId& network,
                                       nemo::NodeId& node) const {
    const auto parsedNetwork = identity(networkId);
    const auto parsedNode = identity(nodeIdValue);
    if (!parsedNetwork || !parsedNode)
        return false;
    network = *parsedNetwork;
    node = *parsedNode;
    // QML may hold a stale network after project replacement; the document
    // lookup below must not throw across the invokable boundary.
    for (const auto& candidate : session_.document().networks()) {
        if (candidate.id() == network)
            return true;
    }
    return false;
}

const nemo::NodeInstance* ReadSourceController::nodeInstance(const nemo::NetworkId network,
                                                             const nemo::NodeId node) const {
    return session_.document().network(network).graph().node(node);
}

// The EFFECTIVE source key this Read binds at a frame/occurrence: a keyed
// `source` publishes its current-frame value, so facts and shared-scope repairs
// must never read the static authored string when it is animated.
std::optional<std::string> ReadSourceController::effectiveSourceKey(const nemo::NetworkId network,
                                                                    const nemo::NodeId node,
                                                                    const nemo::NetworkInstanceId occurrence,
                                                                    const int frame) const {
    const std::optional<nemo::NodeInstance> effective = effectiveNode(network, node, occurrence, frame);
    if (!effective.has_value()) {
        return std::nullopt;
    }
    // Single-key query through the core animation owner: a keyed `source`
    // publishes its current-frame value, and no full node copy is needed.
    const nemo::ParameterAddress address{network, node, std::string{nemo::kReadParamSourceKey}, occurrence};
    const nemo::ParameterValue value =
        nemo::animatedParameterValue(session_.document(), address, static_cast<double>(frame));
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return std::nullopt;
}

bool ReadSourceController::currentBinding(const nemo::NetworkId network, const nemo::NodeId node,
                                          const nemo::NetworkInstanceId occurrence, const int frame, std::string& key,
                                          nemo::SourceReference& reference) const {
    const nemo::Document& document = session_.document();
    // The effective binding at the frame/occurrence, never the stale static one.
    const std::optional<std::string> effective = effectiveSourceKey(network, node, occurrence, frame);
    if (!effective.has_value() || effective->empty()) {
        return false;
    }
    const auto found = document.sources.find(*effective);
    if (found == document.sources.end())
        return false;
    key = *effective;
    reference = found->second;
    return true;
}

std::optional<nemo::NodeInstance> ReadSourceController::effectiveNode(const nemo::NetworkId network,
                                                                      const nemo::NodeId node,
                                                                      const nemo::NetworkInstanceId instanceId,
                                                                      const int frame) const {
    const nemo::NodeInstance* instance = nodeInstance(network, node);
    if (instance == nullptr) {
        return std::nullopt;
    }
    nemo::NodeInstance local = *instance;
    // The occurrence's authored overrides are the same composition the evaluator
    // uses (an occurrence override suppresses the definition value), applied to
    // the copy's own map — no second parameter map is built.
    if (instanceId != nemo::kInvalidNetworkInstance) {
        const nemo::NetworkInstance* occurrence = session_.document().instance(instanceId);
        if (occurrence == nullptr) {
            return std::nullopt;
        }
        if (const auto overrides = occurrence->params.find(node); overrides != occurrence->params.end()) {
            for (const auto& [key, value] : overrides->second) {
                local.params[key] = value;
            }
        }
    }
    // Core owns the animation composition (definition + occurrence at a
    // document-local frame); nothing is re-derived here.
    nemo::applyAnimationParameters(session_.document(), network, node, instanceId, static_cast<double>(frame),
                                   local.params);
    return local;
}

bool ReadSourceController::effectiveRequest(const nemo::NetworkId network, const nemo::NodeId node,
                                            const nemo::NetworkInstanceId instanceId, const int frame,
                                            nemo::EffectiveSourceRequest& request, QString& problem) const {
    // The whole composition stays inside the exception boundary, so an invalid
    // or opaque animation value becomes a reported query problem (the editor
    // shows it) instead of an exception escaping into QML.
    try {
        const std::optional<nemo::NodeInstance> instance = effectiveNode(network, node, instanceId, frame);
        if (!instance.has_value()) {
            problem = QStringLiteral("The Read node no longer exists.");
            return false;
        }
        request = nemo::resolveSourceRequest(session_.document(), *instance, frame);
        return true;
    } catch (const std::exception& failure) {
        problem = QString::fromUtf8(failure.what());
        return false;
    }
}

QVariantMap ReadSourceController::info(const QString& networkId, const QVariant& nodeIdValue, const int frame,
                                       const QVariant& instanceValue) const {
    QVariantMap out;
    out.insert(QStringLiteral("state"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("sourceKey"), QString());
    out.insert(QStringLiteral("path"), QString());
    out.insert(QStringLiteral("resolvedPath"), QString());
    out.insert(QStringLiteral("pending"), false);
    out.insert(QStringLiteral("error"), QString());
    out.insert(QStringLiteral("rangeMode"), QStringLiteral("auto"));
    out.insert(QStringLiteral("rangeFirst"), QString());
    out.insert(QStringLiteral("rangeLast"), QString());
    out.insert(QStringLiteral("originalFirst"), QString());
    out.insert(QStringLiteral("originalLast"), QString());
    out.insert(QStringLiteral("originalCount"), QString());
    out.insert(QStringLiteral("missingCount"), QString());
    out.insert(QStringLiteral("coverageQuality"), QStringLiteral("unknown"));
    out.insert(QStringLiteral("selectedFirst"), QString());
    out.insert(QStringLiteral("selectedLast"), QString());
    out.insert(QStringLiteral("frameOffset"), QStringLiteral("0"));
    out.insert(QStringLiteral("frameStep"), QStringLiteral("1"));
    out.insert(QStringLiteral("startAt"), QString());
    out.insert(QStringLiteral("beforePolicy"), QStringLiteral("error"));
    out.insert(QStringLiteral("afterPolicy"), QStringLiteral("error"));
    out.insert(QStringLiteral("missingPolicy"), QStringLiteral("error"));
    out.insert(QStringLiteral("inputTransform"), QStringLiteral("auto"));
    out.insert(QStringLiteral("inputColorSpace"), QString());
    out.insert(QStringLiteral("resolvedInputColorSpace"), QString());
    out.insert(QStringLiteral("inputTransformOrigin"), QString());
    out.insert(QStringLiteral("workingSpace"), QString::fromStdString(session_.document().color.workingSpace));
    out.insert(QStringLiteral("kind"), QStringLiteral("unknown"));
    out.insert(QStringLiteral("width"), 0);
    out.insert(QStringLiteral("height"), 0);
    out.insert(QStringLiteral("pixelAspect"), 0.0);
    out.insert(QStringLiteral("rate"), QString());
    out.insert(QStringLiteral("frameSpan"), QString());
    out.insert(QStringLiteral("precision"), QString());
    out.insert(QStringLiteral("channels"), QString());
    out.insert(QStringLiteral("offline"), false);
    out.insert(QStringLiteral("status"), QString());
    out.insert(QStringLiteral("shared"), false);
    out.insert(QStringLiteral("choiceRequired"), false);
    out.insert(QStringLiteral("choiceAmbiguous"), false);
    out.insert(QStringLiteral("choiceDetail"), QString());
    out.insert(QStringLiteral("choicePath"), QString());
    out.insert(QStringLiteral("choicePattern"), QString());
    out.insert(QStringLiteral("choiceFirst"), QString());
    out.insert(QStringLiteral("choiceLast"), QString());

    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        return out;
    }
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, false)) {
        out.insert(QStringLiteral("error"), QStringLiteral("Unknown media occurrence scope."));
        return out;
    }
    const nemo::ParameterAddress address = targetAddress(network, node, occurrence);
    out.insert(QStringLiteral("pending"), pendingToken_ != 0 && pendingTarget_ == address);
    if (errorTarget_ == address && !error_.isEmpty())
        out.insert(QStringLiteral("error"), error_);
    if (choice_ && choice_->target == address && choice_->projectGeneration == session_.projectGeneration()) {
        out.insert(QStringLiteral("choiceRequired"), true);
        out.insert(QStringLiteral("choiceAmbiguous"), choice_->ambiguous);
        out.insert(QStringLiteral("choiceDetail"), QString::fromStdString(choice_->result.discovery.detail));
        out.insert(QStringLiteral("choicePath"), choice_->literalPath);
        out.insert(QStringLiteral("choicePattern"), choice_->pattern);
        const nemo::media::SequenceDiscovery& discovery = choice_->result.discovery;
        if (discovery.status == nemo::media::SequenceDiscoveryStatus::Sequence) {
            out.insert(QStringLiteral("choiceFirst"), QString::number(discovery.first));
            out.insert(QStringLiteral("choiceLast"), QString::number(discovery.last));
            out.insert(QStringLiteral("choiceCount"), static_cast<qlonglong>(discovery.availableCount));
            out.insert(QStringLiteral("missingCount"), QString::number(discovery.missingCount));
        }
    }

    const nemo::Document& document = session_.document();
    const nemo::NodeInstance* instance = document.network(network).graph().node(node);
    if (instance == nullptr)
        return out;
    // Authored node choices (the single owner of the Read's own settings).
    const nemo::ReadNodeOverrides overrides = nemo::readAuthoredOverrides(document, *instance);
    const bool custom = overrides.rangeMode == nemo::kReadRangeModeCustom;
    out.insert(QStringLiteral("rangeMode"), QString::fromStdString(overrides.rangeMode));
    out.insert(QStringLiteral("rangeFirst"), custom ? QString::number(overrides.rangeFirst) : QString());
    out.insert(QStringLiteral("rangeLast"), custom ? QString::number(overrides.rangeLast) : QString());
    out.insert(QStringLiteral("frameOffset"), QString::number(overrides.frameOffset));
    out.insert(QStringLiteral("frameStep"), QString::number(overrides.frameStep));
    out.insert(QStringLiteral("beforePolicy"), QString::fromStdString(overrides.beforePolicy));
    out.insert(QStringLiteral("afterPolicy"), QString::fromStdString(overrides.afterPolicy));
    out.insert(QStringLiteral("missingPolicy"), QString::fromStdString(overrides.missingPolicy));
    out.insert(QStringLiteral("inputTransform"), QString::fromStdString(overrides.inputTransform));
    out.insert(QStringLiteral("inputColorSpace"), QString::fromStdString(overrides.inputColorSpace));
    out.insert(QStringLiteral("alphaMode"), QString::fromStdString(overrides.alphaMode));
    // Advanced media-interpretation hints (fill-only, core-validated): shown
    // inside the collapsed Advanced group, never as a default-visible section.
    out.insert(QStringLiteral("sourceTransfer"), QString::fromStdString(overrides.sourceTransfer));
    out.insert(QStringLiteral("sourcePrimaries"), QString::fromStdString(overrides.sourcePrimaries));
    out.insert(QStringLiteral("sourceMatrix"), QString::fromStdString(overrides.sourceMatrix));
    out.insert(QStringLiteral("sourceRange"), QString::fromStdString(overrides.sourceRange));
    out.insert(QStringLiteral("sourceChromaLocation"), QString::fromStdString(overrides.sourceChromaLocation));
    if (const auto problem = nemo::readOverridesProblem(overrides)) {
        out.insert(QStringLiteral("error"), QString::fromStdString(*problem));
        out.insert(QStringLiteral("state"), QStringLiteral("unresolved"));
        return out;
    }
    // The EFFECTIVE binding at this frame/occurrence drives the path, facts,
    // summary and the resolved request: a keyed `source` must never be hidden
    // behind the static authored string.
    std::string key = effectiveSourceKey(network, node, occurrence, frame).value_or("");
    if (key.empty()) {
        out.insert(QStringLiteral("state"), QStringLiteral("empty"));
        return out;
    }
    out.insert(QStringLiteral("sourceKey"), QString::fromStdString(key));
    const auto found = document.sources.find(key);
    if (found == document.sources.end())
        return out;
    const nemo::SourceReference& reference = found->second;
    out.insert(QStringLiteral("path"), QString::fromStdString(reference.path));

    // How many Read nodes name this shared media reference (the Reload/relink
    // scope is visibly shared).
    const std::string sourceName{nemo::kReadParamSourceKey};
    int readers = 0;
    for (const nemo::Network& candidate : document.networks()) {
        for (const nemo::NodeInstance& authored : candidate.graph().nodes()) {
            const auto parameter = authored.params.find(sourceName);
            if (parameter == authored.params.end())
                continue;
            const auto* text = std::get_if<std::string>(&parameter->second);
            if (text != nullptr && *text == key)
                ++readers;
        }
    }
    out.insert(QStringLiteral("shared"), readers > 1);

    // Committed shared facts (the catalog is their persistence owner).
    const nemo::MediaProbeMetadata* committedProbe = nullptr;
    nemo::MediaKind committedKind = nemo::MediaKind::Unknown;
    for (const nemo::MediaCatalogEntry& entry : document.mediaCatalog().entries()) {
        if (entry.sourceKey != key || !entry.metadata.committedProbe)
            continue;
        const nemo::MediaProbeMetadata& probe = *entry.metadata.committedProbe;
        committedProbe = &probe;
        committedKind = entry.metadata.kind;
        out.insert(QStringLiteral("originalFirst"), frameText(probe.firstFrame));
        out.insert(QStringLiteral("originalLast"), frameText(probe.lastFrame));
        out.insert(QStringLiteral("originalCount"), frameText(probe.availableFrameCount));
        out.insert(QStringLiteral("missingCount"), frameText(probe.missingFrameCount));
        out.insert(QStringLiteral("coverageQuality"),
                   QString::fromLatin1(nemo::coverageQualityName(probe.coverageQuality)));
        out.insert(QStringLiteral("width"), static_cast<qlonglong>(probe.width));
        out.insert(QStringLiteral("height"), static_cast<qlonglong>(probe.height));
        out.insert(QStringLiteral("precision"), QString::fromStdString(probe.precision));
        out.insert(QStringLiteral("channels"), QString::fromStdString(probe.channels));
        out.insert(QStringLiteral("kind"), kindName(entry.metadata.kind));
        out.insert(QStringLiteral("offline"), entry.metadata.offline);
        if (probe.pixelAspect) {
            out.insert(QStringLiteral("pixelAspect"), *probe.pixelAspect);
        }
        if (probe.rateNumerator && probe.rateDenominator && *probe.rateDenominator != 0) {
            out.insert(QStringLiteral("rate"), QString::number(*probe.rateNumerator) + QStringLiteral("/") +
                                                   QString::number(*probe.rateDenominator));
        }
        if (probe.firstFrame && probe.lastFrame) {
            out.insert(QStringLiteral("frameSpan"), QString::number(*probe.lastFrame - *probe.firstFrame + 1));
        }
        break;
    }

    // The one effective request (core owns mapping, coverage and precedence).
    std::optional<nemo::EffectiveSourceRequest> resolvedRequest;
    nemo::EffectiveSourceRequest request;
    QString problem;
    if (effectiveRequest(network, node, occurrence, frame, request, problem)) {
        resolvedRequest = request;
        out.insert(QStringLiteral("selectedFirst"), frameText(request.mapping.firstFrame));
        out.insert(QStringLiteral("selectedLast"), frameText(request.mapping.lastFrame));
        out.insert(QStringLiteral("status"), QString::fromLatin1(nemo::sourceRequestStatusName(request.status)));
        out.insert(QStringLiteral("originalFirst"), request.originalFirstFrame
                                                        ? frameText(request.originalFirstFrame)
                                                        : out.value(QStringLiteral("originalFirst")).toString());
        out.insert(QStringLiteral("originalLast"), request.originalLastFrame
                                                       ? frameText(request.originalLastFrame)
                                                       : out.value(QStringLiteral("originalLast")).toString());
        out.insert(QStringLiteral("originalCount"), request.originalFrameCount
                                                        ? frameText(request.originalFrameCount)
                                                        : out.value(QStringLiteral("originalCount")).toString());
        out.insert(QStringLiteral("missingCount"), request.missingFrameCount
                                                       ? frameText(request.missingFrameCount)
                                                       : out.value(QStringLiteral("missingCount")).toString());
        if (const auto start = request.mapping.startAt())
            out.insert(QStringLiteral("startAt"), QString::number(*start));
    } else if (out.value(QStringLiteral("error")).toString().isEmpty()) {
        out.insert(QStringLiteral("error"), problem);
    }

    // Resolved path and online state for the frame this node maps local zero
    // to (the node's own mapping, not the shared reference's).
    try {
        const std::int64_t mapped = nemo::mapSourceFrame(overrides.frameOffset, overrides.frameStep, 0);
        const std::string resolved = nemo::media::resolveFramePath(reference.path, mapped);
        out.insert(QStringLiteral("resolvedPath"), QString::fromStdString(resolved));
        const bool exists = std::filesystem::exists(resolved);
        out.insert(QStringLiteral("state"), exists ? QStringLiteral("ready") : QStringLiteral("offline"));
        if (!exists)
            out.insert(QStringLiteral("offline"), true);
    } catch (const std::exception& failure) {
        out.insert(QStringLiteral("state"), QStringLiteral("offline"));
        if (out.value(QStringLiteral("error")).toString().isEmpty())
            out.insert(QStringLiteral("error"), QString::fromUtf8(failure.what()));
    }
    describeInputColor(out, overrides, resolvedRequest.has_value() ? &*resolvedRequest : nullptr, committedProbe,
                       committedKind, out.value(QStringLiteral("resolvedPath")).toString().toStdString());
    return out;
}

QStringList ReadSourceController::inputTransformChoices() const {
    const std::string config = session_.colorConfigPath();
    // The list is the retained generation's own canonical enumeration: one
    // snapshot per project policy, retired only at the project boundary, so an
    // inspector revision never loads the configuration.
    static_cast<void>(inputColorIdentityFor(config, session_.document().color.workingSpace));
    if (!inputColorCache_ || !inputColorCache_->policy().configBacked()) {
        // A legacy project (the non-config-backed working-space sentinel) has no
        // named config input spaces to offer: the honest state is an empty list
        // plus the explicit/Raw choices, and answering must not open $OCIO.
        return {};
    }
    if (colorSpaceCacheValid_ && config == colorSpaceCacheConfig_) {
        return colorSpaceCache_;
    }
    colorSpaceCacheConfig_ = config;
    colorSpaceCacheValid_ = true;
    colorSpaceCache_.clear();
    try {
        for (const std::string& name : inputColorCache_->snapshot().colorSpaces()) {
            colorSpaceCache_.push_back(QString::fromStdString(name));
        }
    } catch (const std::exception&) {
        // An unusable snapshot leaves the list empty; the resolved-origin label
        // and the evaluation error name the offending relationship.
    }
    return colorSpaceCache_;
}

// Retains one input-color context per project color policy and reports its
// configuration's content identity ("" when the policy opens no config).
std::string ReadSourceController::inputColorIdentityFor(const std::string& configPath,
                                                        const std::string& workingSpace) const {
    // ONE generation per project color policy. The generation is dropped (never
    // mutated) at the project boundary, so a read still using the previous
    // generation keeps it alive; the new generation performs its own single
    // configuration load.
    if (!inputColorCache_ || configPath != inputColorPolicyConfig_ || workingSpace != inputColorPolicyWorking_) {
        inputColorPolicyConfig_ = configPath;
        inputColorPolicyWorking_ = workingSpace;
        inputColorIdentity_.clear();
        try {
            inputColorCache_ = std::make_shared<nemo::media::InputColorCache>(
                nemo::media::SourceColorPolicy{configPath, workingSpace});
            // The policy-aware content identity comes from the same snapshot the
            // rules/processors do, and stays empty (no load) for a legacy
            // non-config-backed working space.
            inputColorIdentity_ = inputColorCache_->configIdentity();
        } catch (const std::exception&) {
            inputColorCache_.reset();  // the label keeps the authored choice; the evaluation error names the config
        }
    }
    return inputColorIdentity_;
}

void ReadSourceController::setError(const nemo::ParameterAddress& target, QString message) {
    error_ = std::move(message);
    errorTarget_ = target;
    emit changed();
}

void ReadSourceController::clearError(const nemo::ParameterAddress& target) {
    // Only the scope that just succeeded is cleared; a sibling scope's reported
    // rejection survives its neighbour's success.
    if (!(errorTarget_ == target))
        return;
    error_.clear();
    errorTarget_ = {};
}

nemo::media::InputColorChoice ReadSourceController::colorChoice(const nemo::NetworkId network, const nemo::NodeId node,
                                                                const nemo::NetworkInstanceId occurrence,
                                                                const int frame) const {
    // The SAME composition the evaluation and the inspector query use: the
    // occurrence's overrides and the current-frame animation are applied to the
    // node before its authored choices and merged hints are read.
    const std::optional<nemo::NodeInstance> effective = effectiveNode(network, node, occurrence, frame);
    if (!effective.has_value()) {
        return {};
    }
    const nemo::ReadNodeOverrides overrides = nemo::readAuthoredOverrides(session_.document(), *effective);
    // Reuse this composition; probing must retain node hints even before a
    // source exists and the full frame request cannot yet resolve.
    try {
        const nemo::EffectiveSourceRequest request = nemo::resolveSourceRequest(session_.document(), *effective, frame);
        return colorChoiceFrom(overrides, &request);
    } catch (const std::exception&) {
        return colorChoiceFrom(overrides, nullptr);
    }
}

void ReadSourceController::describeInputColor(QVariantMap& out, const nemo::ReadNodeOverrides& overrides,
                                              const nemo::EffectiveSourceRequest* request,
                                              const nemo::MediaProbeMetadata* probe, const nemo::MediaKind kind,
                                              const std::string& resolvedPath) const {
    // The Color group reports the resolved interpretation and its origin. The
    // media color owner decides both; this adapter only supplies the authored
    // choice and the committed facts. No UI formula and no second precedence.
    static_cast<void>(inputColorIdentityFor(session_.colorConfigPath(), session_.document().color.workingSpace));
    if (!inputColorCache_) {
        return;
    }
    const nemo::media::InputColorChoice choice = colorChoiceFrom(overrides, request);
    nemo::media::EncodedColorFacts facts;
    if (probe != nullptr) {
        facts.formatName = probe->codec;
        if (kind == nemo::MediaKind::Video) {
            // The decoder already validated the stream's own tags; the probe
            // reports them by name.
            facts.clip = true;
            if (const auto transfer = nemo::media::imageTransferFromName(probe->colorTransfer)) {
                facts.transferKnown = true;
                facts.transfer = *transfer;
            }
            if (const auto primaries = nemo::media::imagePrimariesFromName(probe->colorPrimaries)) {
                facts.primariesKnown = true;
                facts.primaries = *primaries;
            }
        } else {
            facts.declaredColorSpace = probe->declaredInputColorSpace;
        }
    }
    try {
        const nemo::media::ResolvedInputColor resolved =
            inputColorCache_->resolve(choice, facts, resolvedPath, "Read source");
        out.insert(QStringLiteral("inputTransformOrigin"),
                   QString::fromLatin1(nemo::media::inputTransformOriginName(resolved.origin)));
        if (resolved.ocio()) {
            out.insert(QStringLiteral("resolvedInputColorSpace"), QString::fromStdString(resolved.colorSpace));
        } else if (resolved.raw()) {
            out.insert(QStringLiteral("resolvedInputColorSpace"), QString());
        } else {
            out.insert(QStringLiteral("resolvedInputColorSpace"),
                       QString::fromLatin1(nemo::media::imageTransferName(resolved.transfer)));
        }
        out.insert(QStringLiteral("workingSpace"), QString::fromStdString(resolved.workingSpace));
    } catch (const std::exception& failure) {
        // A missing config, missing color space or unsupported interpretation
        // names the offending relationship; nothing is silently substituted.
        out.insert(QStringLiteral("inputTransformError"), QString::fromUtf8(failure.what()));
    }
}

void ReadSourceController::beginProbe(const ProbeAction action, const nemo::NetworkId network, const nemo::NodeId node,
                                      const nemo::NetworkInstanceId occurrence, const int frame, QString path) {
    // The ADMITTED target: every probe decision, every reported error and the
    // outstanding-request identity below belong to this exact scope. The repair
    // identity is meaningful only for a Relink/Reload (a Register authors a NEW
    // binding), so it starts empty: a stale pair can never be read by the next
    // completion.
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    pendingSourceKey_.clear();
    pendingReference_ = nemo::SourceReference{};
    nemo::SourceReference candidate;
    candidate.path = path.toStdString();
    candidate.frameOffset = 0;
    candidate.frameStep = 1;
    nemo::media::ProbeAlignment alignment = nemo::media::ProbeAlignment::DiscoverAvailable;
    if (action != ProbeAction::Register) {
        std::string key;
        nemo::SourceReference current;
        // The CURRENT-effective binding at the request's own occurrence/frame:
        // a keyed `source` resolves the key this repair actually targets.
        if (!currentBinding(network, node, occurrence, frame, key, current)) {
            setError(target, action == ProbeAction::Relink
                                 ? QStringLiteral("Relink requires a Read node that already names media.")
                                 : QStringLiteral("Reload requires a Read node that already names media."));
            return;
        }
        nemo::EffectiveSourceRequest request;
        QString problem;
        if (!effectiveRequest(network, node, occurrence, frame, request, problem)) {
            setError(target, problem);
            return;
        }
        // The admitted repair identity, frozen for the completion: the key this
        // node resolves here and the shared reference as it is NOW.
        pendingSourceKey_ = key;
        pendingReference_ = current;
        // Probe the frame this Read actually evaluates (its own mapping), so a
        // relinked or reloaded reference is validated at the right frame.
        candidate = probeReference(current, request);
        candidate.path = action == ProbeAction::Relink ? path.toStdString() : current.path;
        alignment = nemo::media::ProbeAlignment::Established;
    }

    if (pendingToken_ != 0)
        media_.cancelReferenceProbe(pendingToken_);
    const std::uint64_t generation = ++generation_;
    const nemo::NetworkId probeNetwork = network;
    const nemo::NodeId probeNode = node;
    pendingTarget_ = target;
    probeProjectGeneration_ = session_.projectGeneration();
    const std::uint64_t token = media_.requestReferenceProbe(
        std::move(candidate),
        [this, generation, action, probeNetwork, probeNode, occurrence, frame,
         path](const nemo::media::MediaImportResult& result) {
            handleProbe(generation, action, probeNetwork, probeNode, occurrence, frame, path, result);
        },
        alignment, colorChoice(network, node, occurrence, frame));
    if (token == 0) {
        // The shared probe queue refused the request; nothing is outstanding.
        pendingToken_ = 0;
        setError(target, QStringLiteral("The media probe queue is full. Retry the file selection."));
        return;
    }
    pendingToken_ = token;
    emit changed();
}

bool ReadSourceController::commitRegister(const nemo::NetworkId network, const nemo::NodeId node,
                                          const nemo::NetworkInstanceId occurrence, const int frame,
                                          const QString& path, const nemo::media::MediaImportResult& result,
                                          const bool sequence) {
    // The target is the node that owns the parameter, with its occurrence: one
    // atomic command registers the shared asset AND binds this scope's value,
    // and every error it can raise is reported at this same scope.
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    const nemo::NodeInstance* instance = nodeInstance(network, node);
    if (instance == nullptr) {
        setError(target, QStringLiteral("The Read node no longer exists."));
        return false;
    }
    // A FIRST definition bind returns the range to Auto and aligns the mapping
    // so the available first source frame is local zero (source = offset +
    // local * step, applied once). Every other case — a replacement, or any
    // occurrence-scoped bind — PRESERVES the authored choices: Auto is a RANGE
    // policy only, never a mapping policy, and an occurrence owns its own value
    // rather than initializing the definition.
    // A Read counts as ALREADY BOUND when it authors the `source` parameter
    // (including the empty string a Clear writes) or when that address has an
    // animation channel. Only a node with NEITHER is a fresh binding and only
    // then may the initial choices be supplied; a predicate based on a
    // non-empty string would wrongly re-initialize a cleared or keyed Read.
    const std::string sourceName{nemo::kReadParamSourceKey};
    const bool authorsSource = instance->params.find(sourceName) != instance->params.end();
    const bool sourceKeyed = session_.document().animationChannel(nemo::ParameterAddress{
                                 network, node, sourceName, nemo::kInvalidNetworkInstance}) != nullptr;
    const bool firstDefinitionBind = occurrence == nemo::kInvalidNetworkInstance && !authorsSource && !sourceKeyed;
    const bool singleImage = !sequence && isSequence(result);
    const nemo::MediaKind kind = sequence      ? nemo::MediaKind::Sequence
                                 : singleImage ? nemo::MediaKind::Image
                                               : result.kind;
    const nemo::MediaProbeMetadata probe = singleImage ? stillProbe(result.probe) : result.probe;
    std::optional<nemo::ReadNodeOverrides> initialize;
    if (firstDefinitionBind) {
        nemo::ReadNodeOverrides overrides = nemo::readAuthoredOverrides(session_.document(), *instance);
        overrides.rangeMode = std::string(nemo::kReadRangeModeAuto);
        overrides.rangeFirst = probe.firstFrame.value_or(0);
        overrides.rangeLast = probe.lastFrame.value_or(overrides.rangeFirst);
        overrides.frameStep = 1;
        overrides.frameOffset = probe.firstFrame.value_or(0);
        initialize = std::move(overrides);
    }
    const auto assignedKey = std::make_shared<std::string>();
    try {
        const nemo::EditResult edit =
            session_.submit(nemo::registerReadSourceCommand(target, static_cast<double>(frame), path.toStdString(),
                                                            initialize, kind, probe, assignedKey),
                            nemo::EditOptions{session_.revision(), {}});
        if (!edit.committed) {
            setError(target, edit.error ? QString::fromStdString(edit.error->message)
                                        : QStringLiteral("media selection was rejected"));
            return false;
        }
    } catch (const std::exception& failure) {
        setError(target, QString::fromUtf8(failure.what()));
        return false;
    }
    clearError(target);
    emit changed();
    return true;
}

void ReadSourceController::handleProbe(const std::uint64_t generation, const ProbeAction action,
                                       const nemo::NetworkId network, const nemo::NodeId node,
                                       const nemo::NetworkInstanceId occurrence, const int frame, QString path,
                                       const nemo::media::MediaImportResult& result) {
    if (generation != generation_)
        return;  // a later user action superseded this probe
    // The frozen target of the request that started this probe: its result is
    // published (or refused) at that exact scope and frame, never at whatever
    // the node has since been pointed at.
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    pendingToken_ = 0;
    if (session_.projectGeneration() != probeProjectGeneration_) {
        // The project was replaced while the probe ran: integer identities are
        // only comparable within one project, so the result must not publish
        // into the new target.
        emit changed();
        return;
    }
    if (!result.error.empty()) {
        // Validated rejection: unsupported/ambiguous/malformed media is reported
        // with the adapter's own path/format/reason and nothing is authored.
        setError(target, QString::fromStdString(result.error));
        return;
    }
    if (result.offline) {
        setError(target, QStringLiteral("Media path does not exist: ") + path);
        return;
    }
    if (result.probe.status != nemo::MediaProbeStatus::Ready || result.probe.provenance.empty()) {
        setError(target, QStringLiteral("Media probe returned no validated result for ") + path);
        return;
    }

    if (action == ProbeAction::Reload) {
        // The node must still resolve the KEY this probe was requested for at the
        // same scope/frame. A binding re-pointed while the probe ran belongs to
        // another source, so this result is refused instead of being applied to
        // whatever the node now names.
        std::string key;
        nemo::SourceReference current;
        if (!currentBinding(network, node, occurrence, frame, key, current) || key != pendingSourceKey_) {
            setError(target, QStringLiteral("Reload target changed before the probe completed."));
            return;
        }
        try {
            // The FROZEN reference is the expected value: the core command's own
            // check rejects a reference that moved while the probe ran, so no
            // second validation owner is introduced here.
            const nemo::EditResult edit =
                session_.submit(nemo::reloadReadSourceCommand(key, pendingReference_, result.kind, result.probe),
                                nemo::EditOptions{session_.revision(), {}});
            if (!edit.committed) {
                setError(target, edit.error ? QString::fromStdString(edit.error->message)
                                            : QStringLiteral("reload was rejected"));
                return;
            }
        } catch (const std::exception& failure) {
            setError(target, QString::fromUtf8(failure.what()));
            return;
        }
        clearError(target);
        emit changed();
        return;
    }

    if (action == ProbeAction::Relink) {
        std::string key;
        nemo::SourceReference current;
        if (!currentBinding(network, node, occurrence, frame, key, current) || key != pendingSourceKey_) {
            setError(target, QStringLiteral("Relink target changed before the probe completed."));
            return;
        }
        try {
            const nemo::EditResult edit = session_.submit(
                nemo::relinkReadSourceCommand(key, pendingReference_, path.toStdString(), result.kind, result.probe),
                nemo::EditOptions{session_.revision(), {}});
            if (!edit.committed) {
                setError(target, edit.error ? QString::fromStdString(edit.error->message)
                                            : QStringLiteral("relink was rejected"));
                return;
            }
        } catch (const std::exception& failure) {
            setError(target, QString::fromUtf8(failure.what()));
            return;
        }
        clearError(target);
        emit changed();
        return;
    }

    // Register. A numbered selection (a literal numbered file whose run matches
    // several members) asks for the explicit Sequence vs Single Image choice;
    // an AMBIGUOUS numbering (several runs with real siblings) authors nothing
    // and asks for an explicit '#'/'@' pattern instead of guessing which run is
    // the frame; an authored pattern or a plain still binds immediately.
    if (result.discovery.status == nemo::media::SequenceDiscoveryStatus::Ambiguous) {
        PendingChoice pending;
        pending.network = network;
        pending.node = node;
        pending.projectGeneration = session_.projectGeneration();
        pending.occurrence = occurrence;
        pending.target = targetAddress(network, node, occurrence);
        pending.frame = frame;
        pending.literalPath = path;
        pending.ambiguous = true;
        pending.result = result;
        choice_ = std::move(pending);
        clearError(target);
        emit changed();
        return;
    }
    if (isSequence(result) && !hasImagePattern(path)) {
        PendingChoice pending;
        pending.network = network;
        pending.node = node;
        pending.projectGeneration = session_.projectGeneration();
        pending.occurrence = occurrence;
        pending.target = targetAddress(network, node, occurrence);
        pending.frame = frame;
        pending.literalPath = path;
        pending.pattern =
            QString::fromStdString(result.discovery.pattern.empty() ? path.toStdString() : result.discovery.pattern);
        pending.result = result;
        choice_ = std::move(pending);
        clearError(target);
        emit changed();
        return;
    }
    static_cast<void>(commitRegister(network, node, occurrence, frame, path, result, isSequence(result)));
}

bool ReadSourceController::setSourcePath(const QString& networkId, const QVariant& nodeIdValue, const QString& path,
                                         const QVariant& instanceValue, const int frame) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(targetAddress(network, node, nemo::kInvalidNetworkInstance),
                 QStringLiteral("A Read node identity is required."));
        return false;
    }
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true)) {
        return false;
    }
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        setError(target, QStringLiteral("Media path must not be empty; clear the Read node instead."));
        return false;
    }
    std::string key;
    nemo::SourceReference current;
    // The unchanged-path check resolves the SAME scope/frame the probe would, so
    // an animated `source` is compared against the binding this frame shows.
    if (currentBinding(network, node, occurrence, frame, key, current) &&
        nemo::normalizedSourcePath(current.path) == nemo::normalizedSourcePath(trimmed.toStdString())) {
        return true;  // unchanged: no edit, no probe
    }
    // A new selection supersedes the choice recorded for the SAME scope only: a
    // selection on a sibling occurrence (or the definition) is a different
    // panel's decision and leaves this scope's pending choice intact.
    if (choice_ && choice_->target == target)
        choice_.reset();
    beginProbe(ProbeAction::Register, network, node, occurrence, frame, trimmed);
    return true;
}

bool ReadSourceController::confirmSourceChoice(const QString& networkId, const QVariant& nodeIdValue,
                                               const bool sequence, const QVariant& instanceValue) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(targetAddress(network, node, nemo::kInvalidNetworkInstance),
                 QStringLiteral("A Read node identity is required."));
        return false;
    }
    // The confirmation belongs to ONE scope: the occurrence is validated and the
    // WHOLE choice target is compared, so another occurrence (or the definition)
    // can never consume a choice recorded for a sibling Read of the same child
    // node.
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true)) {
        return false;
    }
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    if (!choice_ || choice_->target != target || choice_->projectGeneration != session_.projectGeneration()) {
        // Another scope's confirmation is REFUSED but never DISCARDS the
        // recorded choice: the owning scope can still confirm it. Only a project
        // replacement retires it, because the recorded identities are only
        // comparable within one project.
        if (choice_ && choice_->projectGeneration != session_.projectGeneration())
            choice_.reset();
        setError(target, QStringLiteral("No numbered media selection is waiting for a choice."));
        return false;
    }
    // An ambiguous numbering cannot be confirmed as a sequence. That refusal is
    // a VALIDATION of the requested choice, not a decision: the pending choice
    // stays available (nothing authored, the document unchanged) so the artist
    // can still take the single-image option or type an explicit pattern. The
    // choice is consumed only by a confirmation that is actually applied.
    if (sequence && choice_->pattern.isEmpty()) {
        setError(target, QStringLiteral("The numbered run is ambiguous. Type an explicit '#' or '@' pattern to load a "
                                        "sequence, or choose Single Image."));
        return false;
    }
    PendingChoice pending = std::move(*choice_);
    choice_.reset();
    const QString path = sequence ? pending.pattern : pending.literalPath;
    return commitRegister(network, node, pending.occurrence, pending.frame, path, pending.result, sequence);
}

void ReadSourceController::chooseSource(const QString& networkId, const QVariant& nodeIdValue,
                                        const QVariant& instanceValue, const int frame) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(targetAddress(network, node, nemo::kInvalidNetworkInstance),
                 QStringLiteral("A Read node identity is required."));
        return;
    }
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true)) {
        return;
    }
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    std::filesystem::path startFolder;
    std::string key;
    nemo::SourceReference current;
    if (currentBinding(network, node, occurrence, frame, key, current))
        startFolder = std::filesystem::path(current.path).parent_path();
    const bool started = chooser_.openFiles(
        this,
        // The browse outcome carries the scope and frame the dialog was OPENED
        // at: the pick is bound (and its errors reported) there, not at whatever
        // the panel has since been pointed at.
        [this, network, node, occurrence, frame, target](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled)
                return;  // a cancelled browse leaves the node unchanged
            if (outcome.status == NativeFileChooser::Outcome::Status::Failed) {
                setError(target, outcome.message);
                return;
            }
            QString failure;
            const QStringList paths = chooserLocalPaths(outcome.urls, failure);
            if (!failure.isEmpty()) {
                setError(target, failure);
                return;
            }
            beginProbe(ProbeAction::Register, network, node, occurrence, frame, paths.constFirst());
        },
        QStringLiteral("Choose media"), false, mediaChooserFilters(), startFolder);
    if (!started)
        setError(target, QStringLiteral("Another file dialog is already open."));
}

void ReadSourceController::chooseRelinkSource(const QString& networkId, const QVariant& nodeIdValue,
                                              const QVariant& instanceValue, const int frame) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(targetAddress(network, node, nemo::kInvalidNetworkInstance),
                 QStringLiteral("A Read node identity is required."));
        return;
    }
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true)) {
        return;
    }
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    std::string key;
    nemo::SourceReference current;
    if (!currentBinding(network, node, occurrence, frame, key, current)) {
        setError(target, QStringLiteral("Relink requires a Read node that already names media."));
        return;
    }
    const std::filesystem::path startFolder = std::filesystem::path(current.path).parent_path();
    const bool started = chooser_.openFiles(
        this,
        [this, network, node, occurrence, frame, target](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled)
                return;
            if (outcome.status == NativeFileChooser::Outcome::Status::Failed) {
                setError(target, outcome.message);
                return;
            }
            QString failure;
            const QStringList paths = chooserLocalPaths(outcome.urls, failure);
            if (!failure.isEmpty()) {
                setError(target, failure);
                return;
            }
            beginProbe(ProbeAction::Relink, network, node, occurrence, frame, paths.constFirst());
        },
        QStringLiteral("Relink media"), false, mediaChooserFilters(), startFolder);
    if (!started)
        setError(target, QStringLiteral("Another file dialog is already open."));
}

bool ReadSourceController::relinkSource(const QString& networkId, const QVariant& nodeIdValue, const QString& path,
                                        const QVariant& instanceValue, const int frame) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(targetAddress(network, node, nemo::kInvalidNetworkInstance),
                 QStringLiteral("A Read node identity is required."));
        return false;
    }
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true)) {
        return false;
    }
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        setError(target, QStringLiteral("Media path must not be empty."));
        return false;
    }
    beginProbe(ProbeAction::Relink, network, node, occurrence, frame, trimmed);
    return true;
}

bool ReadSourceController::reloadSource(const QString& networkId, const QVariant& nodeIdValue,
                                        const QVariant& instanceValue, const int frame) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(targetAddress(network, node, nemo::kInvalidNetworkInstance),
                 QStringLiteral("A Read node identity is required."));
        return false;
    }
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true)) {
        return false;
    }
    const nemo::ParameterAddress target = targetAddress(network, node, occurrence);
    // The binding RELOAD repairs is the one this node resolves at the request's
    // own frame/scope (an animated `source` names a different key per frame).
    std::string key;
    nemo::SourceReference current;
    if (!currentBinding(network, node, occurrence, frame, key, current)) {
        setError(target, QStringLiteral("Reload requires a Read node that already names media."));
        return false;
    }
    beginProbe(ProbeAction::Reload, network, node, occurrence, frame, QString::fromStdString(current.path));
    return true;
}

QString ReadSourceController::startAtOffsetValue(const QString& networkId, const QVariant& nodeIdValue,
                                                 const QString& startAt, const int frame,
                                                 const QVariant& instanceValue) const {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        return {};
    }
    std::int64_t local = 0;
    if (!parseInteger(startAt, local)) {
        return {};
    }
    nemo::EffectiveSourceRequest request;
    QString problem;
    // The EFFECTIVE mapping at the requested frame: an animated range/step must
    // resolve the same offset the artist sees, never the stale authored one.
    nemo::NetworkInstanceId occurrence = nemo::kInvalidNetworkInstance;
    if (!resolveOccurrence(instanceValue, network, node, occurrence, true) ||
        !effectiveRequest(network, node, occurrence, frame, request, problem) || !request.mapping.bounded()) {
        return {};
    }
    try {
        return QString::number(nemo::startAtOffset(local, *request.mapping.firstFrame, *request.mapping.lastFrame,
                                                   request.mapping.frameStep));
    } catch (const std::overflow_error&) {
        return {};
    }
}

}  // namespace nemo::ui
