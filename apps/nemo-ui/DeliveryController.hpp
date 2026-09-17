#pragma once

// Write node delivery (issue #94, stories 74-86): the presentation window onto
// the ONE explicit delivery seam, `nemo::eval::DeliveryQueue`.
//
// Ownership:
//   - This adapter owns the state the panel binds (`jobs`, `busy`, `error`) and
//     BORROWS the queue: the queue is part of the application's viewer runtime
//     because a delivery evaluates through the same native SourceSession /
//     device / allocator owner the runtime already holds, and the runtime stops
//     it before those owners are torn down. Nothing authored lives here: a Write
//     node's delivery settings are document parameters the seam resolves when it
//     accepts a job, and the accepted job retains its own immutable Document
//     snapshot. A later edit, undo or project replacement neither changes
//     accepted frames nor implicitly cancels a job.
//   - It never mutates the document, never creates a history entry, and never
//     reads or drives a viewer: the frame a request is resolved at is the
//     caller's argument — the panel passes its current frame, exactly as the
//     CLI passes its `--frame` — never anything this adapter derives from
//     viewer state. A viewer-cache frame is never what gets written, and the
//     viewer's own display transform is never baked in: an output color
//     transform runs only because the node explicitly authored one.
//   - It performs NO filesystem or media preflight: the queue's worker resolves
//     and refuses a request before the first byte is touched and reports the
//     refusal as that job's own failure naming the offending path. The GUI
//     thread only submits `ProjectSession::snapshot()`, polls the queue's
//     thread-safe queries and asks for cancellation, so no delivery can block
//     the event thread and a pending refusal never leaves an output file.
//   - One timer refreshes `jobs` and runs only while a job is unsettled, so an
//     idle panel polls nothing.
#include "nemo/core/session/ProjectSession.hpp"
#include "nemo/eval/DeliveryJob.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <optional>
#include <string>

namespace nemo::ui {

class DeliveryController final : public QObject {
    Q_OBJECT
    // Every accepted job, newest first, refreshed while any job is unsettled.
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    // Any accepted job that is queued or running: the refresh timer runs
    // exactly while this holds.
    Q_PROPERTY(bool busy READ busy NOTIFY jobsChanged)
    // The last refusal: an identity or job this adapter could not resolve, or a
    // submission/cancellation/forget the seam refused. A successful
    // submit/cancel/forget clears it; the `status` QUERY states a call it could
    // not answer but never clears it. A job's own refusal — including the
    // preflight the seam's worker runs before any write — is stated on that
    // job's entry in `jobs`, never here.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    // `session` is the application's project owner and must outlive this
    // adapter. `queue` is the runtime-owned delivery seam and must also outlive
    // it: the project's authored color configuration is read from the session at
    // submit and at each transform discovery, so this adapter never retains a
    // copy of it.
    DeliveryController(ProjectSession& session, nemo::eval::DeliveryQueue& queue, QObject* parent = nullptr);

    [[nodiscard]] QVariantList jobs() const { return jobs_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] QString error() const { return error_; }

    // The output transforms the ACTIVE project config offers for `mode`:
    // "colorspace" answers the config's colorspace names and "display" its
    // "display/view" pairs; "raw"/"project" resolve no explicit transform and
    // answer an empty list without opening a config. Keys: choices (string
    // list), error (empty when the list was discovered). Discovered once per
    // (project generation, config path, mode) and retained, so an inspector
    // revision never reloads a configuration; a discovery that fails answers
    // empty WITH the reason, which the editor states locally.
    Q_INVOKABLE QVariantMap transformChoices(const QString& mode);

    // Submits one explicit delivery job for a Write node, resolved at `frame`
    // (the panel's current frame), and returns the job's id, or 0 when the
    // request was refused with `error` naming the reason (an unusable identity,
    // an unknown or non-Write node, an unusable setting, a full queue). The
    // seam owns that decision and its preflight; nothing here re-derives either,
    // and nothing here touches the filesystem. The accepted job FREEZES what it
    // resolved then: a later edit, undo or frame change neither changes accepted
    // frames nor cancels the job.
    Q_INVOKABLE quint64 submit(const QString& networkId, const QString& nodeId, int frame);
    // Requests cancellation of a queued or running job: false, with `error`,
    // when the job is unknown or already settled. A running job stops at the
    // next frame boundary; frames already finalized stay on disk and are
    // reported, and a cancelled movie publishes no partial file.
    Q_INVOKABLE bool cancel(quint64 jobId);
    // Drops a settled job's record: false, with `error`, for an unknown or
    // running job.
    Q_INVOKABLE bool forget(quint64 jobId);
    // One job's map: id, state (queued|running|completed|cancelled|failed),
    // network, node, nodeName, totalFrames, writtenFrames, failedFrames,
    // progress (0..1), width, height, channels (string list), file, overwrite,
    // fileType, profile, colorMode, fullQuality, nativeStaging, error, files (a
    // list of {documentFrame, fileFrame, path, written, error}). Identities
    // cross as decimal strings, per-frame facts as decimal strings and
    // counts/geometry as numbers. Empty, with `error`, for an unknown id; this
    // query never clears a refusal a command reported.
    Q_INVOKABLE QVariantMap status(quint64 jobId);

signals:
    void jobsChanged();
    void errorChanged();

private slots:
    void refresh();

private:
    struct Target {
        nemo::NetworkId network{nemo::kInvalidNetwork};
        nemo::NodeId node{nemo::kInvalidNode};
    };
    // The node pair QML addressed, or nothing with `problem` stating the
    // unusable identity: a value that does not parse is never normalised onto
    // 0.
    [[nodiscard]] std::optional<Target> resolveTarget(const QString& networkId, const QString& nodeId,
                                                      QString& problem) const;
    void setError(QString message);
    void clearError();

    ProjectSession& session_;
    // The ONE queue, borrowed from the runtime that owns it: one delivery
    // worker on the application's device/allocator, bounded accepted-job list.
    nemo::eval::DeliveryQueue& queue_;
    // A plain member, never parented: a QObject child that is also a member
    // would be destroyed twice.
    QTimer refreshTimer_;
    QVariantList jobs_;
    bool busy_{false};
    QString error_;

    // The retained transform discovery: one entry per (project generation,
    // config path, mode). A project boundary or a mode change re-discovers; a
    // same-path config edited in place is observed at project reopen, exactly
    // like the Read control's color-space list.
    std::uint64_t transformGeneration_{0};
    std::string transformConfig_;
    std::string transformMode_;
    QStringList transformChoices_;
    QString transformError_;
    bool transformValid_{false};
};

}  // namespace nemo::ui
