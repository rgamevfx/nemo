#include "ReadSourceController.hpp"

#include "MediaChooserSupport.hpp"
#include "MediaLibraryModel.hpp"
#include "nemo/core/commands/ReadSourceCommands.hpp"
#include "nemo/core/document/Graph.hpp"
#include "nemo/media/ImageIO.hpp"

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

// Empty text leaves the field at the caller's default; a non-empty field must
// parse as a whole number or the edit is rejected.
bool parseIntegerOrDefault(const QString& text, std::int64_t fallback, std::int64_t& out) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        out = fallback;
        return true;
    }
    bool ok = false;
    const qlonglong value = trimmed.toLongLong(&ok);
    if (!ok)
        return false;
    out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] std::optional<std::int64_t> parseRangeBound(const QString& text, bool& ok) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        ok = true;
        return std::nullopt;
    }
    const qlonglong value = trimmed.toLongLong(&ok);
    return ok ? std::optional<std::int64_t>{static_cast<std::int64_t>(value)} : std::nullopt;
}

}  // namespace

ReadSourceController::ReadSourceController(nemo::ProjectSession& session, MediaLibraryModel& media,
                                           NativeFileChooser& chooser, QObject* parent)
    : QObject(parent), session_(session), media_(media), chooser_(chooser) {}

ReadSourceController::~ReadSourceController() {
    if (pendingToken_ != 0)
        media_.cancelReferenceProbe(pendingToken_);
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

bool ReadSourceController::currentBinding(const nemo::NetworkId network, const nemo::NodeId node, std::string& key,
                                          nemo::SourceReference& reference) const {
    const nemo::Document& document = session_.document();
    const nemo::NodeInstance* instance = document.network(network).graph().node(node);
    if (instance == nullptr)
        return false;
    const auto parameter = instance->params.find("source");
    if (parameter == instance->params.end())
        return false;
    const auto* text = std::get_if<std::string>(&parameter->second);
    if (text == nullptr || text->empty())
        return false;
    const auto found = document.sources.find(*text);
    if (found == document.sources.end())
        return false;
    key = *text;
    reference = found->second;
    return true;
}

QVariantMap ReadSourceController::info(const QString& networkId, const QVariant& nodeIdValue) const {
    QVariantMap out;
    out.insert(QStringLiteral("state"), QStringLiteral("unresolved"));
    out.insert(QStringLiteral("sourceKey"), QString());
    out.insert(QStringLiteral("path"), QString());
    out.insert(QStringLiteral("resolvedPath"), QString());
    out.insert(QStringLiteral("frameOffset"), QStringLiteral("0"));
    out.insert(QStringLiteral("frameStep"), QStringLiteral("1"));
    out.insert(QStringLiteral("firstFrame"), QString());
    out.insert(QStringLiteral("lastFrame"), QString());
    out.insert(QStringLiteral("pending"), false);
    out.insert(QStringLiteral("error"), QString());

    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        out.insert(QStringLiteral("state"), QStringLiteral("unresolved"));
        return out;
    }
    out.insert(QStringLiteral("pending"), pendingToken_ != 0 && pendingNetwork_ == network && pendingNode_ == node);
    if (errorNetwork_ == network && errorNode_ == node && !error_.isEmpty())
        out.insert(QStringLiteral("error"), error_);

    const nemo::Document& document = session_.document();
    const nemo::NodeInstance* instance = document.network(network).graph().node(node);
    if (instance == nullptr)
        return out;
    std::string key;
    const auto parameter = instance->params.find("source");
    if (parameter != instance->params.end()) {
        if (const auto* text = std::get_if<std::string>(&parameter->second))
            key = *text;
    }
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
    out.insert(QStringLiteral("frameOffset"), QString::number(reference.frameOffset));
    out.insert(QStringLiteral("frameStep"), QString::number(reference.frameStep));
    out.insert(QStringLiteral("firstFrame"), reference.firstFrame ? QString::number(*reference.firstFrame) : QString());
    out.insert(QStringLiteral("lastFrame"), reference.lastFrame ? QString::number(*reference.lastFrame) : QString());
    try {
        const std::string resolved = nemo::media::resolveFramePath(reference.path, reference.frameAt(0));
        out.insert(QStringLiteral("resolvedPath"), QString::fromStdString(resolved));
        out.insert(QStringLiteral("state"),
                   std::filesystem::exists(resolved) ? QStringLiteral("ready") : QStringLiteral("offline"));
    } catch (const std::exception& failure) {
        out.insert(QStringLiteral("state"), QStringLiteral("offline"));
        out.insert(QStringLiteral("error"), QString::fromUtf8(failure.what()));
    }
    return out;
}

void ReadSourceController::setError(const nemo::NetworkId network, const nemo::NodeId node, QString message) {
    error_ = std::move(message);
    errorNetwork_ = network;
    errorNode_ = node;
    emit changed();
}

