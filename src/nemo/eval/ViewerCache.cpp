#include "nemo/eval/ViewerCache.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "nemo/core/Hashing.hpp"
#include "nemo/gpu/Error.hpp"

namespace nemo::eval {
namespace {

using json = nlohmann::json;

// Pack container: one append-only file of self-describing records. Every
// record is independently committed and independently replayable — there is no
// chunk, GOP or temporal grouping — and the record prefix lets a reader reject
// a torn or oversized record without allocating beyond the bytes the file
// actually holds.
inline constexpr char kPackMagic[8] = {'N', 'E', 'M', 'O', 'B', 'C', '7', 'P'};
inline constexpr std::uint32_t kPackSchemaVersion = 1;
inline constexpr std::uint64_t kPackHeaderBytes = 24;
inline constexpr std::uint64_t kRecordPrefixBytes = 32;
inline constexpr std::uint64_t kMaxRecordMetadataBytes = 4ULL * 1024ULL * 1024ULL;
// Roll to a new pack before one grows past this, so reclaiming a pack is
// bounded work rather than a full-cache rewrite.
inline constexpr std::uint64_t kMaxPackBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxSerializedChannels = 256;
inline constexpr int kMaxEncodeAdmissionAttempts = 16;
inline constexpr auto kCompletionPollInterval = std::chrono::microseconds(500);

[[nodiscard]] std::string hexHash(std::uint64_t value) {
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

[[nodiscard]] std::uint64_t parseHexHash(const std::string& text) {
    if (text.size() != 16)
        throw std::runtime_error("cache record has a malformed integrity value");
    std::uint64_t value = 0;
    for (const char digit : text) {
        value <<= 4;
        if (digit >= '0' && digit <= '9')
            value |= static_cast<std::uint64_t>(digit - '0');
        else if (digit >= 'a' && digit <= 'f')
            value |= static_cast<std::uint64_t>(digit - 'a' + 10);
        else
            throw std::runtime_error("cache record has a malformed integrity value");
    }
    return value;
}

[[nodiscard]] std::uint64_t bytesChecksum(const std::uint8_t* bytes, std::size_t size) {
    std::uint64_t hash = kFnv1a64Basis;
    hashMix(hash, bytes, size);
    return hash;
}

// Exact compressed payload of one logical raster. `bc7PayloadBytes` performs
// the checked ceil(width/4)*ceil(height/4)*16 arithmetic the representation is
// defined by, and throws on an unrepresentable extent.
[[nodiscard]] std::uint64_t payloadSize(const ImageLayout& layout) {
    if (layout.width <= 0 || layout.height <= 0)
        throw std::runtime_error("cached frame has no raster extent");
    return static_cast<std::uint64_t>(
        gpu::bc7PayloadBytes(static_cast<std::uint32_t>(layout.width), static_cast<std::uint32_t>(layout.height)));
}

// --- JSON field codecs. Every reader validates shape and range and throws a
// descriptive error, so a foreign or damaged record is rejected instead of
// being guessed at.

[[nodiscard]] json regionToJson(const Region& region) {
    return json{{"x", region.x}, {"y", region.y}, {"width", region.width}, {"height", region.height}};
}

[[nodiscard]] Region regionFromJson(const json& value) {
    return Region{value.at("x").get<int>(), value.at("y").get<int>(), value.at("width").get<int>(),
                  value.at("height").get<int>()};
}

[[nodiscard]] std::vector<std::string> channelsFromJson(const json& value) {
    std::vector<std::string> channels = value.get<std::vector<std::string>>();
    if (channels.empty() || channels.size() > kMaxSerializedChannels)
        throw std::runtime_error("cached frame names an invalid channel list");
    return channels;
}

[[nodiscard]] std::string precisionToJson(Precision precision) {
    switch (precision) {
    case Precision::Float32:
        return "float32";
    }
    return "float32";
}

[[nodiscard]] Precision precisionFromJson(const json& value) {
    if (value.get<std::string>() != "float32")
        throw std::runtime_error("cached frame names an unsupported precision");
    return Precision::Float32;
}

[[nodiscard]] Quality qualityFromJson(const json& value) {
    const std::string name = value.get<std::string>();
    if (name == "full")
        return Quality::Full;
    if (name == "draft")
        return Quality::Draft;
    throw std::runtime_error("cached request names an unknown quality");
}

[[nodiscard]] ImageAssociation associationFromJson(const json& value) {
    const std::string name = value.get<std::string>();
    if (name == "straight")
        return ImageAssociation::Straight;
    if (name == "premultiplied")
        return ImageAssociation::Premultiplied;
    throw std::runtime_error("cached frame names an unknown alpha association");
}

[[nodiscard]] ColorInterpretation colorFromJson(const json& value) {
    const std::string name = value.get<std::string>();
    if (name == "scene-linear")
        return ColorInterpretation::SceneLinear;
    if (name == "display-referred")
        return ColorInterpretation::DisplayReferred;
    if (name == "data")
        return ColorInterpretation::Data;
    throw std::runtime_error("cached frame names an unknown color interpretation");
}

[[nodiscard]] json layoutToJson(const ImageLayout& layout) {
    return json{{"width", layout.width},
                {"height", layout.height},
                {"pixelAspect", layout.pixelAspect},
                {"channels", json(layout.channels)},
                {"precision", precisionToJson(layout.precision)},
                {"color", colorInterpretationName(layout.color)}};
}

[[nodiscard]] ImageLayout layoutFromJson(const json& value) {
    ImageLayout layout;
    layout.width = value.at("width").get<int>();
    layout.height = value.at("height").get<int>();
    layout.pixelAspect = value.at("pixelAspect").get<float>();
    layout.channels = channelsFromJson(value.at("channels"));
    layout.precision = precisionFromJson(value.at("precision"));
    layout.color = colorFromJson(value.at("color"));
    if (layout.width <= 0 || layout.height <= 0 || !(layout.pixelAspect > 0.0F))
        throw std::runtime_error("cached frame has invalid raster geometry");
    return layout;
}

[[nodiscard]] json descriptionToJson(const ImageDescription& description) {
    return json{{"format", regionToJson(description.format)},
                {"dataBounds", regionToJson(description.dataBounds)},
                {"edgeExtension", description.edgeExtension},
                {"pixelAspect", description.pixelAspect},
                {"channels", json(description.channels)},
                {"precision", precisionToJson(description.precision)},
                {"association", imageAssociationName(description.association)},
                {"color", colorInterpretationName(description.color)}};
}

[[nodiscard]] ImageDescription descriptionFromJson(const json& value) {
    ImageDescription description;
    description.format = regionFromJson(value.at("format"));
    description.dataBounds = regionFromJson(value.at("dataBounds"));
    description.edgeExtension = value.at("edgeExtension").get<bool>();
    description.pixelAspect = value.at("pixelAspect").get<float>();
    description.channels = channelsFromJson(value.at("channels"));
    description.precision = precisionFromJson(value.at("precision"));
    description.association = associationFromJson(value.at("association"));
    description.color = colorFromJson(value.at("color"));
    if (hasNoImageFormat(description) || !(description.pixelAspect > 0.0F))
        throw std::runtime_error("cached frame has an invalid description");
    return description;
}

[[nodiscard]] json requestToJson(const EvaluationRequest& request) {
    return json{{"network", request.network},
                {"output", request.output},
                {"localTime", request.localTime},
                {"region", regionToJson(request.region)},
                {"channels", json(request.channels)},
                {"quality", qualityName(request.quality)},
                {"samplingScale", request.samplingScale},
                {"fullWidth", request.fullWidth},
                {"fullHeight", request.fullHeight}};
}

[[nodiscard]] EvaluationRequest requestFromJson(const json& value) {
    EvaluationRequest request;
    request.network = value.at("network").get<NetworkId>();
    request.output = value.at("output").get<NodeId>();
    request.localTime = value.at("localTime").get<std::int64_t>();
    request.region = regionFromJson(value.at("region"));
    if (value.contains("channels")) {
        request.channels = value.at("channels").get<std::vector<std::string>>();
        if (request.channels.size() > kMaxSerializedChannels)
            throw std::runtime_error("cached request names an invalid channel list");
    }
    request.quality = qualityFromJson(value.at("quality"));
    request.samplingScale = value.at("samplingScale").get<int>();
    if (!isSamplingScale(request.samplingScale))
        throw std::runtime_error("cached request names an unsupported sampling scale");
    request.fullWidth = value.at("fullWidth").get<int>();
    request.fullHeight = value.at("fullHeight").get<int>();
    return request;
}

// --- Byte-exact file helpers. The record writer appends to its own handle;
// readers open their own. Nothing here runs with the cache state lock held.

#if defined(_WIN32)
using NativeFile = int;
inline constexpr NativeFile kNoFile = -1;

[[nodiscard]] NativeFile openAppendFile(const std::filesystem::path& path, std::string& error) {
    int handle = -1;
    if (_sopen_s(&handle, path.string().c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_APPEND | _O_BINARY, _S_DENYNO,
                 _S_IREAD | _S_IWRITE) != 0) {
        error = "cannot open cache pack '" + path.string() + "': " + std::strerror(errno);
        return kNoFile;
    }
    return handle;
}

[[nodiscard]] bool writeAllFile(NativeFile handle, const void* data, std::size_t size, std::string& error) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const std::size_t remaining = size - written;
        const unsigned int batch = static_cast<unsigned int>(std::min<std::size_t>(remaining, 1U << 30));
        const int result = ::_write(handle, bytes + written, batch);
        if (result <= 0) {
            error = "cannot write cache pack: " + std::string(std::strerror(errno));
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

[[nodiscard]] bool syncFile(NativeFile handle, std::string& error) {
    if (::_commit(handle) != 0) {
        error = "cannot flush cache pack: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

void closeFile(NativeFile handle) noexcept {
    if (handle != kNoFile)
        ::_close(handle);
}
#else
using NativeFile = int;
inline constexpr NativeFile kNoFile = -1;

[[nodiscard]] NativeFile openAppendFile(const std::filesystem::path& path, std::string& error) {
    const NativeFile handle = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (handle < 0)
        error = "cannot open cache pack '" + path.string() + "': " + std::strerror(errno);
    return handle;
}

[[nodiscard]] bool writeAllFile(NativeFile handle, const void* data, std::size_t size, std::string& error) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(handle, bytes + written, size - written);
        if (result <= 0) {
            if (result < 0 && errno == EINTR)
                continue;
            error = "cannot write cache pack: " + std::string(std::strerror(errno));
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

[[nodiscard]] bool syncFile(NativeFile handle, std::string& error) {
    if (::fsync(handle) != 0) {
        error = "cannot flush cache pack: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

void closeFile(NativeFile handle) noexcept {
    if (handle != kNoFile)
        ::close(handle);
}
#endif

[[nodiscard]] bool readExact(std::ifstream& input, std::uint64_t offset, void* data, std::size_t size) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
        return false;
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(input.gcount()) == size;
}

void writeWord(std::uint64_t value, unsigned char* destination) {
    for (int index = 0; index < 8; ++index) {
        destination[index] = static_cast<unsigned char>(value & 0xFFULL);
        value >>= 8;
    }
}

[[nodiscard]] std::uint64_t readWord(const unsigned char* source) {
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index)
        value = (value << 8) | static_cast<std::uint64_t>(source[index]);
    return value;
}

}  // namespace

// Foreground construction gate (issue #106). The gate is shared state with no
// back-reference to the cache, so a scope that outlives its cache is a no-op
// and can never release through freed memory.
struct ViewerCache::ForegroundGate {
    std::mutex mutex;
    std::condition_variable released;
    std::size_t active{0};
    bool stopped{false};
};

ViewerCache::ForegroundScope::ForegroundScope(std::shared_ptr<ForegroundGate> gate) : gate_(gate) {
    if (gate) {
        std::lock_guard lock(gate->mutex);
        ++gate->active;
    }
}

ViewerCache::ForegroundScope& ViewerCache::ForegroundScope::operator=(ForegroundScope&& other) noexcept {
    if (this != &other) {
        ForegroundScope previous(std::move(*this));
        gate_ = std::move(other.gate_);
    }
    return *this;
}

ViewerCache::ForegroundScope::~ForegroundScope() {
    const std::shared_ptr<ForegroundGate> gate = gate_.lock();
    if (!gate)
        return;
    std::lock_guard lock(gate->mutex);
    if (gate->active != 0)
        --gate->active;
    if (gate->active == 0)
        gate->released.notify_all();
}

struct ViewerCache::Impl {
    struct Contributor {
        std::uint64_t revision{};
        std::uint64_t generation{};
        ViewerDestination destination{ViewerDestination::Interactive};
        std::function<bool()> publicationGuard;
    };
    struct Job {
        ViewerCachePublication publication;
        // Bytes this job retains for the writer: the display image the encoder
        // still needs. Released when the job leaves the pipeline.
        std::uint64_t inputCharge{0};
        std::uint64_t payloadCharge{0};
        bool acceptingContributors{true};
        // Coalescing keeps one image and at most one freshness record per other
        // destination, so a shared frame is encoded once.
        std::vector<Contributor> alternatives;
    };
    // One destination's freshness token, viewed without copying: a publication's
    // primary record and every coalesced alternative answer the same questions.
    struct Freshness {
        std::uint64_t revision;
        std::uint64_t generation;
        ViewerDestination destination;
        const std::function<bool()>& publicationGuard;
    };
    [[nodiscard]] static Freshness freshnessOf(const Job& job) {
        return Freshness{job.publication.revision, job.publication.generation, job.publication.destination,
                         job.publication.publicationGuard};
    }
    [[nodiscard]] static Freshness freshnessOf(const Contributor& contributor) {
        return Freshness{contributor.revision, contributor.generation, contributor.destination,
                         contributor.publicationGuard};
    }
    struct Pack {
        std::filesystem::path path;
        std::uint64_t fileBytes{0};
        std::size_t liveRecords{0};
        std::size_t readers{0};
        bool reclaiming{false};
    };
    struct Entry {
        ImageLayout layout;
        ImageDescription description;
        EvaluationRequest request;
        std::string viewingIdentity;
        // Compressed RAM tier: the exact blocks a replay uploads, so a hit
        // needs no readback and no re-encode.
        std::shared_ptr<const std::vector<std::uint8_t>> blocks;
        std::uint64_t blocksBytes{0};
        bool blocksPending{false};
        // GPU-resident BC7 tier: the only state a Ready answer needs.
        std::shared_ptr<const gpu::Bc7Image> resident;
        std::uint64_t residentBytes{0};
        // Durable pack location.
        std::shared_ptr<Pack> pack;
        std::uint64_t payloadOffset{0};
        std::uint64_t payloadBytes{0};
        std::uint64_t checksum{0};
        bool durable{false};
        bool durableQueued{false};
        std::uint64_t lastUse{0};
    };

    gpu::Device& device;
    gpu::Allocator& allocator;
    std::filesystem::path shaderDirectory;
    std::unique_ptr<gpu::Bc7Encoder> encoder;
    // Foreground construction gate; see ViewerCache::ForegroundScope.
    std::shared_ptr<ForegroundGate> gate{std::make_shared<ForegroundGate>()};
    // One encoder is created once, lazily, by whichever worker needs it first.
    // The encoder documents concurrent use, so the writer and the replay thread
    // never serialize their submissions against each other; only creation is
    // once-per-cache and a capability refusal is terminal for this process.
    std::once_flag encoderOnce;
    std::string encoderFailure;
    gpu::Allocator::EvictorHandle evictor;

    mutable std::mutex mutex;
    std::condition_variable wakeWriter;
    std::condition_variable wakeDurable;
    std::condition_variable wakeReplay;
    std::condition_variable idle;

    std::deque<Job> pending;
    std::optional<Job> activeJob;
    std::deque<std::string> durableBacklog;
    std::deque<std::string> replayQueue;
    std::map<std::string, Entry> entries;
    // Identities with admitted work in flight (encode or replay), by reason.
    std::map<std::string, std::string> loading;
    // Terminal answers that must never be confused with "never seen".
    std::map<std::string, std::string> failed;
    std::map<std::string, std::uint64_t> evicted;
    std::map<std::filesystem::path, std::shared_ptr<Pack>> packs;
    std::map<std::pair<ViewerDestination, std::string>, std::uint64_t> latestGenerationByIdentity;
    std::map<ViewerDestination, std::uint64_t> latestRevisionByDestination;
    std::map<ViewerDestination, std::uint64_t> latestGenerationByDestination;
    ViewerCacheOptions options;
    ViewerCacheCounts count;
    std::thread writerThread;
    std::thread durableThread;
    std::thread replayThread;
    bool configured{false};
    bool stopping{false};
    bool writerActive{false};
    bool durableActive{false};
    bool replayActive{false};
    std::uint64_t tick{0};
    std::uint64_t nextFileId{1};
    int writerLockFd{-1};
    // Non-empty when this cache could not be established in the configured
    // directory. The live rendering path stays authoritative: every lookup
    // reports the identifying reason and never a false Ready.
    std::string unavailable;

    Impl(gpu::Device& deviceRef, gpu::Allocator& allocatorRef, const std::filesystem::path& shaderDirectoryRef)
        : device(deviceRef), allocator(allocatorRef), shaderDirectory(shaderDirectoryRef) {}

    [[nodiscard]] std::filesystem::path namespacePath() const { return options.directory / kViewerCacheRepresentation; }

    void configure(const ViewerCacheOptions& configured);

    // --- accounting. Every charge is owned by exactly one holder and released
    // by that holder, so a tier total can never drift from what it names.
    void chargePendingLocked(std::uint64_t bytes) {
        count.pendingBytes += bytes;
        count.peakPendingFrames = std::max(count.peakPendingFrames, count.pendingFrames + count.activeFrames);
    }
    void releasePendingLocked(std::uint64_t bytes) { count.pendingBytes -= std::min(count.pendingBytes, bytes); }
    void touchLocked(Entry& entry) { entry.lastUse = ++tick; }

    void setErrorLocked(std::string message) {
        ++count.errors;
        count.lastError = std::move(message);
    }

    [[nodiscard]] bool drainedLocked() const {
        return pending.empty() && !writerActive && durableBacklog.empty() && !durableActive && replayQueue.empty() &&
               !replayActive && loading.empty();
    }

    void notifyProgressLocked() {
        if (drainedLocked() || stopping)
            idle.notify_all();
    }

    [[nodiscard]] ViewerCacheCounts snapshotCountsLocked() const {
        ViewerCacheCounts result = count;
        // Live quantities only; every byte/charge total is maintained by its
        // owner as it changes. Never waits, never blocks the caller.
        result.pendingFrames = pending.size();
        result.activeFrames = writerActive ? 1U : 0U;
        result.loadingFrames = loading.size();
        return result;
    }

    [[nodiscard]] bool payloadEstimate(const ImageLayout& layout, std::uint64_t& bytes) const noexcept {
        try {
            bytes = payloadSize(layout);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // --- terminal states -----------------------------------------------------
    void rememberFailedLocked(const std::string& identity, std::string diagnostic) {
        failed[identity] = std::move(diagnostic);
        trimTombstonesLocked();
    }

    void rememberEvictedLocked(const std::string& identity) {
        ++count.evictedFrames;
        evicted[identity] = tick;
        trimTombstonesLocked();
    }

    void trimTombstonesLocked() {
        while (evicted.size() > options.maxMetadataEntries) {
            auto oldest = evicted.begin();
            for (auto it = evicted.begin(); it != evicted.end(); ++it) {
                if (it->second < oldest->second)
                    oldest = it;
            }
            evicted.erase(oldest);
        }
        while (failed.size() > options.maxMetadataEntries)
            failed.erase(failed.begin());
    }

    void clearTerminalLocked(const std::string& identity) {
        failed.erase(identity);
        evicted.erase(identity);
    }

    // --- eviction ------------------------------------------------------------
    // A record's compressed copy is charged to pending work until its durable
    // attempt finishes and to the RAM tier afterwards. Every release below
    // therefore releases the tier the charge is actually in.
    void releaseBlocksChargeLocked(const Entry& entry) {
        if (!entry.blocks)
            return;
        if (entry.blocksPending)
            releasePendingLocked(entry.blocksBytes);
        else
            count.compressedHotBytes -= std::min(count.compressedHotBytes, entry.blocksBytes);
    }

    void eraseEntryLocked(const std::string& identity) {
        const auto it = entries.find(identity);
        if (it == entries.end())
            return;
        if (it->second.resident) {
            count.residentBytes -= std::min(count.residentBytes, it->second.residentBytes);
            if (count.residentFrames != 0)
                --count.residentFrames;
            it->second.resident.reset();
        }
        releaseBlocksChargeLocked(it->second);
        retireLocationLocked(it->second);
        entries.erase(it);
    }

    void takeResidentLocked(const std::string& identity) {
        const auto it = entries.find(identity);
        if (it == entries.end() || !it->second.resident)
            return;
        count.residentBytes -= std::min(count.residentBytes, it->second.residentBytes);
        if (count.residentFrames != 0)
            --count.residentFrames;
        it->second.residentBytes = 0;
        it->second.resident.reset();
    }

    // Eligible residency for an external admission request or for the cache's
    // own budget: never a frame whose preparation is in flight.
    [[nodiscard]] std::string oldestEvictableResidentLocked(const std::string& exclude) const {
        std::string victim;
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (const auto& [identity, entry] : entries) {
            if (!entry.resident || identity == exclude || loading.contains(identity) ||
                entry.resident.use_count() != 1 || entry.resident->image.retain().use_count() != 2)
                continue;
            if (entry.lastUse < oldest) {
                oldest = entry.lastUse;
                victim = identity;
            }
        }
        return victim;
    }

    void enforceResidentBudgetLocked(const std::string& keep, std::uint64_t incoming) {
        while (incoming > options.maxResidentBytes - std::min(count.residentBytes, options.maxResidentBytes)) {
            const std::string victim = oldestEvictableResidentLocked(keep);
            if (victim.empty())
                return;
            takeResidentLocked(victim);
        }
    }

    void enforceRamBudgetLocked() {
        while (count.compressedHotBytes > options.maxRamBytes) {
            std::string victim;
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (const auto& [identity, entry] : entries) {
                // Only a committed copy is RAM-tier eligible: a copy still
                // awaiting its durable attempt is charged to pending work and
                // is that attempt's input.
                if (!entry.blocks || entry.blocksPending || entry.blocks.use_count() != 1)
                    continue;
                if (entry.lastUse < oldest) {
                    oldest = entry.lastUse;
                    victim = identity;
                }
            }
            if (victim.empty())
                return;
            auto& entry = entries.at(victim);
            count.compressedHotBytes -= std::min(count.compressedHotBytes, entry.blocksBytes);
            entry.blocksBytes = 0;
            entry.blocks.reset();
            // With nothing left to replay from, the record is gone: report an
            // eviction rather than an absent identity.
            if (!entry.pack && !entry.resident) {
                entries.erase(victim);
                rememberEvictedLocked(victim);
            }
        }
    }

    // Logical retirement does not release physical disk bytes. Pack reclamation
    // owns that charge until the actual file has been removed.
    void retireLocationLocked(Entry& entry) {
        if (!entry.pack)
            return;
        if (entry.pack->liveRecords != 0)
            --entry.pack->liveRecords;
        entry.pack.reset();
        entry.payloadOffset = entry.payloadBytes = 0;
        entry.checksum = 0;
        entry.durable = false;
    }

    // --- external eviction seam (allocator admission) ------------------------
    // Runs on the allocating thread with no allocator lock held and returns the
    // bytes this cache actually gave up. Dropping only this cache's own
    // reference can never free a frame a viewer still holds; it stops counting
    // on cache residency, which is exactly the eligible set.
    //
    // This cache never allocates GPU memory while holding its own lock, but the
    // callback must still never wait forever: a bounded try-lock keeps a
    // re-entrant allocation (which would otherwise deadlock) honest by
    // returning 0 — "nothing eligible was released" — instead of blocking.
    [[nodiscard]] std::uint64_t evictEligibleResidency(std::uint64_t wantedBytes) {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        bool acquired = lock.owns_lock();
        for (int attempt = 0; !acquired && attempt < 128; ++attempt) {
            std::this_thread::yield();
            acquired = lock.try_lock();
        }
        if (!lock.owns_lock())
            return 0;
        std::uint64_t released = 0;
        while (released < wantedBytes) {
            const std::string victim = oldestEvictableResidentLocked({});
            if (victim.empty())
                break;
            released += entries.at(victim).residentBytes;
            takeResidentLocked(victim);
        }
        return released;
    }

    // --- writer lock ---------------------------------------------------------
    void acquireWriterLockLocked();
    void releaseWriterLock() noexcept;

    // Creates the process's one BC7 encoder on first use. A device that cannot
    // carry BC7 or a missing kernel is reported as cache unavailability with
    // the encoder's own identifying message, and live rendering is untouched.
    [[nodiscard]] bool ensureEncoder(std::string& error) {
        std::call_once(encoderOnce, [this] {
            try {
                encoder = std::make_unique<gpu::Bc7Encoder>(device, allocator, shaderDirectory);
            } catch (const std::exception& failure) {
                encoderFailure = std::string("viewer cache BC7 encoding unavailable: ") + failure.what();
            }
        });
        if (encoder)
            return true;
        error = encoderFailure.empty() ? std::string("viewer cache BC7 encoder is unavailable") : encoderFailure;
        return false;
    }

    // Defers a GPU submission while a foreground construction scope is alive.
    // Called with NO cache lock held: it waits only on the gate's own mutex, so
    // a re-entrant allocation or a foreground render can never deadlock with
    // it. Wakes on shutdown as well as on release.
    void waitForForeground() const {
        std::unique_lock lock(gate->mutex);
        gate->released.wait(lock, [&] { return gate->active == 0 || gate->stopped; });
    }

    // Worker-side readiness observation, never a presentation-thread wait.
    // Cancellation retires our interest, not the queue's completion-owned
    // resources; encode and upload therefore share this shutdown boundary.
    [[nodiscard]] bool observeCompletion(gpu::SubmissionQueue::Completion completion) {
        auto& queue = device.submissions(device.graphics_family());
        while (!queue.poll(completion)) {
            {
                std::lock_guard lock(mutex);
                if (stopping)
                    return false;
            }
            std::this_thread::sleep_for(kCompletionPollInterval);
        }
        return true;
    }

    [[nodiscard]] bool staleLocked(const Job& job) const;
    void retireGenerationLocked(const Job& job);
    bool admitDestinationLocked(ViewerDestination destination);
    void mergeContributorLocked(Job& job, ViewerCachePublication publication);

    // --- index ---------------------------------------------------------------
    void loadIndexLocked();
    void scanPackLocked(const std::filesystem::path& path);

    // --- writer -------------------------------------------------------------
    void writerLoop();
    void encodeJob(Job& job);
    void encodeJobWork(Job& job, std::optional<std::string>& failure);
    bool publishResidencyLocked(const std::string& identity, std::shared_ptr<const gpu::Bc7Image> image,
                                std::uint64_t chargedBytes, std::string& diagnostic);

    // --- durability ---------------------------------------------------------
    void durableLoop();
    void persistJob(const std::string& identity, const std::shared_ptr<const std::vector<std::uint8_t>>& blocks,
                    const std::string& viewingIdentity, const ImageLayout& layout, const ImageDescription& description,
                    const EvaluationRequest& request, NativeFile& activeHandle, std::shared_ptr<Pack>& activePack);
    bool makeDiskRoom(std::uint64_t bytes, NativeFile& activeHandle, std::shared_ptr<Pack>& activePack);

    // --- replay -------------------------------------------------------------
    void replayLoop();
    void replayFrame(const std::string& identity);

    void stop() noexcept;
};

void ViewerCache::Impl::acquireWriterLockLocked() {
    std::error_code error;
    std::filesystem::create_directories(options.directory, error);
    if (error)
        throw std::runtime_error("viewer cache: cannot create directory '" + options.directory.string() +
                                 "': " + error.message());
    const auto lockPath = options.directory / ".nemo-viewer-cache.lock";
#if defined(_WIN32)
    int fd = -1;
    if (_sopen_s(&fd, lockPath.string().c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) !=
        0) {
        if (errno == EACCES || errno == EBUSY)
            throw std::runtime_error("viewer cache directory is already in use: " + options.directory.string());
        throw std::runtime_error("viewer cache: cannot acquire writer lock '" + lockPath.string() +
                                 "': " + std::strerror(errno));
    }
#else
    const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::runtime_error("viewer cache: cannot open writer lock '" + lockPath.string() +
                                 "': " + std::strerror(errno));
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int failure = errno;
        ::close(fd);
        if (failure == EWOULDBLOCK || failure == EAGAIN)
            throw std::runtime_error("viewer cache directory is already in use: " + options.directory.string());
        throw std::runtime_error("viewer cache: cannot acquire writer lock '" + lockPath.string() +
                                 "': " + std::strerror(failure));
    }
#endif
    writerLockFd = fd;
}

void ViewerCache::Impl::releaseWriterLock() noexcept {
    if (writerLockFd < 0)
        return;
#if defined(_WIN32)
    ::_close(writerLockFd);
#else
    ::flock(writerLockFd, LOCK_UN);
    ::close(writerLockFd);
#endif
    writerLockFd = -1;
}

bool ViewerCache::Impl::staleLocked(const Job& job) const {
    const auto staleContributor = [&](const Freshness& contributor) {
        if (contributor.publicationGuard && !contributor.publicationGuard())
            return true;
        const auto revision = latestRevisionByDestination.find(contributor.destination);
        if (revision != latestRevisionByDestination.end() && contributor.revision != revision->second)
            return true;
        const auto identity =
            latestGenerationByIdentity.find(std::pair{contributor.destination, job.publication.identity});
        return identity != latestGenerationByIdentity.end() && contributor.generation < identity->second;
    };
    if (!staleContributor(freshnessOf(job)))
        return false;
    return std::all_of(job.alternatives.begin(), job.alternatives.end(),
                       [&](const Contributor& contributor) { return staleContributor(freshnessOf(contributor)); });
}

void ViewerCache::Impl::retireGenerationLocked(const Job& job) {
    const auto retireContributor = [&](const Freshness& contributor) {
        const auto it = latestGenerationByIdentity.find(std::pair{contributor.destination, job.publication.identity});
        if (it != latestGenerationByIdentity.end() && it->second == contributor.generation)
            latestGenerationByIdentity.erase(it);
    };
    retireContributor(freshnessOf(job));
    for (const auto& contributor : job.alternatives)
        retireContributor(freshnessOf(contributor));
}

bool ViewerCache::Impl::admitDestinationLocked(ViewerDestination destination) {
    if (latestGenerationByDestination.contains(destination) ||
        latestGenerationByDestination.size() < kMaxViewerDestinations)
        return true;
    ++count.admissionRejected;
    count.lastError = "viewer cache destination " + std::to_string(static_cast<std::uint32_t>(destination)) +
                      " exceeds destination capacity " + std::to_string(kMaxViewerDestinations);
    return false;
}

void ViewerCache::Impl::mergeContributorLocked(Job& job, ViewerCachePublication publication) {
    const bool primary = job.publication.destination == publication.destination;
    const auto found = std::find_if(job.alternatives.begin(), job.alternatives.end(),
                                    [&](const auto& origin) { return origin.destination == publication.destination; });
    const bool append = !primary && found == job.alternatives.end();
    if (append && job.alternatives.size() == job.alternatives.capacity())
        job.alternatives.reserve(std::max(job.alternatives.size() + 1, job.alternatives.capacity() * 2));
    // Reserve before touching the generation map; the remaining moves cannot
    // allocate, so a failed admission cannot leave orphan metadata.
    latestGenerationByIdentity[{publication.destination, job.publication.identity}] = publication.generation;
    if (primary) {
        job.publication.revision = publication.revision;
        job.publication.generation = publication.generation;
        job.publication.publicationGuard = std::move(publication.publicationGuard);
    } else if (append) {
        job.alternatives.push_back({publication.revision, publication.generation, publication.destination,
                                    std::move(publication.publicationGuard)});
    } else {
        found->revision = publication.revision;
        found->generation = publication.generation;
        found->publicationGuard = std::move(publication.publicationGuard);
    }
}

// --- index -------------------------------------------------------------------

void ViewerCache::Impl::loadIndexLocked() {
    std::error_code error;
    const auto directory = namespacePath();
    std::filesystem::create_directories(directory, error);
    if (error)
        throw std::runtime_error("viewer cache: cannot create namespace '" + directory.string() +
                                 "': " + error.message());
    // Only this cache's own versioned namespace is ever read, indexed or
    // removed: source media, exports, project data and any older disposable
    // cache layout are never opened and never deleted.
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        if (error)
            break;
        error.clear();
        if (item.is_symlink(error) || error) {
            error.clear();
            continue;
        }
        if (!item.is_regular_file(error) || error)
            continue;
        const std::string name = item.path().filename().string();
        if (item.path().extension() != ".pack" || name.rfind("pack-", 0) != 0)
            continue;
        std::uint64_t fileId = 0;
        const auto parsed = std::from_chars(name.data() + 5, name.data() + name.size(), fileId);
        if (parsed.ec == std::errc{} && fileId < std::numeric_limits<std::uint64_t>::max())
            nextFileId = std::max(nextFileId, fileId + 1);
        scanPackLocked(item.path());
    }
    if (error)
        throw std::runtime_error("viewer cache: cannot scan namespace '" + directory.string() +
                                 "': " + error.message());
}

void ViewerCache::Impl::scanPackLocked(const std::filesystem::path& path) {
    std::error_code error;
    const std::uint64_t fileBytes = std::filesystem::file_size(path, error);
    if (error)
        return;
    const auto pack = std::make_shared<Pack>();
    pack->path = path;
    pack->fileBytes = fileBytes;
    count.diskBytes += fileBytes;
    packs.emplace(path, pack);
    std::ifstream input(path, std::ios::binary);
    unsigned char header[kPackHeaderBytes]{};
    if (!input || fileBytes < kPackHeaderBytes || !readExact(input, 0, header, sizeof(header)) ||
        std::memcmp(header, kPackMagic, sizeof(kPackMagic)) != 0 || readWord(header + 8) != kPackSchemaVersion ||
        readWord(header + 16) != kPackHeaderBytes) {
        ++count.invalidEntries;
        setErrorLocked("cache pack has an unsupported or incomplete header: " + path.string());
        return;
    }
    std::uint64_t offset = kPackHeaderBytes;
    while (fileBytes - offset >= kRecordPrefixBytes) {
        unsigned char prefix[kRecordPrefixBytes]{};
        if (!readExact(input, offset, prefix, sizeof(prefix)))
            break;
        const auto recordBytes = readWord(prefix);
        const auto metadataBytes = readWord(prefix + 8);
        const auto payloadBytes = readWord(prefix + 16);
        if (recordBytes < kRecordPrefixBytes || recordBytes > fileBytes - offset || metadataBytes == 0 ||
            metadataBytes > kMaxRecordMetadataBytes || metadataBytes > recordBytes - kRecordPrefixBytes ||
            payloadBytes == 0 || payloadBytes != recordBytes - kRecordPrefixBytes - metadataBytes)
            break;
        std::string metadata(static_cast<std::size_t>(metadataBytes), '\0');
        if (!readExact(input, offset + kRecordPrefixBytes, metadata.data(), metadata.size()))
            break;
        try {
            if (bytesChecksum(reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size()) !=
                readWord(prefix + 24))
                throw std::runtime_error("cache record metadata checksum mismatch");
            const json parsed = json::parse(metadata);
            if (parsed.at("format").get<std::string>() != kViewerCacheRepresentation ||
                parsed.at("schema").get<std::uint32_t>() != kPackSchemaVersion)
                throw std::runtime_error("cache record has a foreign format");
            const auto identity = parsed.at("identity").get<std::string>();
            if (identity.empty())
                throw std::runtime_error("cache record has an empty identity");
            Entry entry;
            entry.viewingIdentity = parsed.at("viewingIdentity").get<std::string>();
            entry.layout = layoutFromJson(parsed.at("layout"));
            entry.description = descriptionFromJson(parsed.at("description"));
            entry.request = requestFromJson(parsed.at("request"));
            if (parsed.at("payloadBytes").get<std::uint64_t>() != payloadBytes ||
                payloadSize(entry.layout) != payloadBytes)
                throw std::runtime_error("cache record length does not match its raster");
            entry.checksum = parseHexHash(parsed.at("checksum").get<std::string>());
            if (entries.contains(identity))
                eraseEntryLocked(identity);
            if (entries.size() >= options.maxMetadataEntries) {
                ++count.admissionRejected;
                throw std::runtime_error("cache metadata capacity rejected an indexed record");
            }
            entry.pack = pack;
            entry.payloadOffset = offset + kRecordPrefixBytes + metadataBytes;
            entry.payloadBytes = payloadBytes;
            entry.durable = true;
            entry.lastUse = ++tick;
            ++pack->liveRecords;
            entries.emplace(identity, std::move(entry));
        } catch (const std::exception& invalid) {
            ++count.invalidEntries;
            setErrorLocked(path.string() + ": " + invalid.what());
        }
        offset += recordBytes;
    }
    if (offset != fileBytes) {
        ++count.invalidEntries;
        setErrorLocked("cache pack has a truncated or invalid record tail: " + path.string());
    }
}

// --- writer ------------------------------------------------------------------

void ViewerCache::Impl::writerLoop() {
    for (;;) {
        {
            std::unique_lock lock(mutex);
            wakeWriter.wait(lock, [&] { return stopping || !pending.empty(); });
            if (stopping)
                break;
            activeJob.emplace(std::move(pending.front()));
            pending.pop_front();
            writerActive = true;
            count.pendingFrames = pending.size();
            count.peakPendingFrames = std::max(count.peakPendingFrames, count.pendingFrames + 1);
        }
        encodeJob(*activeJob);
        {
            std::lock_guard lock(mutex);
            activeJob.reset();
        }
    }
    std::lock_guard lock(mutex);
    writerActive = false;
    notifyProgressLocked();
}

void ViewerCache::Impl::encodeJob(Job& job) {
    const std::string identity = job.publication.identity;
    std::optional<std::string> failure;
    try {
        encodeJobWork(job, failure);
    } catch (const std::exception& error) {
        failure = std::string("viewer cache preparation failed: ") + error.what();
    }
    // One epilogue: every path above releases the job's retained input and its
    // loading entry exactly once, so flush() and lookup() always agree with the
    // pipeline.
    std::lock_guard lock(mutex);
    job.publication.image.reset();
    releasePendingLocked(job.inputCharge + job.payloadCharge);
    job.inputCharge = job.payloadCharge = 0;
    retireGenerationLocked(job);
    if (failure) {
        rememberFailedLocked(identity, *failure);
        setErrorLocked(*failure);
    }
    loading.erase(identity);
    writerActive = false;
    count.activeFrames = 0;
    count.encodingFrames = 0;
    notifyProgressLocked();
}

void ViewerCache::Impl::encodeJobWork(Job& job, std::optional<std::string>& failure) {
    const std::string identity = job.publication.identity;
    {
        std::lock_guard lock(mutex);
        if (stopping || staleLocked(job)) {
            job.acceptingContributors = false;
            ++count.staleRejected;
            retireGenerationLocked(job);
            return;
        }
    }
    std::uint64_t payload = 0;
    if (!payloadEstimate(job.publication.layout, payload)) {
        failure = "viewer cache refused a frame with an unrepresentable raster extent";
        return;
    }
    std::string encoderError;
    if (!ensureEncoder(encoderError)) {
        failure = std::move(encoderError);
        return;
    }
    std::optional<gpu::Bc7Submission> submission;
    for (int attempt = 0; attempt <= kMaxEncodeAdmissionAttempts && !submission; ++attempt) {
        // The live frame that produced this publication may still be under
        // construction and presentation: defer our submission so it cannot win
        // the shared device queue ahead of that presentation. Re-checked on
        // every attempt, because a retry can outlive the foreground scope.
        waitForForeground();
        {
            std::lock_guard lock(mutex);
            if (stopping || staleLocked(job)) {
                job.acceptingContributors = false;
                ++count.staleRejected;
                retireGenerationLocked(job);
                return;
            }
            count.encodingFrames = 1;
        }
        try {
            // Throws for an invalid source image; returns nullopt when the
            // bounded submission pool could not admit the work.
            submission = encoder->encode(*job.publication.image);
        } catch (const std::exception& error) {
            failure = std::string("viewer cache BC7 encoding failed: ") + error.what();
            return;
        }
        if (!submission && attempt != kMaxEncodeAdmissionAttempts) {
            {
                std::lock_guard lock(mutex);
                if (stopping)
                    return;
            }
            std::this_thread::sleep_for(kCompletionPollInterval);
        }
    }
    if (!submission || submission->completion == 0) {
        failure = "viewer cache BC7 encoding could not be admitted by the bounded submission queue";
        return;
    }
    if (!observeCompletion(submission->completion))
        return;
    // The readback is compressed blocks only, sized by the logical raster.
    if (!submission->readback.mapped() || submission->readback.size() != payload) {
        failure = "viewer cache BC7 encoder returned no complete compressed readback";
        return;
    }
    std::vector<std::uint8_t> blocks(static_cast<std::size_t>(payload));
    std::memcpy(blocks.data(), submission->readback.mapped(), blocks.size());

    std::lock_guard lock(mutex);
    if (stopping)
        return;
    count.encodedFrames += 1;
    count.encodingFrames = 0;
    job.acceptingContributors = false;
    if (staleLocked(job)) {
        // Superseded while the encoder ran: the destination moved on, so
        // neither residency nor a durable entry is published.
        ++count.staleRejected;
        retireGenerationLocked(job);
        return;
    }
    retireGenerationLocked(job);
    const auto existing = entries.find(identity);
    if (existing == entries.end()) {
        if (entries.size() >= options.maxMetadataEntries) {
            ++count.admissionRejected;
            failure = "viewer cache metadata capacity reached";
            return;
        }
        Entry entry;
        entry.lastUse = ++tick;
        entries.emplace(identity, std::move(entry));
    }
    auto& entry = entries.at(identity);
    entry.layout = job.publication.layout;
    entry.description = job.publication.description;
    entry.request = job.publication.request;
    entry.viewingIdentity = job.publication.viewingIdentity;
    touchLocked(entry);
    clearTerminalLocked(identity);
    job.publication.image.reset();
    releasePendingLocked(job.inputCharge);
    job.inputCharge = 0;
    std::string diagnostic;
    if (!publishResidencyLocked(identity, submission->image, submission->image->image.charged_bytes(), diagnostic)) {
        // The frame is valid but cannot be held resident under the configured
        // budget: it stays available as compressed payload, and the limitation
        // is reported instead of being hidden.
        ++count.admissionRejected;
        setErrorLocked(diagnostic);
    }
    // The compressed copy is what durability and RAM replay consume; it is
    // charged to pending work until the durable attempt finishes.
    {
        entry.blocks = std::make_shared<const std::vector<std::uint8_t>>(std::move(blocks));
        entry.blocksBytes = entry.blocks->size();
        entry.blocksPending = true;
        // Transfer the output reservation to the pending durable payload.
        job.payloadCharge = 0;
        if (!entry.durableQueued && !entry.durable) {
            entry.durableQueued = true;
            durableBacklog.push_back(identity);
            wakeDurable.notify_one();
        }
    }
    ++count.published;
    notifyProgressLocked();
}

bool ViewerCache::Impl::publishResidencyLocked(const std::string& identity, std::shared_ptr<const gpu::Bc7Image> image,
                                               std::uint64_t chargedBytes, std::string& diagnostic) {
    enforceResidentBudgetLocked(identity, chargedBytes);
    if (count.residentBytes + chargedBytes > options.maxResidentBytes) {
        diagnostic = "viewer cache resident byte budget cannot hold another frame (" +
                     std::to_string(count.residentBytes) + " + " + std::to_string(chargedBytes) + " > " +
                     std::to_string(options.maxResidentBytes) + "); the frame stays available as compressed payload";
        return false;
    }
    takeResidentLocked(identity);
    const auto it = entries.find(identity);
    if (it == entries.end())
        return false;
    it->second.resident = std::move(image);
    it->second.residentBytes = chargedBytes;
    count.residentBytes += chargedBytes;
    ++count.residentFrames;
    return true;
}

// --- durability --------------------------------------------------------------

void ViewerCache::Impl::durableLoop() {
    NativeFile activeHandle = kNoFile;
    std::shared_ptr<Pack> activePack;
    for (;;) {
        std::string identity;
        bool haveWork = false;
        {
            std::unique_lock lock(mutex);
            wakeDurable.wait(lock, [&] { return stopping || !durableBacklog.empty(); });
            while (!durableBacklog.empty() && !haveWork) {
                identity = std::move(durableBacklog.front());
                durableBacklog.pop_front();
                const auto it = entries.find(identity);
                if (it == entries.end() || !it->second.blocks || it->second.durable) {
                    if (it != entries.end())
                        it->second.durableQueued = false;
                    continue;
                }
                haveWork = true;
            }
            if (!haveWork) {
                if (stopping)
                    break;
            } else {
                durableActive = true;
            }
        }
        if (!haveWork)
            continue;
        // Snapshot outside the lock: file I/O must never hold it. The payload
        // is shared, never copied: the record's own reference keeps the blocks
        // alive for exactly as long as this write needs them.
        std::shared_ptr<const std::vector<std::uint8_t>> blocks;
        ImageLayout layout;
        ImageDescription description;
        EvaluationRequest request;
        std::string viewingIdentity;
        {
            std::lock_guard lock(mutex);
            const auto it = entries.find(identity);
            if (it == entries.end() || !it->second.blocks) {
                durableActive = false;
                notifyProgressLocked();
                continue;
            }
            blocks = it->second.blocks;
            layout = it->second.layout;
            description = it->second.description;
            request = it->second.request;
            viewingIdentity = it->second.viewingIdentity;
        }
        try {
            persistJob(identity, blocks, viewingIdentity, layout, description, request, activeHandle, activePack);
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            setErrorLocked(std::string("viewer cache durable write failed: ") + error.what());
        }
        blocks.reset();
        std::lock_guard lock(mutex);
        const auto it = entries.find(identity);
        if (it != entries.end()) {
            auto& entry = it->second;
            entry.durableQueued = false;
            if (entry.blocks && entry.blocksPending) {
                releasePendingLocked(entry.blocksBytes);
                count.compressedHotBytes += entry.blocksBytes;
                entry.blocksPending = false;
            }
        }
        enforceRamBudgetLocked();
        durableActive = false;
        notifyProgressLocked();
    }
    closeFile(activeHandle);
    std::lock_guard lock(mutex);
    durableActive = false;
    notifyProgressLocked();
}

void ViewerCache::Impl::persistJob(const std::string& identity,
                                   const std::shared_ptr<const std::vector<std::uint8_t>>& blocks,
                                   const std::string& viewingIdentity, const ImageLayout& layout,
                                   const ImageDescription& description, const EvaluationRequest& request,
                                   NativeFile& activeHandle, std::shared_ptr<Pack>& activePack) {
    const auto fail = [&](std::string error) {
        std::lock_guard lock(mutex);
        rememberFailedLocked(identity, error);
        setErrorLocked(std::move(error));
    };
    if (!blocks || blocks->empty())
        return;
    {
        std::lock_guard lock(mutex);
        const auto it = entries.find(identity);
        if (stopping || it == entries.end() || it->second.blocks != blocks)
            return;
    }
    std::string metadata;
    const auto checksum = bytesChecksum(blocks->data(), blocks->size());
    try {
        if (payloadSize(layout) != blocks->size())
            throw std::runtime_error("compressed payload does not match the frame raster");
        metadata = json{{"format", std::string(kViewerCacheRepresentation)},
                        {"schema", kPackSchemaVersion},
                        {"identity", identity},
                        {"viewingIdentity", viewingIdentity},
                        {"payloadBytes", blocks->size()},
                        {"checksum", hexHash(checksum)},
                        {"layout", layoutToJson(layout)},
                        {"description", descriptionToJson(description)},
                        {"request", requestToJson(request)}}
                       .dump();
        if (metadata.size() > kMaxRecordMetadataBytes)
            throw std::runtime_error("frame metadata exceeds the cache record limit");
    } catch (const std::exception& error) {
        fail(std::string("viewer cache cannot persist frame: ") + error.what());
        return;
    }
    const std::uint64_t recordBytes = kRecordPrefixBytes + metadata.size() + blocks->size();
    const auto packLimit = std::min(kMaxPackBytes, options.maxDiskBytes);
    if (recordBytes > packLimit || kPackHeaderBytes > packLimit - recordBytes) {
        fail("viewer cache disk budget cannot hold this independent frame record");
        return;
    }
    if (activePack && recordBytes > packLimit - activePack->fileBytes) {
        closeFile(activeHandle);
        activeHandle = kNoFile;
        activePack.reset();
    }
    // Charge physical bytes, including pack headers and dead records. Whole
    // reader-free packs are eviction units; frames have no decode dependency.
    if (!makeDiskRoom(recordBytes + kPackHeaderBytes, activeHandle, activePack)) {
        fail("viewer cache disk budget is pinned by active readers or cannot reclaim owned packs");
        return;
    }
    const bool newPack = !activePack;
    std::string error;
    if (newPack) {
        auto pack = std::make_shared<Pack>();
        pack->path = namespacePath() / ("pack-" + std::to_string(nextFileId++) + "-" + hexHash(checksum) + ".pack");
        activeHandle = openAppendFile(pack->path, error);
        if (activeHandle == kNoFile) {
            fail(error);
            return;
        }
        activePack = std::move(pack);
        std::lock_guard lock(mutex);
        packs.emplace(activePack->path, activePack);
    }
    const auto target = activePack;
    const auto priorBytes = target->fileBytes;
    const auto recordOffset = newPack ? kPackHeaderBytes : priorBytes;
    const auto payloadOffset = recordOffset + kRecordPrefixBytes + metadata.size();
    unsigned char header[kPackHeaderBytes]{};
    std::memcpy(header, kPackMagic, sizeof(kPackMagic));
    writeWord(kPackSchemaVersion, header + 8);
    writeWord(kPackHeaderBytes, header + 16);
    unsigned char prefix[kRecordPrefixBytes]{};
    writeWord(recordBytes, prefix);
    writeWord(metadata.size(), prefix + 8);
    writeWord(blocks->size(), prefix + 16);
    writeWord(bytesChecksum(reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size()), prefix + 24);
    const bool written = (!newPack || writeAllFile(activeHandle, header, sizeof(header), error)) &&
                         writeAllFile(activeHandle, prefix, sizeof(prefix), error) &&
                         writeAllFile(activeHandle, metadata.data(), metadata.size(), error) &&
                         writeAllFile(activeHandle, blocks->data(), blocks->size(), error) &&
                         syncFile(activeHandle, error);
    std::error_code sizeError;
    const auto physicalBytes = std::filesystem::file_size(target->path, sizeError);
    {
        std::lock_guard lock(mutex);
        // A failed stat retains the full reserved charge rather than hiding
        // partially written storage. Reopening recovers its exact size.
        const auto committedBytes = sizeError ? payloadOffset + blocks->size() : physicalBytes;
        count.diskBytes += committedBytes - priorBytes;
        target->fileBytes = committedBytes;
        const auto it = entries.find(identity);
        if (written && it != entries.end() && it->second.blocks == blocks) {
            auto& entry = it->second;
            retireLocationLocked(entry);
            entry.pack = target;
            entry.payloadOffset = payloadOffset;
            entry.payloadBytes = blocks->size();
            entry.checksum = checksum;
            entry.durable = true;
            ++target->liveRecords;
        }
    }
    if (!written) {
        closeFile(activeHandle);
        activeHandle = kNoFile;
        activePack.reset();
        fail(error.empty() ? "viewer cache pack write failed" : error);
    }
}

// Reclaim complete cache-owned packs before writing. Never rewrite live record
// offsets in place: readers pin their pack, and pending preparation excludes it.
bool ViewerCache::Impl::makeDiskRoom(std::uint64_t bytes, NativeFile& activeHandle, std::shared_ptr<Pack>& activePack) {
    for (;;) {
        std::shared_ptr<Pack> victim;
        {
            std::lock_guard lock(mutex);
            if (bytes <= options.maxDiskBytes - std::min(count.diskBytes, options.maxDiskBytes) &&
                count.diskBytes <= options.maxDiskBytes)
                return true;
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (const auto& [path, pack] : packs) {
                (void)path;
                if (pack->readers != 0 || pack->reclaiming)
                    continue;
                bool pinned = false;
                std::uint64_t use = 0;
                for (const auto& [identity, entry] : entries) {
                    if (entry.pack != pack)
                        continue;
                    pinned = pinned || loading.contains(identity);
                    use = std::max(use, entry.lastUse);
                }
                if (!pinned && (!victim || use < oldest)) {
                    victim = pack;
                    oldest = use;
                }
            }
            if (!victim)
                return false;
            victim->reclaiming = true;
        }
        if (activePack == victim) {
            closeFile(activeHandle);
            activeHandle = kNoFile;
            activePack.reset();
        }
        std::error_code error;
        std::filesystem::remove(victim->path, error);
        std::lock_guard lock(mutex);
        if (error) {
            victim->reclaiming = false;
            setErrorLocked("cannot reclaim owned cache pack '" + victim->path.string() + "': " + error.message());
            return false;
        }
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.pack != victim) {
                ++it;
                continue;
            }
            retireLocationLocked(it->second);
            if (!it->second.blocks && !it->second.resident) {
                rememberEvictedLocked(it->first);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
        count.diskBytes -= victim->fileBytes;
        packs.erase(victim->path);
    }
}

// --- replay ------------------------------------------------------------------

void ViewerCache::Impl::replayLoop() {
    for (;;) {
        std::string identity;
        {
            std::unique_lock lock(mutex);
            wakeReplay.wait(lock, [&] { return stopping || !replayQueue.empty(); });
            if (replayQueue.empty() && stopping)
                break;
            identity = std::move(replayQueue.front());
            replayQueue.pop_front();
            replayActive = true;
        }
        try {
            replayFrame(identity);
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            const auto diagnostic = std::string("viewer cache replay preparation failed: ") + error.what();
            rememberFailedLocked(identity, diagnostic);
            setErrorLocked(diagnostic);
        }
        std::lock_guard lock(mutex);
        replayActive = false;
        // The preparation for this identity is over, ready or not: a later
        // lookup re-admits it from whatever representation remains.
        loading.erase(identity);
        enforceRamBudgetLocked();
        notifyProgressLocked();
    }
    std::lock_guard lock(mutex);
    replayActive = false;
    notifyProgressLocked();
}

void ViewerCache::Impl::replayFrame(const std::string& identity) {
    std::shared_ptr<const std::vector<std::uint8_t>> ram;
    std::shared_ptr<Pack> pack;
    std::uint64_t payloadOffset = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t checksum = 0;
    ImageLayout layout;
    struct ReadLease {
        Impl& owner;
        std::shared_ptr<Pack>& pack;
        std::uint64_t bytes{0};
        ~ReadLease() {
            std::lock_guard lock(owner.mutex);
            owner.releasePendingLocked(bytes);
            if (bytes != 0 && pack && pack->readers != 0)
                --pack->readers;
        }
    } readLease{*this, pack};
    {
        std::lock_guard lock(mutex);
        const auto it = entries.find(identity);
        if (it == entries.end() || it->second.resident)
            return;
        ram = it->second.blocks;
        layout = it->second.layout;
        if (!ram) {
            pack = it->second.pack;
            if (!pack || !pack->liveRecords) {
                entries.erase(identity);
                rememberEvictedLocked(identity);
                return;
            }
            if (pack->reclaiming) {
                // Reclamation has selected this pack; defer until it commits
                // its authoritative entry removal.
                return;
            }
            payloadOffset = it->second.payloadOffset;
            payloadBytes = it->second.payloadBytes;
            checksum = it->second.checksum;
            if (payloadBytes > options.maxPendingBytes - std::min(count.pendingBytes, options.maxPendingBytes)) {
                ++count.admissionDropped;
                pack.reset();
                return;
            }
            ++pack->readers;
            chargePendingLocked(payloadBytes);
            readLease.bytes = payloadBytes;
        } else {
            ++count.compressedHotHits;
        }
    }
    const auto fail = [&](const std::string& diagnostic, bool invalid) {
        std::lock_guard lock(mutex);
        if (invalid) {
            ++count.invalidEntries;
            const auto it = entries.find(identity);
            if (it != entries.end()) {
                eraseEntryLocked(identity);
            }
        }
        rememberFailedLocked(identity, diagnostic);
        setErrorLocked(diagnostic);
    };

    std::vector<std::uint8_t> buffer;
    const std::vector<std::uint8_t>* source = ram.get();
    if (!source) {
        std::error_code error;
        const std::uint64_t fileBytes = std::filesystem::file_size(pack->path, error);
        if (error || payloadBytes == 0 || payloadOffset > fileBytes || payloadBytes > fileBytes - payloadOffset) {
            fail("cached frame payload is missing from its pack '" + pack->path.string() + "'", true);
            return;
        }
        try {
            buffer.resize(static_cast<std::size_t>(payloadBytes));
        } catch (const std::bad_alloc&) {
            fail("viewer cache could not allocate a replay read buffer", false);
            return;
        }
        std::ifstream input(pack->path, std::ios::binary);
        if (!input || !readExact(input, payloadOffset, buffer.data(), buffer.size())) {
            fail("cached frame payload is truncated in '" + pack->path.string() + "'", true);
            return;
        }
        if (bytesChecksum(buffer.data(), buffer.size()) != checksum) {
            fail("cached frame payload failed its integrity check in '" + pack->path.string() + "'", true);
            return;
        }
        source = &buffer;
    }

    std::string encoderError;
    if (!ensureEncoder(encoderError)) {
        fail(encoderError, false);
        return;
    }
    std::optional<gpu::Bc7Submission> submission;
    for (int attempt = 0; attempt <= kMaxEncodeAdmissionAttempts && !submission; ++attempt) {
        // Same foreground gate as the writer: a replay upload must not be
        // queued ahead of the live frame this worker is about to present.
        waitForForeground();
        try {
            submission =
                encoder->upload(static_cast<std::uint32_t>(layout.width), static_cast<std::uint32_t>(layout.height),
                                std::span<const std::uint8_t>(*source));
        } catch (const std::exception& error) {
            fail(std::string("viewer cache BC7 upload failed: ") + error.what(), false);
            return;
        }
        if (!submission && attempt != kMaxEncodeAdmissionAttempts) {
            {
                std::lock_guard lock(mutex);
                if (stopping) {
                    return;
                }
            }
            std::this_thread::sleep_for(kCompletionPollInterval);
        }
    }
    if (!submission || submission->completion == 0) {
        fail("viewer cache BC7 upload could not be admitted by the bounded submission queue", false);
        return;
    }
    if (!observeCompletion(submission->completion))
        return;
    std::string diagnostic;
    {
        std::lock_guard lock(mutex);
        if (stopping)
            return;
        const auto it = entries.find(identity);
        if (it == entries.end())
            return;
        if (!publishResidencyLocked(identity, submission->image, submission->image->image.charged_bytes(),
                                    diagnostic)) {
            ++count.admissionRejected;
            setErrorLocked(diagnostic);
            return;
        }
        ++count.uploadedFrames;
        notifyProgressLocked();
    }
}

// --- lifecycle ---------------------------------------------------------------

void ViewerCache::Impl::configure(const ViewerCacheOptions& configuredOptions) {
    if (configuredOptions.directory.empty())
        throw std::invalid_argument("viewer cache directory must not be empty");
    if (configuredOptions.maxPendingFrames == 0 || configuredOptions.maxMetadataEntries == 0 ||
        configuredOptions.maxReplayFrames == 0)
        throw std::invalid_argument("viewer cache frame, metadata and replay bounds must be positive");
    if (configuredOptions.maxDiskBytes == 0 || configuredOptions.maxRamBytes == 0 ||
        configuredOptions.maxResidentBytes == 0 || configuredOptions.maxPendingBytes == 0)
        throw std::invalid_argument("viewer cache byte bounds must be positive");
    std::unique_lock lock(mutex);
    if (configured)
        throw std::logic_error("viewer cache is already configured");
    options = configuredOptions;
    configured = true;
    try {
        acquireWriterLockLocked();
        loadIndexLocked();
        NativeFile noFile = kNoFile;
        std::shared_ptr<Pack> noPack;
        lock.unlock();
        const bool fits = makeDiskRoom(0, noFile, noPack);
        lock.lock();
        if (!fits)
            throw std::runtime_error("existing cache packs cannot be reclaimed within the configured disk budget");
        // Working-set admission: the allocator can ask this cache to give up
        // eligible residency before it fails an allocation. Registration is
        // process-wide and bounded; without it the cache could not take part in
        // shared pressure at all, which is a configuration failure rather than
        // a reason to allocate privately.
        evictor = allocator.register_evictor(
            [this](std::uint64_t wantedBytes) { return evictEligibleResidency(wantedBytes); });
    } catch (const std::exception& error) {
        // A cache-local failure: the directory is unusable, already owned by
        // another process, or the allocator refuses another evictor. The live
        // rendering path stays authoritative and every lookup reports the
        // identifying reason instead of a false miss or a false Ready.
        unavailable = std::string("viewer cache unavailable: ") + error.what();
        evictor.reset();
        releaseWriterLock();
        entries.clear();
        packs.clear();
        count = ViewerCacheCounts{};
        setErrorLocked(unavailable);
        return;
    }
    writerThread = std::thread([state = this] { state->writerLoop(); });
    durableThread = std::thread([state = this] { state->durableLoop(); });
    replayThread = std::thread([state = this] { state->replayLoop(); });
}

void ViewerCache::Impl::stop() noexcept {
    // Deregistration first: it waits for an in-flight eviction callback of this
    // registration, so the callback can never touch state we are about to
    // destroy. No cache lock is held here, so a callback blocked on that lock
    // still completes.
    evictor.reset();
    // Release any writer deferred by a foreground scope before joining, so
    // shutdown is bounded even while a foreground construction scope is alive.
    {
        std::lock_guard gateLock(gate->mutex);
        gate->stopped = true;
    }
    gate->released.notify_all();
    {
        std::lock_guard lock(mutex);
        stopping = true;
        for (auto& job : pending) {
            job.publication.image.reset();
            releasePendingLocked(job.inputCharge + job.payloadCharge);
            loading.erase(job.publication.identity);
            retireGenerationLocked(job);
        }
        pending.clear();
        for (const auto& identity : replayQueue)
            loading.erase(identity);
        replayQueue.clear();
    }
    wakeWriter.notify_all();
    wakeDurable.notify_all();
    wakeReplay.notify_all();
    idle.notify_all();
    if (writerThread.joinable())
        writerThread.join();
    if (durableThread.joinable())
        durableThread.join();
    if (replayThread.joinable())
        replayThread.join();
    releaseWriterLock();
}

ViewerCache::ViewerCache(gpu::Device& device, gpu::Allocator& allocator, const std::filesystem::path& shaderDirectory)
    : impl_(std::make_unique<Impl>(device, allocator, shaderDirectory)) {}

ViewerCache::~ViewerCache() {
    if (impl_)
        impl_->stop();
}

void ViewerCache::configure(const ViewerCacheOptions& options) {
    impl_->configure(options);
}

ViewerCache::ForegroundScope ViewerCache::foregroundScope() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->configured || !impl_->unavailable.empty() || impl_->stopping)
        return ForegroundScope{};
    return ForegroundScope{impl_->gate};
}

ViewerCacheLookup ViewerCache::lookup(const std::string& identity) {
    ViewerCacheLookup result;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->configured) {
        result.diagnostic = "viewer cache is not configured";
        return result;
    }
    if (!impl_->unavailable.empty()) {
        // Nothing is cached and nothing can be: the caller renders live, with
        // the real reason attached instead of an unexplained miss.
        result.diagnostic = impl_->unavailable;
        return result;
    }
    if (impl_->stopping) {
        result.diagnostic = "viewer cache is shutting down";
        return result;
    }
    const auto entry = impl_->entries.find(identity);
    if (entry != impl_->entries.end()) {
        if (entry->second.resident) {
            impl_->touchLocked(entry->second);
            ++impl_->count.hits;
            result.state = ViewerCacheState::Ready;
            result.frame = ViewerCacheResult{entry->second.resident, entry->second.layout};
            return result;
        }
        const auto loading = impl_->loading.find(identity);
        if (loading != impl_->loading.end()) {
            // An admitted preparation is never an absent entry: the caller
            // keeps waiting instead of planning and evaluating the graph again.
            result.state = ViewerCacheState::Loading;
            result.diagnostic = loading->second;
            return result;
        }
        std::shared_ptr<Impl::Pack> pack = entry->second.pack;
        if (!entry->second.blocks && pack && pack->reclaiming) {
            result.state = ViewerCacheState::Loading;
            result.diagnostic = "cached frame is being moved inside the cache";
            return result;
        }
        if (!entry->second.blocks && (!pack || !pack->liveRecords)) {
            impl_->eraseEntryLocked(identity);
            impl_->rememberEvictedLocked(identity);
            result.state = ViewerCacheState::Evicted;
            result.diagnostic = "cached frame was reclaimed; render it again";
            return result;
        }
        const std::size_t admitted = impl_->replayQueue.size() + (impl_->replayActive ? 1U : 0U);
        if (admitted >= impl_->options.maxReplayFrames) {
            result.state = ViewerCacheState::Loading;
            result.diagnostic = "viewer cache replay window is full";
            return result;
        }
        // Admission order matters: the queue entry exists before the loading
        // record, so a failed allocation can never leave an identity marked
        // loading with nothing that will ever finish it.
        try {
            impl_->replayQueue.push_back(identity);
            impl_->loading[identity] = "cached frame is being read and uploaded";
        } catch (const std::bad_alloc&) {
            if (!impl_->replayQueue.empty() && impl_->replayQueue.back() == identity)
                impl_->replayQueue.pop_back();
            impl_->loading.erase(identity);
            result.state = ViewerCacheState::Failed;
            result.diagnostic = "viewer cache could not admit a replay preparation";
            return result;
        }
        impl_->wakeReplay.notify_one();
        result.state = ViewerCacheState::Loading;
        return result;
    }
    if (const auto loading = impl_->loading.find(identity); loading != impl_->loading.end()) {
        result.state = ViewerCacheState::Loading;
        result.diagnostic = loading->second;
        return result;
    }
    const auto failed = impl_->failed.find(identity);
    if (failed != impl_->failed.end()) {
        result.state = ViewerCacheState::Failed;
        result.diagnostic = failed->second;
        return result;
    }
    if (impl_->evicted.contains(identity)) {
        result.state = ViewerCacheState::Evicted;
        result.diagnostic = "cached frame is no longer retained";
        return result;
    }
    ++impl_->count.misses;
    result.state = ViewerCacheState::Missing;
    return result;
}

bool ViewerCache::enqueue(ViewerCachePublication publication) {
    std::lock_guard lock(impl_->mutex);
    return enqueueLocked(std::move(publication));
}

bool ViewerCache::enqueueLocked(ViewerCachePublication publication) {
    if (publication.identity.empty() || !publication.image || publication.layout.width <= 0 ||
        publication.layout.height <= 0 || publication.layout.color != ColorInterpretation::DisplayReferred)
        return false;
    if (!impl_->configured || impl_->stopping || !impl_->unavailable.empty())
        return false;
    if (!impl_->admitDestinationLocked(publication.destination))
        return false;
    // Revision freshness is an equality token within this destination.
    // Generation remains the monotonic ordering used to reject older
    // publications. Other destinations retain their own valid work.
    if (publication.publicationGuard && !publication.publicationGuard()) {
        ++impl_->count.staleRejected;
        return false;
    }
    const auto currentGeneration = impl_->latestGenerationByDestination.find(publication.destination);
    if (currentGeneration != impl_->latestGenerationByDestination.end() &&
        publication.generation < currentGeneration->second) {
        ++impl_->count.staleRejected;
        return false;
    }
    const auto currentIdentityGeneration =
        impl_->latestGenerationByIdentity.find(std::pair{publication.destination, publication.identity});
    if (currentIdentityGeneration != impl_->latestGenerationByIdentity.end() &&
        publication.generation < currentIdentityGeneration->second) {
        ++impl_->count.staleRejected;
        return false;
    }
    if (currentGeneration == impl_->latestGenerationByDestination.end() ||
        publication.generation >= currentGeneration->second) {
        impl_->latestGenerationByDestination[publication.destination] = publication.generation;
        impl_->latestRevisionByDestination[publication.destination] = publication.revision;
    }

    const std::string identity = publication.identity;
    std::uint64_t payload = 0;
    if (!impl_->payloadEstimate(publication.layout, payload)) {
        ++impl_->count.admissionRejected;
        impl_->count.lastError = "viewer cache refused a publication with an unrepresentable raster extent";
        return false;
    }
    const std::uint64_t inputCharge = publication.image->charged_bytes();
    if (inputCharge > impl_->options.maxPendingBytes || payload > impl_->options.maxPendingBytes - inputCharge) {
        ++impl_->count.admissionRejected;
        impl_->count.lastError = "viewer cache pending byte budget cannot retain this frame and its blocks";
        return false;
    }
    const auto totalCharge = inputCharge + payload;
    if (impl_->activeJob && impl_->writerActive && impl_->activeJob->publication.identity == identity) {
        if (!impl_->activeJob->acceptingContributors) {
            if (impl_->entries.contains(identity))
                return true;
            ++impl_->count.admissionDropped;
            return false;
        }
        impl_->mergeContributorLocked(*impl_->activeJob, std::move(publication));
        return true;
    }
    const auto pending = std::find_if(impl_->pending.begin(), impl_->pending.end(),
                                      [&](const Impl::Job& job) { return job.publication.identity == identity; });
    if (pending != impl_->pending.end()) {
        impl_->mergeContributorLocked(*pending, std::move(publication));
        impl_->wakeWriter.notify_one();
        return true;
    }
    // Content this cache already holds needs no second encode: a resident frame
    // is already answerable, and a retained compressed frame is coalesced into
    // the replay that turns it back into residency.
    const auto existing = impl_->entries.find(identity);
    if (existing != impl_->entries.end()) {
        impl_->clearTerminalLocked(identity);
        const bool loading = impl_->loading.contains(identity);
        const std::size_t admitted = impl_->replayQueue.size() + (impl_->replayActive ? 1U : 0U);
        if (!existing->second.resident && !loading && admitted < impl_->options.maxReplayFrames &&
            (existing->second.blocks || existing->second.pack)) {
            // Queue entry first: see lookup()'s admission order.
            try {
                impl_->replayQueue.push_back(identity);
                impl_->loading[identity] = "cached frame is being read and uploaded";
            } catch (const std::bad_alloc&) {
                if (!impl_->replayQueue.empty() && impl_->replayQueue.back() == identity)
                    impl_->replayQueue.pop_back();
                impl_->loading.erase(identity);
                return false;
            }
            impl_->wakeReplay.notify_one();
        } else if (!existing->second.resident && !loading && !existing->second.blocks && !existing->second.pack) {
            // No representation is left to serve: drop the record so the next
            // lookup honestly reports a miss for this fresh publication.
            impl_->entries.erase(existing);
        }
        return true;
    }
    if (impl_->entries.size() >= impl_->options.maxMetadataEntries) {
        ++impl_->count.admissionRejected;
        impl_->count.lastError = "viewer cache metadata capacity reached";
        return false;
    }
    while (impl_->pending.size() + (impl_->writerActive ? 1U : 0U) >= impl_->options.maxPendingFrames ||
           impl_->count.pendingBytes > impl_->options.maxPendingBytes - totalCharge) {
        const auto obsolete = std::find_if(impl_->pending.begin(), impl_->pending.end(),
                                           [&](const Impl::Job& job) { return impl_->staleLocked(job); });
        if (obsolete == impl_->pending.end()) {
            ++impl_->count.admissionDropped;
            impl_->count.lastError = "viewer cache pending work reached its byte/frame admission bound";
            return false;
        }
        Impl::Job dropped = std::move(*obsolete);
        impl_->pending.erase(obsolete);
        dropped.publication.image.reset();
        impl_->releasePendingLocked(dropped.inputCharge + dropped.payloadCharge);
        impl_->loading.erase(dropped.publication.identity);
        impl_->retireGenerationLocked(dropped);
        ++impl_->count.admissionDropped;
    }
    try {
        impl_->latestGenerationByIdentity[{publication.destination, identity}] = publication.generation;
        impl_->clearTerminalLocked(identity);
        impl_->loading[identity] = "frame is being encoded";
    } catch (...) {
        return false;
    }
    Impl::Job job;
    job.publication = std::move(publication);
    job.inputCharge = inputCharge;
    job.payloadCharge = payload;
    // The enqueue that actually holds an image is the one that charges it, so a
    // failed admission can never leave an orphan charge behind.
    impl_->chargePendingLocked(totalCharge);
    try {
        impl_->pending.emplace_back(std::move(job));
    } catch (...) {
        impl_->loading.erase(identity);
        impl_->releasePendingLocked(totalCharge);
        return false;
    }
    impl_->count.pendingFrames = impl_->pending.size();
    impl_->count.peakPendingFrames =
        std::max(impl_->count.peakPendingFrames, impl_->count.pendingFrames + impl_->count.activeFrames);
    impl_->wakeWriter.notify_one();
    return true;
}

void ViewerCache::supersede(std::uint64_t revision, std::uint64_t generation, ViewerDestination destination) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->admitDestinationLocked(destination))
        throw std::runtime_error(impl_->count.lastError);
    const auto current = impl_->latestGenerationByDestination.find(destination);
    if (current != impl_->latestGenerationByDestination.end() && generation < current->second)
        return;
    impl_->latestGenerationByDestination[destination] = generation;
    impl_->latestRevisionByDestination[destination] = revision;
}

void ViewerCache::flush() {
    std::unique_lock lock(impl_->mutex);
    impl_->idle.wait(lock, [&] { return impl_->drainedLocked() || impl_->stopping; });
    if (impl_->count.errors != 0)
        throw std::runtime_error(impl_->count.lastError.empty() ? "viewer cache storage failed"
                                                                : impl_->count.lastError);
}

ViewerCacheCounts ViewerCache::counts() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshotCountsLocked();
}

std::optional<ViewerCacheCounts> ViewerCache::tryCounts() const {
    std::unique_lock lock(impl_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return std::nullopt;
    return impl_->snapshotCountsLocked();
}

}  // namespace nemo::eval
