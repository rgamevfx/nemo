#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nemo/eval/ChannelProjection.hpp"
#include "nemo/eval/GpuContribution.hpp"
#include "nemo/eval/GpuExecutor.hpp"
#include "nemo/eval/SourceSession.hpp"
#include "nemo/eval/ViewIntent.hpp"
#include "nemo/eval/ViewerCache.hpp"
#include "nemo/gpu/Bc7.hpp"
#include "nemo/gpu/GpuViewingTransform.hpp"
#include "nemo/media/ViewingTransform.hpp"

namespace nemo::eval {

// The four roles the displayed representation carries (issue #90): the viewer
// projects the composition's named channels into exactly these, and any
// further named channel stays a data channel that no color transform touches.
inline const std::vector<std::string> kViewerPresentationChannels{"R", "G", "B", "A"};

struct ViewerProjection {
    std::array<std::int32_t, 4> roles{-1, -1, -1, -1};
    bool applyViewingTransform{false};
};

// Resolve storage planes and viewing policy together, before lookup/execution.
// Complete identified primary RGB uses named roles regardless of storage order.
// Other selections are data: a single plane is opaque gray; multiple planes
// retain requested order. Empty demand uses the image's actual channel names.
[[nodiscard]] ViewerProjection resolveViewerProjection(const std::vector<std::string>& requested,
                                                       const std::vector<std::string>& channels);

struct ViewerFrame {
    // Immutable, completed display-referred output. Presentation and the
    // asynchronous viewer cache share this ownership; no image copy occurs.
    //
    // A frame carries EXACTLY ONE representation, honestly typed (issue #106):
    // `image` is the live display-referred float result of an executed graph,
    // `replay` is the compressed BC7 frame a validated cached representation
    // was sampled from. A replay is never a float working image and is never an
    // effect input, an accurate picker source or a full-quality bake, so a
    // consumer branches on which one it holds instead of reinterpreting one as
    // the other.
    std::shared_ptr<const gpu::Image> image;
    std::shared_ptr<const gpu::Bc7Image> replay;
    ImageLayout layout;
    // The described output this frame was produced from (issue #88): its actual
    // format, data bounds, pixel aspect, channels and interpretation. Framing
    // consumers read this instead of guessing a global canvas, and because it
    // travels with the frame no per-frame description round trip is needed.
    ImageDescription description;
    EvaluationRequest request;
    std::uint64_t revision{};
    std::uint64_t requestId{};
    bool cacheHit{false};
    // Accepted for asynchronous encoding, not proof of a persisted frame.
    bool cacheQueued{false};
    // The presentation-only display isolation the view asked for (issue #98):
    // RGBA presents the stored RGB opaquely (issue #99), a single identified
    // primary RGB channel of a color-managed layer is isolated in the
    // presentation copy, and alpha is not isolated at all — it is demanded as
    // data and carried in the evaluated image's RGB. It never changes the
    // evaluated frame.
    gpu::ViewerChannel presentationChannel{gpu::ViewerChannel::RGBA};
};

// A validated compressed representation of this exact demand exists, but its
// asynchronous preparation (compressed blocks read/uploaded into the bounded
// ready set) has not finished (issue #106). It is NOT a cache miss: the caller
// must retry the same render rather than fall back to the live graph, so an
// ordinary playback tick can never turn in-flight replay work into a render —
// or a second one. A headless caller polls/retries explicitly; the interactive
// runtime keeps the work pending. The exception carries the identity of the
// representation being prepared, so a caller can coalesce it with the work it
// already has instead of inventing a new one.
struct ViewerReplayPending : std::runtime_error {
    ViewerReplayPending(std::string identity, std::string message)
        : std::runtime_error(std::move(message)), identity(std::move(identity)) {}

    std::string identity;
};

// Worker-confined orchestration over the shared native dependency plan:
// source decode -> native effects -> GPU OCIO. No routine host readback; the
// only device-to-host transfer is the on-demand viewport sample below.
// Matching scene-linear results reuse #9's cache; distinct representations
// coexist. Returned display images are immutable and ready for presentation.
// timeout_ns bounds individual GPU waits, not CPU decoding/compilation or
// the total request. Cache preparation is always asynchronous. Both render
// entry points are worker-confined; the replay call below is the one
// nonblocking query a playback window may make from another thread.
class ViewerSession {
public:
    using CachePublicationGuard = std::function<bool()>;

