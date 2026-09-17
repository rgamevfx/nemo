#pragma once

// Explicit delivery jobs (issue #94, stories 74-86): the ONE headless
// submit/status/progress/results/cancel interface that the native UI and the
// CLI both consume.
//
// Ownership boundary (issue #94 continuation, #97 under #14): this module owns
// the ORCHESTRATION of one accepted job. Producing pixels stays with the shared
// native executor — `evaluateGpu` over a retained `SourceSession`, i.e. the same
// decode/effect path the desktop viewer runs, at the same full quality — and
// the final device-to-host transfer stays with the GPU owner
// (`gpu::ExportStaging`, charged through the application's own `gpu::Allocator`
// and retained through its `gpu::SubmissionQueue`). There is no CPU
// evaluation fallback and no private device, allocator or scheduler: the queue
// borrows the application's `Instance`/`Device`/`Allocator`, exactly as the
// desktop bootstrap and the CLI's native entry point already create them.
//
// A Write node is ordinary document state whose ordinary evaluation is
// side-effect-free: nothing in this file runs during graph evaluation, only when
// a caller explicitly submits a job. A submitted job retains its own immutable
// Document snapshot, its already-resolved settings (story 83) and its own
// `SourceSession` (decode state, color configuration and retained processors),
// so later edits neither change accepted frames nor implicitly cancel a job, and
// repeating a request cannot duplicate a write because the files it would touch
// already exist.
//
// Files: an EXR is written per frame inside a private, exclusively claimed
// temporary directory beside its final path and published only when it is
// complete, so a completed frame is always a real file and a failed or cancelled
// frame can never masquerade as delivered output (story 82); the directory is
// removed by ownership on every exit path. A movie has ONE output path (no
// `#`/`@` sequence tokens) and is published only after the whole range encoded
// successfully; a cancelled or failed movie creates no file at its final path and
// leaves any file already there untouched.
//
// Destination ownership: submitting claims the exact final paths lexically (no
// filesystem work on the caller's thread), and the worker resolves them as the
// filesystem sees them before it writes anything, so a relative spelling or a
// symlinked directory cannot smuggle a second active delivery onto one file. The
// older accepted job keeps its claim.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/document/Ids.hpp"
#include "nemo/gpu/Allocator.hpp"
#include "nemo/gpu/Device.hpp"
#include "nemo/gpu/Instance.hpp"
#include "nemo/media/DeliveryOutput.hpp"
#include "nemo/media/ImageIO.hpp"

namespace nemo::eval {

// Delivery failures identify the offending file, node or setting (repo rule:
// errors identify the offending relationship).
struct DeliveryException : std::runtime_error {
    DeliveryException(std::string message, std::string path = {})
        : std::runtime_error(std::move(message)), path(std::move(path)) {}

