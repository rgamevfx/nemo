#include "DeliveryController.hpp"

#include "nemo/media/DeliveryOutput.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace nemo::ui {
namespace {

// Progress polling cadence while a job is unsettled, the same 200 ms the
// viewer's scheduler poll uses. The timer is stopped the moment every accepted
// job has settled, so an idle panel polls nothing.
constexpr int kRefreshIntervalMs = 200;

// One QML-supplied identity: a non-empty decimal string naming an existing id.
// Identities cross as strings because a document identity exceeds the range a
// QML number represents exactly; an unusable value is refused rather than
// normalised onto 0.
[[nodiscard]] std::optional<std::uint64_t> identity(const QString& value) {
    const QString text = value.trimmed();
    if (text.isEmpty())
        return std::nullopt;
    bool ok = false;
    const auto id = text.toULongLong(&ok);
    if (!ok || id == 0)
        return std::nullopt;
    return id;
}

[[nodiscard]] QVariantList stringList(const std::vector<std::string>& values) {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string& value : values)
        list.push_back(QString::fromStdString(value));
    return list;
}

// One job's documented map. Document identities and per-frame facts cross as
// decimal strings (QML never rounds them), counts and the raster geometry as
// numbers, and the state name comes from the seam's own vocabulary. The job's
// own failure — a refusal the worker's preflight found before any write, a
// per-frame failure, a cancellation — travels on this entry, never in the
// adapter's error slot, so the panel can show the last job's outcome while a
// later submission is still being resolved.
[[nodiscard]] QVariantMap jobMap(const nemo::eval::DeliveryJobInfo& job) {
    QVariantList files;
    files.reserve(static_cast<qsizetype>(job.files.size()));
    for (const nemo::eval::DeliveryFileResult& file : job.files) {
        files.push_back(QVariantMap{
            {QStringLiteral("documentFrame"), QString::number(file.documentFrame)},
            {QStringLiteral("fileFrame"), QString::number(file.fileFrame)},
            {QStringLiteral("path"), QString::fromStdString(file.path)},
            {QStringLiteral("written"), file.written},
            {QStringLiteral("error"), QString::fromStdString(file.error)},
        });
    }
    QVariantMap out;
    out.insert(QStringLiteral("id"), static_cast<qulonglong>(job.id));
    out.insert(QStringLiteral("state"), QString::fromLatin1(nemo::eval::deliveryStateName(job.state)));
    out.insert(QStringLiteral("network"), QString::number(job.network));
    out.insert(QStringLiteral("node"), QString::number(job.node));
    out.insert(QStringLiteral("nodeName"), QString::fromStdString(job.nodeName));
    out.insert(QStringLiteral("totalFrames"), static_cast<qulonglong>(job.totalFrames));
    out.insert(QStringLiteral("writtenFrames"), static_cast<qulonglong>(job.writtenFrames));
    out.insert(QStringLiteral("failedFrames"), static_cast<qulonglong>(job.failedFrames));
    out.insert(QStringLiteral("progress"), job.progress());
    out.insert(QStringLiteral("width"), job.width);
    out.insert(QStringLiteral("height"), job.height);
    out.insert(QStringLiteral("channels"), stringList(job.channels));
    out.insert(QStringLiteral("file"), QString::fromStdString(job.settings.file));
    out.insert(QStringLiteral("overwrite"), job.settings.overwrite);
    out.insert(QStringLiteral("fileType"), QString::fromStdString(job.settings.output.fileType));
    // The delivered format's own settings, so the panel can state what a job
    // produced without re-reading the node the job froze its settings from.
    out.insert(QStringLiteral("profile"), QString::fromStdString(job.settings.output.profile));
    out.insert(QStringLiteral("colorMode"), QString::fromStdString(job.settings.output.colorMode));
    // Quality evidence (story 79): a delivery is always the full-quality
    // reference image, and the staging form it used is stated, so a proxy or a
    // viewer-cache frame can never be mistaken for delivered output.
    out.insert(QStringLiteral("fullQuality"), job.fullQuality);
    out.insert(QStringLiteral("nativeStaging"), job.nativeStaging);
    out.insert(QStringLiteral("error"), QString::fromStdString(job.error));
    out.insert(QStringLiteral("files"), files);
    return out;
}

}  // namespace

DeliveryController::DeliveryController(ProjectSession& session, nemo::eval::DeliveryQueue& queue, QObject* parent)
    : QObject(parent), session_(session),
      // The runtime's ONE queue: it holds the application's device and
      // allocator, and it is stopped before those owners are torn down.
      queue_(queue) {
    refreshTimer_.setInterval(kRefreshIntervalMs);
    connect(&refreshTimer_, &QTimer::timeout, this, &DeliveryController::refresh);
}

void DeliveryController::refresh() {
    const std::vector<nemo::eval::DeliveryJobInfo> accepted = queue_.jobs();
    QVariantList listed;
    listed.reserve(static_cast<qsizetype>(accepted.size()));
    bool busy = false;
    // Newest first: the panel's list is read top-down, and the job just
    // submitted is the one being watched.
    for (auto job = accepted.rbegin(); job != accepted.rend(); ++job) {
        busy = busy || !job->settled();
        listed.push_back(jobMap(*job));
    }
    jobs_ = std::move(listed);
    busy_ = busy;
    // Polling exists only while a job is unsettled: an idle panel costs
    // nothing, and a settled one is not re-polled for a state that cannot
    // change again.
    if (busy_ && !refreshTimer_.isActive())
        refreshTimer_.start();
    else if (!busy_)
        refreshTimer_.stop();
    emit jobsChanged();
}