    // Foreground construction scope (issue #106): while one is alive, the
    // session's asynchronous cache writer defers its own GPU submissions (block
    // encode, compressed upload) so a live frame's presentation construction
    // wins the shared device queue. It is a construction gate, not a device
    // wait: the writer keeps its CPU-side work and submits as soon as the scope
    // ends. The runtime takes it around a live render plus the presentation it
    // builds from that frame, on the worker thread only; it never blocks on
    // anything and never touches the GUI. The scope belongs to the cache that
    // owns the writer — this forwards it under that existing ownership, and a
    // session without a configured cache returns an empty scope, so holding one
    // is always safe.
    [[nodiscard]] ViewerCache::ForegroundScope foregroundScope();

    // `ocioConfigPath` is the project's authored color configuration. Empty
    // keeps the OCIO application default: the $OCIO environment variable is
    // resolved on the first viewing request. A non-empty path overrides it for
    // this session only, without mutating process-global environment state.
    //
    // `contributions` is the session's COMPLETE immutable native inventory
    // (issue #37): the built-in projection plus every installed package's
    // callbacks and metadata the owner assembled. The default is the real
    // built-in-only assembly `builtinGpuContributions()`, which is exactly what
    // this session used to load implicitly — a caller that supplies a list gets
    // its own list, never a merge with a hidden second inventory and never a
    // mutable global registry. The list is moved, once, into the session's own
    // immutable `EffectLibrary` snapshot — the session never borrows a package
    // list that its owner could destroy. Shader loading and compilation stay on
    // the worker that constructs the session.
    ViewerSession(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                  const std::filesystem::path& shaderDirectory, std::string ocioConfigPath = {},
                  std::vector<GpuNodeContribution> contributions = builtinGpuContributions());
    ~ViewerSession();
    ViewerSession(const ViewerSession&) = delete;
    ViewerSession& operator=(const ViewerSession&) = delete;
    // The optional guard is checked before enqueue and by the asynchronous
    // cache writer through publication. It must be thread-safe; captured
    // owners must outlive the session/cache worker. An omitted guard preserves
    // direct callers' revision/generation freshness contract.
    [[nodiscard]] ViewerFrame render(const Document& document, const EvaluationRequest& request,
                                     std::uint64_t timeout_ns = 10'000'000'000ULL, std::uint64_t generation = 0,
                                     ViewerDestination destination = ViewerDestination::Interactive,
                                     CachePublicationGuard publicationGuard = {});
    // The viewer's own entry point (issue #98): ONE worker job resolves the
    // immutable view intent against the current frame's described image — the
    // authored format for exactly its local time, the addressed layer/channel
    // and the demand this view produces — then keys and executes the SAME
    // resolved plan, and returns the frame with the description it was actually
    // produced from. The panel therefore needs no description round trip to
    // state a demand, and a steady frame costs one completed worker request.
    //
    // `resolution` is this destination's Auto hysteresis state. The worker
    // retains it for the destination's lifetime, so a steady view does not
    // oscillate between representations and a neighbouring frame cannot
    // re-resolve it; the frame's request states the effective sampling scale
    // the panel presents. An unavailable layer or channel throws
    // ViewUnavailable, which states that this view has nothing to present and
    // carries the description that refused it, so the caller can adopt the
    // current frame's channels in the same job instead of retrying.
    [[nodiscard]] ViewerFrame render(const Document& document, const ViewIntent& intent,
                                     ViewerResolutionPolicy& resolution, std::uint64_t timeout_ns = 10'000'000'000ULL,
                                     std::uint64_t generation = 0,
                                     ViewerDestination destination = ViewerDestination::Interactive,
                                     CachePublicationGuard publicationGuard = {});
    // The replay-only preparation call (issue #106): the seam the playback
    // window uses to prepare a KNOWN frame or neighbour while transport keeps
    // running. It never describes, plans, evaluates or encodes — a neighbour
    // whose representation is not already validated and ready is simply not
    // replayable, so a speculative frame can never pull the heavy graph in
    // behind a playback tick. Nonblocking; callers serialize access to their
    // destination's resolution policy.
    //
    // `snapshotRevision` is `Document::stateRevision()` of the immutable
    // document snapshot this demand belongs to, computed once by the owner of
    // that snapshot (the scheduler hashes it at admission). It is the snapshot's
    // identity here — never a scheduler id or a request generation — so an
    // ordinary playback tick never fingerprints the document again, and the
    // served frame reports exactly this revision.
    // `resolution` is the destination's retained Auto policy, as for render:
    // cached frame metadata may resolve a new view, but cached density never
    // overrides its current viewport demand.
    [[nodiscard]] std::optional<ViewerFrame> replay(const ViewIntent& intent, std::uint64_t snapshotRevision,
                                                    ViewerResolutionPolicy& resolution,
                                                    ViewerDestination destination = ViewerDestination::Interactive);
    // Replay a previously resolved concrete headless demand under the same
    // immutable stamp. Like the view-intent form, this never receives or plans
    // a Document; an unknown demand is a miss, not permission to render.
    [[nodiscard]] std::optional<ViewerFrame> replay(const EvaluationRequest& request, std::uint64_t snapshotRevision,
                                                    ViewerDestination destination = ViewerDestination::Interactive);
    // Worker-only metadata query (issue #88): the target's authored output
    // description — its actual format, data bounds, pixel aspect, channels and
    // interpretation — resolved through the same shared dependency planner the
    // render path uses, WITHOUT acquiring a pixel or touching the device.
    // `request` identifies the target (network, output, local time); its domain
    // is not consulted, so the caller can ask before it knows the format. Real
    // media is described from the source session's metadata, never by decoding
    // a frame to measure it.
    [[nodiscard]] ImageDescription describe(const Document& document, const EvaluationRequest& request);