    std::string path;
};

// The settings one job is accepted with. They are authored on a Write node and
// resolved once at submit (story 83), so the frozen copy — not the node — is
// what the job delivers with. The output format/color choices are the media
// owner's `DeliveryOutputOptions`; the file/range/directory policy stays here.
struct DeliverySettings {
    // The output path: for `exr` a still file or a '#'/'@' sequence pattern
    // (consumed verbatim by `media::resolveFramePath`), for `mov`/`mp4` exactly
    // one movie file. Relative paths resolve against the process working
    // directory, exactly as the CLI's other output flags do.
    std::string file;
    bool createDirectories{true};
    // The explicit overwrite authorization (story 80): with it off, a job that
    // would touch an existing file is refused before any write.
    bool overwrite{false};
    // Document frames, inclusive; the file frame is `frame + frameOffset`
    // (story 75). Endpoints are resolved without overflowing int64.
    std::int64_t frameFirst{1};
    std::int64_t frameLast{1};
    std::int64_t frameOffset{0};
    // Format, MOV profile, MP4 bitrate, frame rate and output color (raw /
    // project / colorspace / display, plus an optional LUT applied after the
    // base transform to primary RGB only).
    media::DeliveryOutputOptions output;
};

// One frame a job will deliver.
struct DeliveryFrame {
    std::int64_t documentFrame{0};
    std::int64_t fileFrame{0};
    std::string path;
};

// Preflight of one job (story 80): the exact frames and paths it would write,
// the collisions that refuse it, and the setting error that refuses it. Nothing
// is created, written or removed here.
struct DeliveryPlan {
    DeliverySettings settings;
    std::vector<DeliveryFrame> frames;
    // Final paths that already exist, in frame order. Non-empty means the plan
    // is refused until the caller authorizes overwriting.
    std::vector<std::string> collisions;
    // The refusal reason (empty pattern, an unwritable directory, an unsupported
    // format/profile/color combination, a pattern that cannot name a sequence,
    // ...), or empty when the settings are usable.
    std::string problem;
    // Raster geometry the delivered image has, filled by preflight: the
    // delivered format, not the node's raster. Movies additionally deliver one
    // file, so `frames` names the frames that container carries.
    int width{0};
    int height{0};
    std::vector<std::string> channels;
    // True when the delivered output is one movie container rather than one file
    // per frame.
    bool movie{false};

    [[nodiscard]] bool ok() const { return problem.empty() && (settings.overwrite || collisions.empty()); }
};

enum class DeliveryState { Queued, Running, Completed, Cancelled, Failed };

[[nodiscard]] const char* deliveryStateName(DeliveryState state);

// One frame's outcome. For an EXR sequence a frame is either written (a real,
// atomically published file) or failed with the reason naming that frame's path.
// A movie delivers ONE file, so its frames report their encoding progress
// through `writtenFrames` while `files` stays empty until the container is
// published, and then holds exactly one entry (the movie path).
struct DeliveryFileResult {
    std::int64_t documentFrame{0};
    std::int64_t fileFrame{0};
    std::string path;
    bool written{false};
    std::string error;
};

// One job's complete observable state: progress, per-frame results and the
// job-level failure/cancellation reason (stories 81-82).
struct DeliveryJobInfo {
    std::uint64_t id{0};
    DeliveryState state{DeliveryState::Queued};
    NetworkId network{kInvalidNetwork};
    NodeId node{kInvalidNode};
    std::string networkName;
    std::string nodeName;
    DeliverySettings settings;
    std::size_t totalFrames{0};
    // Frames this job actually delivered (finalized EXR files, or frames encoded
    // into a movie that was then published).
    std::size_t writtenFrames{0};
    std::size_t failedFrames{0};
    // Quality and raster geometry are stated on the job, so a reduced-quality or
    // proxy image can never satisfy a delivery (story 79). Quality is always
    // full here: native evaluation at the authored format's resolution. The
    // geometry is filled by the worker's own preflight — describing a target
    // needs the native source session — so a queued job reports 0 until the
    // worker reaches it, and a refused job reports the refusal instead.
    bool fullQuality{true};
    int width{0};
    int height{0};
    std::vector<std::string> channels;
    // True when this job delivers ONE movie container rather than one file per
    // frame. A movie's frames report encoding progress through `writtenFrames`
    // and its `files` stays empty until the container is published, then holds
    // exactly one entry; a cancelled or failed movie publishes nothing.
    bool movie{false};
    // Native staging evidence (story 84, #97): true once at least one frame's
    // final transfer went through the GPU owner, and the peak bytes that
    // transfer charged to the shared allocator.
    bool nativeStaging{false};
    std::uint64_t stagingBytes{0};
    std::vector<DeliveryFileResult> files;
    // Non-empty when the job was refused or failed as a whole (a collision the
    // caller did not authorize, a missing directory, cancellation).
    std::string error;