std::optional<DeliveryController::Target>
DeliveryController::resolveTarget(const QString& networkId, const QString& nodeId, QString& problem) const {
    const std::optional<std::uint64_t> network = identity(networkId);
    const std::optional<std::uint64_t> node = identity(nodeId);
    if (!network || !node) {
        problem = QStringLiteral("Delivery requires a network and node identity; received '%1' / '%2'.")
                      .arg(networkId, nodeId);
        return std::nullopt;
    }
    return Target{static_cast<nemo::NetworkId>(*network), static_cast<nemo::NodeId>(*node)};
}

QVariantMap DeliveryController::transformChoices(const QString& mode) {
    QVariantMap out;
    const std::string requested = mode.toStdString();
    // `raw` writes the working pixels unchanged and `project` resolves the
    // project's own delivery transform: neither names an entry, so neither
    // opens a configuration. Answering is therefore free and honest.
    if (requested != "colorspace" && requested != "display") {
        out.insert(QStringLiteral("choices"), QStringList{});
        out.insert(QStringLiteral("error"), QString());
        return out;
    }
    const std::string config = session_.colorConfigPath();
    const std::uint64_t generation = session_.projectGeneration();
    if (transformValid_ && transformGeneration_ == generation && transformConfig_ == config &&
        transformMode_ == requested) {
        out.insert(QStringLiteral("choices"), transformChoices_);
        out.insert(QStringLiteral("error"), transformError_);
        return out;
    }
    QStringList choices;
    QString problem;
    try {
        for (const std::string& name : nemo::media::deliveryTransformChoices(config, requested))
            choices.push_back(QString::fromStdString(name));
    } catch (const std::exception& error) {
        // The list cannot be discovered (an unusable configuration, a mode the
        // configuration does not state). The reason is stated with the empty
        // list and shown beside the control, never swallowed and never written
        // into the command error slot: this is a repeated query, not a command.
        problem = QString::fromUtf8(error.what());
    }
    transformGeneration_ = generation;
    transformConfig_ = config;
    transformMode_ = requested;
    transformChoices_ = choices;
    transformError_ = problem;
    transformValid_ = true;
    out.insert(QStringLiteral("choices"), choices);
    out.insert(QStringLiteral("error"), problem);
    return out;
}

quint64 DeliveryController::submit(const QString& networkId, const QString& nodeId, const int frame) {
    QString problem;
    const std::optional<Target> target = resolveTarget(networkId, nodeId, problem);
    if (!target) {
        setError(problem);
        return 0;
    }
    std::uint64_t accepted = 0;
    try {
        // The seam copies the snapshot into the accepted job and resolves the
        // request at the CALLER's frame; invalid authored values and a full
        // queue are refused here, with the offending setting named. Destination
        // collisions and everything else that needs the filesystem are resolved
        // by the job's own worker before the first write, and reported on the
        // job — never by the event thread, which never touches a file.
        accepted = queue_.submit(session_.snapshot(), target->network, target->node, static_cast<std::int64_t>(frame),
                                 session_.colorConfigPath());
    } catch (const std::exception& error) {
        setError(QString::fromUtf8(error.what()));
        return 0;
    }
    if (accepted == 0) {
        // The seam returns the id of every accepted job and states every
        // refusal as an exception, so a 0 here is a refusal it did not name:
        // report it rather than leave a silent no-op.
        setError(QStringLiteral("The delivery queue did not accept the job; no reason was reported."));
        return 0;
    }
    clearError();
    // Publishes the new job and starts the refresh timer, which now has
    // something unsettled to watch.
    refresh();
    return accepted;
}

bool DeliveryController::cancel(const quint64 jobId) {
    bool cancelled = false;
    try {
        cancelled = queue_.cancel(jobId);
    } catch (const std::exception& error) {
        setError(QString::fromUtf8(error.what()));
        return false;
    }
    if (!cancelled)
        setError(QStringLiteral("Delivery job %1 is unknown or already settled; nothing was cancelled.").arg(jobId));
    else
        clearError();
    refresh();
    return cancelled;
}

bool DeliveryController::forget(const quint64 jobId) {
    bool forgotten = false;
    try {
        forgotten = queue_.forget(jobId);
    } catch (const std::exception& error) {
        setError(QString::fromUtf8(error.what()));
        return false;
    }
    if (!forgotten)
        setError(QStringLiteral("Delivery job %1 is unknown or still running; its record was kept.").arg(jobId));
    else
        clearError();
    refresh();
    return forgotten;
}

QVariantMap DeliveryController::status(const quint64 jobId) {
    try {
        return jobMap(queue_.status(jobId));
    } catch (const std::exception& error) {
        setError(QString::fromUtf8(error.what()));
        return {};
    }
}

void DeliveryController::setError(QString message) {
    if (error_ == message)
        return;
    error_ = std::move(message);
    emit errorChanged();
}

void DeliveryController::clearError() {
    if (error_.isEmpty())
        return;
    error_.clear();
    emit errorChanged();
}

}  // namespace nemo::ui