void ReadSourceController::beginProbe(const ProbeAction action, const nemo::NetworkId network, const nemo::NodeId node,
                                      QString path) {
    nemo::SourceReference candidate;
    candidate.path = path.toStdString();
    if (action == ProbeAction::Relink) {
        std::string key;
        nemo::SourceReference current;
        if (!currentBinding(network, node, key, current)) {
            setError(network, node, QStringLiteral("Relink requires a Read node that already names media."));
            return;
        }
        // Probe the frame the relinked reference will actually decode.
        candidate = current;
        candidate.path = path.toStdString();
    } else {
        candidate.frameOffset = 0;
        candidate.frameStep = 1;
    }

    if (pendingToken_ != 0)
        media_.cancelReferenceProbe(pendingToken_);
    const std::uint64_t generation = ++generation_;
    const nemo::NetworkId probeNetwork = network;
    const nemo::NodeId probeNode = node;
    pendingNetwork_ = probeNetwork;
    pendingNode_ = probeNode;
    const std::uint64_t token =
        media_.requestReferenceProbe(std::move(candidate), [this, generation, action, probeNetwork, probeNode,
                                                            path](const nemo::media::MediaImportResult& result) {
            handleProbe(generation, action, probeNetwork, probeNode, path, result);
        });
    if (token == 0) {
        // The shared probe queue refused the request; nothing is outstanding.
        pendingToken_ = 0;
        setError(network, node, QStringLiteral("The media probe queue is full. Retry the file selection."));
        return;
    }
    pendingToken_ = token;
    emit changed();
}

void ReadSourceController::handleProbe(const std::uint64_t generation, const ProbeAction action,
                                       const nemo::NetworkId network, const nemo::NodeId node, QString path,
                                       const nemo::media::MediaImportResult& result) {
    if (generation != generation_)
        return;  // a later user action superseded this probe
    pendingToken_ = 0;
    if (!result.error.empty()) {
        // Validated rejection: unsupported/ambiguous/malformed media is reported
        // with the adapter's own path/format/reason and nothing is authored.
        setError(network, node, QString::fromStdString(result.error));
        return;
    }
    if (result.offline) {
        setError(network, node, QStringLiteral("Media path does not exist: ") + path);
        return;
    }
    if (result.probe.status != nemo::MediaProbeStatus::Ready || result.probe.provenance.empty()) {
        setError(network, node, QStringLiteral("Media probe returned no validated result for ") + path);
        return;
    }

    if (action == ProbeAction::Relink) {
        std::string key;
        nemo::SourceReference current;
        if (!currentBinding(network, node, key, current)) {
            setError(network, node, QStringLiteral("Relink target changed before the probe completed."));
            return;
        }
        const nemo::EditResult edit =
            session_.submit(nemo::relinkReadSourceCommand(key, current, path.toStdString(), result.probe),
                            nemo::EditOptions{session_.revision(), {}});
        if (!edit.committed) {
            setError(network, node,
                     edit.error ? QString::fromStdString(edit.error->message) : QStringLiteral("relink was rejected"));
            return;
        }
    } else {
        nemo::ReadSourceTiming timing;
        const nemo::EditResult edit = session_.submit(
            nemo::registerReadSourceCommand(network, node, path.toStdString(), std::move(timing), result.probe),
            nemo::EditOptions{session_.revision(), {}});
        if (!edit.committed) {
            setError(network, node,
                     edit.error ? QString::fromStdString(edit.error->message)
                                : QStringLiteral("media selection was rejected"));
            return;
        }
    }
    error_.clear();
    errorNetwork_ = nemo::kInvalidNetwork;
    errorNode_ = nemo::kInvalidNode;
    emit changed();
}

bool ReadSourceController::setSourcePath(const QString& networkId, const QVariant& nodeIdValue, const QString& path) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(network, node, QStringLiteral("A Read node identity is required."));
        return false;
    }
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        setError(network, node, QStringLiteral("Media path must not be empty; clear the Read node instead."));
        return false;
    }
    std::string key;
    nemo::SourceReference current;
    if (currentBinding(network, node, key, current) &&
        nemo::normalizedSourcePath(current.path) == nemo::normalizedSourcePath(trimmed.toStdString()))
        return true;  // unchanged: no edit, no probe
    beginProbe(ProbeAction::Register, network, node, trimmed);
    return true;
}

void ReadSourceController::chooseSource(const QString& networkId, const QVariant& nodeIdValue) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(network, node, QStringLiteral("A Read node identity is required."));
        return;
    }
    std::filesystem::path startFolder;
    std::string key;
    nemo::SourceReference current;
    if (currentBinding(network, node, key, current))
        startFolder = std::filesystem::path(current.path).parent_path();
    const bool started = chooser_.openFiles(
        this,
        [this, network, node](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled)
                return;  // a cancelled browse leaves the node unchanged
            if (outcome.status == NativeFileChooser::Outcome::Status::Failed) {
                setError(network, node, outcome.message);
                return;
            }
            QString failure;
            const QStringList paths = chooserLocalPaths(outcome.urls, failure);
            if (!failure.isEmpty()) {
                setError(network, node, failure);
                return;
            }
            beginProbe(ProbeAction::Register, network, node, paths.constFirst());
        },
        QStringLiteral("Choose media"), false, mediaChooserFilters(), startFolder);
    if (!started)
        setError(network, node, QStringLiteral("Another file dialog is already open."));
}