    [[nodiscard]] double progress() const {
        return totalFrames == 0 ? 0.0
                                : static_cast<double>(writtenFrames + failedFrames) / static_cast<double>(totalFrames);
    }
    [[nodiscard]] bool settled() const {
        return state == DeliveryState::Completed || state == DeliveryState::Cancelled || state == DeliveryState::Failed;
    }
};

// Resolves a Write node's authored delivery settings at `localTime` through the
// shared effective-parameter seam (instance overrides and animation included).
// Throws DeliveryException naming the node when it is missing, is not a Write
// node, or carries an unsupported output choice.
[[nodiscard]] DeliverySettings deliverySettings(const Document& document, NetworkId network, NodeId node,
                                                std::int64_t localTime);

// The one bounded delivery queue. Jobs run one at a time on a private worker
// thread with their own Document snapshot and their own retained SourceSession:
// submitting never blocks the caller's thread and never blocks on the viewer, and
// a job's lifetime is independent of any viewer destination.
//
// The queue BORROWS the application's native objects (Instance, Device,
// Allocator) and the compiled shader directory; they must outlive it, and it
// never creates a device, an allocator or a renderer of its own. One
// `gpu::ExportStaging` transfer per frame is charged to that allocator and
// retained through the device's own submission queue.
class DeliveryQueue {
public:
    DeliveryQueue(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& shaders, std::size_t maxAcceptedJobs = 8);
    ~DeliveryQueue();
    DeliveryQueue(const DeliveryQueue&) = delete;
    DeliveryQueue& operator=(const DeliveryQueue&) = delete;

    // Accepts one job: the node is resolved and the authored settings are read
    // through the shared parameter seam. Invalid authored values (an unknown
    // format/profile/color mode, an unsupported combination, a frame range this
    // queue cannot accept, a destination already reserved by an active job) are
    // refused HERE, by exception, with the offending value named. Everything
    // that needs the filesystem or real media is refused by the worker BEFORE
    // any write and reported as a failed job, so the UI thread never blocks on a
    // preflight. The document is copied, not referenced.
    std::uint64_t submit(Document document, NetworkId network, NodeId node, std::int64_t localTime,
                         std::string configPath = {});
    std::uint64_t submit(Document document, NetworkId network, NodeId node, DeliverySettings settings,
                         std::int64_t localTime, std::string configPath = {});

    // Blocking preflight (CLI-only): the frames, paths, collisions and raster a
    // job would produce, described through the SAME native source descriptions
    // execution uses. It is a QUERY, not a thrower: an unusable node, an
    // unsupported authored choice, a collision or an undecodable source is
    // reported in `problem` (and `collisions`). It writes nothing, creates
    // nothing and removes nothing, but it does read real media headers, so it
    // must not be called on the UI event thread.
    [[nodiscard]] DeliveryPlan plan(const Document& document, NetworkId network, NodeId node, std::int64_t localTime,
                                    const std::string& configPath = {});
    [[nodiscard]] DeliveryPlan plan(const Document& document, NetworkId network, NodeId node,
                                    const DeliverySettings& settings, std::int64_t localTime,
                                    const std::string& configPath = {});

    // Every accepted job, oldest first.
    [[nodiscard]] std::vector<DeliveryJobInfo> jobs() const;
    // One job's state; throws DeliveryException for an unknown id.
    [[nodiscard]] DeliveryJobInfo status(std::uint64_t job) const;
    // Requests cancellation of a queued or running job. Returns false when the
    // job is unknown or already settled. A running job stops at the next frame
    // boundary; EXR frames already finalized stay on disk and are reported, and a
    // movie leaves no file at its final path.
    [[nodiscard]] bool cancel(std::uint64_t job);
    // Drops a settled job's record. Returns false for an unknown or running job.
    [[nodiscard]] bool forget(std::uint64_t job);

    // Blocks until every accepted job has settled. Tests and one-shot CLI use
    // only; the UI never waits on the event thread.
    void waitForIdle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::eval