    // Issue #102: evaluates `request`'s target in the WORKING space and returns
    // exactly the one pixel it names. `request` must be a single pixel at
    // sampling scale 1 (`canonicalizeRequest` applied), so the transfer a pick
    // charges is bounded by the demand itself; anything wider is refused with
    // EvaluationException rather than quietly turned into a frame download.
    //
    // The demand's channels are the caller's statement of what the sample
    // means: an empty demand is the image's own channels (the working RGB(A) the
    // graph produced), which is what a color picker wants, while the display's
    // layer/channel isolation belongs to the presentation and is deliberately
    // not part of it. Nothing is cached or published: a pick is a read, not a
    // viewer frame, and it never replaces what a destination displays.
    [[nodiscard]] std::array<float, 4> sampleWorkingPixel(const Document& document, const EvaluationRequest& request,
                                                          std::uint64_t timeout_ns = 10'000'000'000ULL);

    // Configures persistent requested-only display cache storage. Setup is
    // worker-side and may allocate media resources; render remains live-first.
    void configureCache(const ViewerCacheOptions& options);
    // Explicit color refresh boundary (project replacement or a deliberate
    // configuration reload): retires the retained viewing programs/LUTs and the
    // source session's retained OCIO processors, so the next render re-reads
    // the configuration content. Nothing polls for file changes; the owner
    // must invoke this boundary after replacing the configuration.
    void refreshColorConfig();

    // Shutdown/headless drain only; throws when asynchronous cache preparation
    // reported an error.
    void flushCache();
    [[nodiscard]] ViewerCacheCounts cacheCounts() const;
    [[nodiscard]] std::optional<ViewerCacheCounts> tryCacheCounts() const;
    // Thread-safe headless/worker freshness signal; may wait for cache setup.
    // Interactive callers use scheduler publication guards instead.
    void supersedeCache(std::uint64_t revision, std::uint64_t generation,
                        ViewerDestination destination = ViewerDestination::Interactive);
    // Forgets a retired destination's publication freshness so a reused id
    // starts clean and capacity is released back to the bounded destination
    // table. Thread-safe; the scheduler already rejects its in-flight work.
    void retireDestination(ViewerDestination destination);

    struct SourceProbe {
        media::ClipInfo info;
        media::DecodeDecision decision;  // Selected candidate before decoding.
    };
    // Opens an independent decoder. May block; do not call on the UI thread.
    [[nodiscard]] SourceProbe probeSource(const Document& document, const std::string& sourceKey) const;
    [[nodiscard]] CacheCounts reuseCounts() const;

private:
    struct ViewingState {
        // Immutable effective OCIO snapshot. A configuration reload creates
        // a new session; changing the policy selects another snapshot.
        media::OcioGpuProgram program;
        std::string identity;
        std::unique_ptr<gpu::GpuViewingTransform> transform;
    };
    [[nodiscard]] ViewingState& viewingStateFor(const ColorPolicy& policy);
    // freshnessMutex_ is held by callers.
    std::uint64_t& generationForLocked(ViewerDestination destination);

    // Validated per-frame metadata (issue #106). One record per resolved
    // demand holds what that demand actually resolved to: the canonical request,
    // the frame's REAL description (a time-varying raster is stored per frame,
    // never assumed range-wide), the effective representation identity derived
    // from the frame's content key, the display isolation its view stated, and
    // the document snapshot plus colour configuration the record was validated
    // against. Records are reachable by three keys into the same store: the full
    // view intent, the canonical concrete request, and the FRAME IDENTITY
    // (network, target, local time). The frame identity is the one both render
    // paths share, so a frame first visited through the concrete-request path —
    // an explicitly populated cache range — is already the frame an equivalent
    // view asks for: the intent is resolved against the stored description and
    // destination's current density policy (pure arithmetic, no graph description
    // or planning) and served only when coverage and sampling agree.
    // With unchanged snapshot and colour stamps, an exact-intent record is
    // authoritative; ordinary playback needs no graph description or planning.
    // Any edit moves the document stamp and the demand
    // is resolved again from the current snapshot, which recomputes the effective
    // key: an unchanged key still hits the frame the cache holds while unrelated
    // valid siblings stay eligible, so a stale session-wide stamp or a naked
    // frame number can never serve the wrong representation.
    struct FrameRecord {
        EvaluationRequest request;
        ImageDescription description;
        std::string cacheIdentity;
        std::uint64_t documentRevision{};
        std::string colorIdentity;
        gpu::ViewerChannel presentationChannel{gpu::ViewerChannel::RGBA};
    };

    // One request's freshness ticket, taken before any lookup or execution.
    struct RequestTicket {
        std::uint64_t requestId{};
        std::uint64_t revision{};
        std::uint64_t generation{};
    };

    // One frame's validated record, matched against a view: the record itself
    // plus the presentation-only isolation THAT VIEW states. The isolation is
    // deliberately not part of the match, because it is applied when the frame
    // is presented and never changes what was evaluated or stored — the
    // representation the record holds is the same image either way.
    struct FrameMatch {
        std::shared_ptr<const FrameRecord> record;
        gpu::ViewerChannel presentationChannel{gpu::ViewerChannel::RGBA};
    };

    [[nodiscard]] static std::string intentRecordKey(ViewerDestination destination, const ViewIntent& intent);
    [[nodiscard]] static std::string requestRecordKey(ViewerDestination destination, const EvaluationRequest& request);
    // The frame identity both render paths share: one target at one local time.
    // It carries no destination, because what a frame IS does not depend on
    // which panel or which range fill asked for it.
    [[nodiscard]] static std::string frameRecordKey(const EvaluationRequest& request);
    // Match a frame record against the current view using the same description
    // and destination sampling policy as the render path. This needs no graph
    // work, but a cached density cannot override a changed viewport or zoom.
    // Empty channels mean all described channels. Unavailable views miss here
    // so the cold path can report them with a fresh description.
    [[nodiscard]] std::optional<FrameMatch> frameRecordForIntent(const ViewIntent& intent, std::uint64_t revision,
                                                                 const std::string& colorIdentity,
                                                                 ViewerResolutionPolicy& resolution) const;
    // The record for `key` when it is still validated against this document
    // snapshot and colour configuration; otherwise nothing and the caller
    // resolves the demand again. Records are immutable and shared, so a lookup
    // never copies one and a concurrent reader keeps the record it holds valid.
    [[nodiscard]] std::optional<std::shared_ptr<const FrameRecord>>
    findRecord(const std::string& key, std::uint64_t revision, const std::string& colorIdentity) const;
    void record(const std::string& key, FrameRecord entry);
    void forgetRecords();
    // Serves one validated record: the typed replay frame when its compressed
    // representation is ready, ViewerReplayPending while it is still loading,
    // and nothing when it must be produced again. `presentationChannel` is the
    // isolation the CALLER's view states — it is applied when the frame is
    // presented and never changes what was stored, so a record may be served to
    // a view that isolates a different channel of the same evaluated image.
    [[nodiscard]] std::optional<ViewerFrame> serveRecorded(const FrameRecord& record, const RequestTicket& ticket,
                                                           gpu::ViewerChannel presentationChannel);
    // Share only the compressed representation, not its producer's target:
    // the validated consumer request and description remain authoritative.
    [[nodiscard]] static ViewerFrame replayFrame(const ViewerCacheResult& result, const EvaluationRequest& request,
                                                 const ImageDescription& description, gpu::ViewerChannel channel,
                                                 std::uint64_t requestId, std::uint64_t revision);
    // Advances this destination's publication freshness and the cache's
    // supersession watermark for one request. Taken before every lookup and
    // execution, so an abandoned older request can never publish.
    [[nodiscard]] RequestTicket beginRequest(const Document& document, std::uint64_t generation,
                                             ViewerDestination destination);
    // The shared execution body of both render entry points: validates the
    // concrete request, plans (or adopts) exactly one region plan, keys it,
    // executes it and turns the result into the displayed representation.
    // `described` is the description plan the view-intent path already resolved
    // for this document, target and local time; the concrete-request path
    // leaves it empty and is planned here. `recordKey` is the full demand's
    // index key; the resolved metadata is published under it, so the next tick
    // for the same demand resolves nothing. Empty means do not index.
    [[nodiscard]] ViewerFrame renderResolved(const Document& document, const EvaluationRequest& request,
                                             std::optional<ImageDescriptionPlan> described, std::string recordKey,
                                             std::uint64_t timeout_ns, RequestTicket ticket,
                                             ViewerDestination destination, CachePublicationGuard publicationGuard,
                                             gpu::ViewerChannel presentationChannel);

    gpu::Instance& instance_;
    gpu::Device& device_;
    gpu::Allocator& allocator_;
    std::string ocioConfigPath_;  // Resolve $OCIO on first viewing request.
    std::filesystem::path shaderDirectory_;
    std::filesystem::path replayShader_;
    SourceSession sources_;
    EffectLibrary effects_;
    ChannelProjection projections_;
    ResultCache<GpuNodeImage> reuse_;
    std::map<std::pair<std::string, std::string>, ViewingState> viewing_;
    std::map<ViewerDestination, std::uint64_t> latestRevisionByDestination_;
    std::map<ViewerDestination, std::uint64_t> latestGenerationByDestination_;
    mutable std::mutex cacheMutex_;
    std::unique_ptr<ViewerCache> cache_;
    mutable std::mutex freshnessMutex_;
    // Worker-confined renders and cross-thread replay probes share this
    // counter, so it is atomic rather than merely monotonic.
    std::atomic<std::uint64_t> nextRequestId_{1};
    // Bounded validated-record index. Dropping the oldest record costs one
    // re-resolve of a demand that is no longer being played; it never changes
    // which representation a demand resolves to.
    static constexpr std::size_t kMaxFrameRecords = 4096;
    mutable std::mutex recordsMutex_;
    std::map<std::string, std::shared_ptr<const FrameRecord>> records_;
    std::deque<std::string> recordOrder_;
};

}  // namespace nemo::eval