void ReadSourceController::chooseRelinkSource(const QString& networkId, const QVariant& nodeIdValue) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(network, node, QStringLiteral("A Read node identity is required."));
        return;
    }
    std::string key;
    nemo::SourceReference current;
    if (!currentBinding(network, node, key, current)) {
        setError(network, node, QStringLiteral("Relink requires a Read node that already names media."));
        return;
    }
    const std::filesystem::path startFolder = std::filesystem::path(current.path).parent_path();
    const bool started = chooser_.openFiles(
        this,
        [this, network, node](NativeFileChooser::Outcome outcome) {
            if (outcome.status == NativeFileChooser::Outcome::Status::Cancelled)
                return;
            if (outcome.status == NativeFileChooser::Outcome::Status::Failed) {
                setError(network, node, outcome.message);
                return;
            }
            QString failure;
            const QStringList paths = chooserLocalPaths(outcome.urls, failure);
            if (!failure.isEmpty()) {
                setError(network, node, failure);
                return;
            }
            beginProbe(ProbeAction::Relink, network, node, paths.constFirst());
        },
        QStringLiteral("Relink media"), false, mediaChooserFilters(), startFolder);
    if (!started)
        setError(network, node, QStringLiteral("Another file dialog is already open."));
}

bool ReadSourceController::relinkSource(const QString& networkId, const QVariant& nodeIdValue, const QString& path) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(network, node, QStringLiteral("A Read node identity is required."));
        return false;
    }
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        setError(network, node, QStringLiteral("Media path must not be empty."));
        return false;
    }
    beginProbe(ProbeAction::Relink, network, node, trimmed);
    return true;
}

bool ReadSourceController::setSourceTiming(const QString& networkId, const QVariant& nodeIdValue,
                                           const QString& frameOffset, const QString& frameStep,
                                           const QString& firstFrame, const QString& lastFrame) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(network, node, QStringLiteral("A Read node identity is required."));
        return false;
    }
    std::string key;
    nemo::SourceReference current;
    if (!currentBinding(network, node, key, current)) {
        setError(network, node, QStringLiteral("Choose a media file before editing its timing."));
        return false;
    }
    std::int64_t offset = 0;
    std::int64_t step = 1;
    if (!parseIntegerOrDefault(frameOffset, 0, offset) || !parseIntegerOrDefault(frameStep, 1, step)) {
        setError(network, node, QStringLiteral("Frame offset and step must be whole numbers."));
        return false;
    }
    bool firstOk = false;
    bool lastOk = false;
    const std::optional<std::int64_t> first = parseRangeBound(firstFrame, firstOk);
    const std::optional<std::int64_t> last = parseRangeBound(lastFrame, lastOk);
    if (!firstOk || !lastOk) {
        setError(network, node, QStringLiteral("First and last frame must be whole numbers or empty."));
        return false;
    }
    nemo::ReadSourceTiming timing;
    timing.frameOffset = offset;
    timing.frameStep = step;
    timing.firstFrame = first;
    timing.lastFrame = last;
    // Interpretation stays the reference's explicit metadata; this control does
    // not guess a working space.
    timing.interpretation = current.interpretation;
    try {
        const nemo::EditResult edit = session_.submit(nemo::setReadSourceTimingCommand(key, current, std::move(timing)),
                                                      nemo::EditOptions{session_.revision(), {}});
        if (!edit.committed) {
            setError(network, node,
                     edit.error ? QString::fromStdString(edit.error->message)
                                : QStringLiteral("timing edit was rejected"));
            return false;
        }
    } catch (const std::exception& failure) {
        setError(network, node, QString::fromUtf8(failure.what()));
        return false;
    }
    error_.clear();
    errorNetwork_ = nemo::kInvalidNetwork;
    errorNode_ = nemo::kInvalidNode;
    emit changed();
    return true;
}

bool ReadSourceController::clearSource(const QString& networkId, const QVariant& nodeIdValue) {
    nemo::NetworkId network = nemo::kInvalidNetwork;
    nemo::NodeId node = nemo::kInvalidNode;
    if (!resolveNode(networkId, nodeIdValue, network, node)) {
        setError(network, node, QStringLiteral("A Read node identity is required."));
        return false;
    }
    if (pendingToken_ != 0) {
        media_.cancelReferenceProbe(pendingToken_);
        pendingToken_ = 0;
        ++generation_;  // a late result can never re-point a cleared node
    }
    const nemo::EditResult edit =
        session_.submit(nemo::setParamCommand(network, node, "source", nemo::ParameterValue{std::string{}}),
                        nemo::EditOptions{session_.revision(), {}});
    if (!edit.committed) {
        setError(network, node,
                 edit.error ? QString::fromStdString(edit.error->message) : QStringLiteral("clear was rejected"));
        return false;
    }
    error_.clear();
    errorNetwork_ = nemo::kInvalidNetwork;
    errorNode_ = nemo::kInvalidNode;
    emit changed();
    return true;
}

}  // namespace nemo::ui
