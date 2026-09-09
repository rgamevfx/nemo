#include "nemo/eval/ViewerCache.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif
#include <utility>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.h>

#include "nemo/core/Hashing.hpp"
#include "nemo/gpu/ComputePass.hpp"
#include "nemo/gpu/Error.hpp"
#include "nemo/media/VideoDecode.hpp"

namespace nemo::eval {
namespace {

using json = nlohmann::json;

[[nodiscard]] std::uint64_t identityHash(const std::string& identity) {
    std::uint64_t hash = kFnv1a64Basis;
    hashMixText(hash, identity);
    return hash;
}

[[nodiscard]] std::string hexHash(std::uint64_t value) {
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

void addEncodeStats(media::EncodeStats& aggregate, const media::EncodeStats& value) {
    if (!value.codec.empty())
        aggregate.codec = value.codec;
    if (!value.profile.empty())
        aggregate.profile = value.profile;
    aggregate.initializationMs += value.initializationMs;
    aggregate.allocationPackingMs += value.allocationPackingMs;
    aggregate.conversionMs += value.conversionMs;
    aggregate.gpuConversionMs += value.gpuConversionMs;
    aggregate.hostToDeviceMs += value.hostToDeviceMs;
    aggregate.hostToDeviceBytes += value.hostToDeviceBytes;
    aggregate.deviceToDeviceMs += value.deviceToDeviceMs;
    aggregate.deviceToDeviceBytes += value.deviceToDeviceBytes;
    aggregate.deviceToHostMs += value.deviceToHostMs;
    aggregate.deviceToHostBytes += value.deviceToHostBytes;
    aggregate.stagingBytes = std::max(aggregate.stagingBytes, value.stagingBytes);
    aggregate.submissionDrainMs += value.submissionDrainMs;
    aggregate.muxFinalizationMs += value.muxFinalizationMs;
    aggregate.completeChunkMs += value.completeChunkMs;
    aggregate.coldSetupMs += value.coldSetupMs;
    aggregate.warmSetupMs += value.warmSetupMs;
    aggregate.sessionChunkCount += value.sessionChunkCount;
    aggregate.sessionReuseCount += value.sessionReuseCount;
    aggregate.encodedBytes += value.encodedBytes;
    aggregate.encodedFrames += value.encodedFrames;
    aggregate.sessionReused = aggregate.sessionReused || value.sessionReused;
    if (!value.fallbackReason.empty())
        aggregate.fallbackReason = value.fallbackReason;
}

[[nodiscard]] bool transientReplayFailure(const std::exception& error) {
    std::string message = error.what();
    std::transform(message.begin(), message.end(), message.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    constexpr std::string_view transient[] = {
        "timeout",
        "timed out",
        "temporarily unavailable",
        "resource busy",
        "device or resource busy",
        "text file busy",
        "file in use",
        "sharing violation",
        "permission denied",
        "input/output error",
        "i/o error",
        "would block",
        "try again",
        "allocation",
    };
    for (const std::string_view token : transient) {
        if (message.find(token) != std::string::npos)
            return true;
    }
    return false;
}

[[nodiscard]] bool sameLayout(const ImageLayout& a, const ImageLayout& b) {
    return a.width == b.width && a.height == b.height && a.pixelAspect == b.pixelAspect && a.channels == b.channels &&
           a.precision == b.precision && a.color == b.color;
}
// Replay hot tiers are deliberately fixed safety bounds, not user policy:
// large chunks remain disk-backed and decoded images use a tiny FIFO queue.
constexpr std::uint64_t kCompressedHotChunkBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kCompressedHotBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kDecodedHotFrames = 4;

[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> readCompressedHot(const std::filesystem::path& path,
                                                                                 std::uint64_t bytes) {
    if (bytes == 0 || bytes > kCompressedHotChunkBytes)
        return {};
    try {
        std::vector<std::uint8_t> contents(static_cast<std::size_t>(bytes));
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return {};
        input.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(contents.size()))
            return {};
        return std::make_shared<const std::vector<std::uint8_t>>(std::move(contents));
    } catch (const std::bad_alloc&) {
        return {};
    }
}

}  // namespace

struct ViewerCache::Impl {
    struct DiskChunk {
        std::filesystem::path mediaPath;
        std::filesystem::path metadataPath;
        std::uint64_t bytes{0};
        std::size_t entryRefs{0};
        std::size_t lookupRefs{0};
        std::shared_ptr<const std::vector<std::uint8_t>> compressedHot;
        bool retired{false};
        bool cleanupQueued{false};
    };
    struct DiskEntry {
        std::shared_ptr<DiskChunk> chunk;
        std::size_t offset{0};
        ImageLayout layout;
    };
    using Job = ViewerCachePublication;
    struct DecodedHot {
        std::shared_ptr<const gpu::Image> image;
        ImageLayout layout;
    };
    struct Cleanup {
        std::filesystem::path mediaPath;
        std::filesystem::path metadataPath;
    };

    gpu::Instance& instance;
    gpu::Device& device;
    gpu::Allocator& allocator;
    std::filesystem::path convertSpirv;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Job> pending;
    std::map<std::string, DiskEntry> entries;
    std::map<std::filesystem::path, std::shared_ptr<DiskChunk>> chunks;
    std::map<std::pair<ViewerDestination, std::string>, std::uint64_t> latestGenerationByIdentity;
    std::map<ViewerDestination, std::uint64_t> latestRevisionByDestination;
    std::map<ViewerDestination, std::uint64_t> latestGenerationByDestination;
    std::map<std::string, DecodedHot> decodedHot;
    std::deque<std::string> decodedHotOrder;
    std::deque<std::filesystem::path> compressedHotOrder;
    std::uint64_t compressedHotBytes{0};
    ViewerCacheOptions options;
    std::optional<media::EncodeFailure> encodingFailure;
    ViewerCacheCounts count;
    std::thread worker;
    bool configured{false};
    bool stopping{false};
    bool active{false};
    std::uint64_t decodedInFlight{0};
    std::uint64_t reservedDiskBytes{0};
    std::uint64_t nextFileId{1};
    std::unique_ptr<media::ViewerChunkEncoder> encoder;
    int writerLockFd{-1};
    Impl(gpu::Instance& instanceRef, gpu::Device& deviceRef, gpu::Allocator& allocatorRef,
         const std::filesystem::path& convertSpirvPath)
        : instance(instanceRef), device(deviceRef), allocator(allocatorRef), convertSpirv(convertSpirvPath) {}

    [[nodiscard]] std::optional<ViewerCacheResult> lookup(const std::string& identity, const ImageLayout& expected,
                                                          std::uint64_t timeout_ns);

    [[nodiscard]] ViewerCacheCounts snapshotCountsLocked() const {
        ViewerCacheCounts result = count;
        result.pendingFrames = pending.size();
        result.compressedHotBytes = compressedHotBytes;
        result.compressedHotChunks = compressedHotOrder.size();
        result.decodedHotFrames = decodedHot.size();
        return result;
    }

    void setErrorLocked(std::string message) {
        ++count.errors;
        count.lastError = std::move(message);
    }
    void dropDecodedHotLocked(const std::string& identity) {
        decodedHot.erase(identity);
        std::erase(decodedHotOrder, identity);
    }

    void dropCompressedHotLocked(const std::shared_ptr<DiskChunk>& chunk) {
        if (!chunk->compressedHot)
            return;
        compressedHotBytes -= std::min(compressedHotBytes, static_cast<std::uint64_t>(chunk->compressedHot->size()));
        chunk->compressedHot.reset();
        std::erase(compressedHotOrder, chunk->mediaPath);
    }

    void retainCompressedHotLocked(const std::shared_ptr<DiskChunk>& chunk,
                                   std::shared_ptr<const std::vector<std::uint8_t>> contents) {
        if (!contents || contents->size() > kCompressedHotChunkBytes)
            return;
        dropCompressedHotLocked(chunk);
        while (compressedHotOrder.size() >= 4 || compressedHotBytes > kCompressedHotBytes - contents->size()) {
            if (compressedHotOrder.empty())
                break;
            const auto path = compressedHotOrder.front();
            compressedHotOrder.pop_front();
            const auto it = chunks.find(path);
            if (it != chunks.end() && it->second->compressedHot) {
                compressedHotBytes -=
                    std::min(compressedHotBytes, static_cast<std::uint64_t>(it->second->compressedHot->size()));
                it->second->compressedHot.reset();
            }
        }
        if (compressedHotOrder.size() >= 4 || compressedHotBytes > kCompressedHotBytes - contents->size())
            return;
        chunk->compressedHot = std::move(contents);
        compressedHotBytes += chunk->compressedHot->size();
        compressedHotOrder.push_back(chunk->mediaPath);
    }

    void retainDecodedHotLocked(const std::string& identity, const ImageLayout& layout,
                                std::shared_ptr<const gpu::Image> image) {
        dropDecodedHotLocked(identity);
        while (decodedHot.size() >= kDecodedHotFrames && !decodedHotOrder.empty()) {
            const std::string oldest = std::move(decodedHotOrder.front());
            decodedHotOrder.pop_front();
            decodedHot.erase(oldest);
        }
        if (decodedHot.size() >= kDecodedHotFrames)
            return;
        decodedHot.emplace(identity, DecodedHot{std::move(image), layout});
        decodedHotOrder.push_back(identity);
    }

    void acquireWriterLockLocked() {
        std::error_code error;
        std::filesystem::create_directories(options.directory, error);
        if (error)
            throw std::runtime_error("viewer cache: cannot create directory '" + options.directory.string() +
                                     "': " + error.message());
        const auto lockPath = options.directory / ".nemo-viewer-cache.lock";
#if defined(_WIN32)
        int fd = -1;
        if (_sopen_s(&fd, lockPath.string().c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYRW,
                     _S_IREAD | _S_IWRITE) != 0) {
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

    void releaseWriterLock() noexcept {
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

    [[nodiscard]] bool staleLocked(const Job& job) const {
        if (job.publicationGuard && !job.publicationGuard())
            return true;
        const auto revision = latestRevisionByDestination.find(job.destination);
        if (revision != latestRevisionByDestination.end() && job.revision != revision->second)
            return true;
        const auto identity = latestGenerationByIdentity.find(std::pair{job.destination, job.identity});
        return identity != latestGenerationByIdentity.end() && job.generation < identity->second;
    }

    void retireGenerationLocked(const Job& job) {
        const auto it = latestGenerationByIdentity.find(std::pair{job.destination, job.identity});
        if (it != latestGenerationByIdentity.end() && it->second == job.generation)
            latestGenerationByIdentity.erase(it);
    }

    void retireGenerationsLocked(const std::vector<Job>& jobs) {
        for (const Job& job : jobs)
            retireGenerationLocked(job);
    }

    void queueChunkCleanupLocked(const std::shared_ptr<DiskChunk>& chunk, std::vector<Cleanup>& cleanup) {
        if (chunk->entryRefs != 0 || chunk->lookupRefs != 0 || !chunk->retired || chunk->cleanupQueued)
            return;
        chunk->cleanupQueued = true;
        const auto it = chunks.find(chunk->mediaPath);
        if (it != chunks.end() && it->second == chunk)
            chunks.erase(it);
        cleanup.push_back(Cleanup{chunk->mediaPath, chunk->metadataPath});
    }

    void retireChunkLocked(const std::shared_ptr<DiskChunk>& chunk, std::vector<Cleanup>& cleanup) {
        if (chunk->entryRefs == 0 && !chunk->retired) {
            chunk->retired = true;
            count.diskBytes -= std::min(count.diskBytes, chunk->bytes);
            dropCompressedHotLocked(chunk);
        }
        queueChunkCleanupLocked(chunk, cleanup);
    }

    void detachEntryLocked(const std::string& identity, std::vector<Cleanup>& cleanup) {
        const auto it = entries.find(identity);
        if (it == entries.end())
            return;
        const std::shared_ptr<DiskChunk> chunk = it->second.chunk;
        entries.erase(it);
        dropDecodedHotLocked(identity);
        if (chunk->entryRefs != 0)
            --chunk->entryRefs;
        retireChunkLocked(chunk, cleanup);
    }

    void detachChunkEntriesLocked(const std::shared_ptr<DiskChunk>& chunk, std::vector<Cleanup>& cleanup) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.chunk != chunk) {
                ++it;
                continue;
            }
            const std::string identity = it->first;
            it = entries.erase(it);
            dropDecodedHotLocked(identity);
            if (chunk->entryRefs != 0)
                --chunk->entryRefs;
        }
        retireChunkLocked(chunk, cleanup);
    }

    void releaseLookupLocked(const std::shared_ptr<DiskChunk>& chunk, std::vector<Cleanup>& cleanup) {
        if (chunk->lookupRefs != 0)
            --chunk->lookupRefs;
        queueChunkCleanupLocked(chunk, cleanup);
    }

    void removeFiles(const std::vector<Cleanup>& cleanup) {
        std::vector<std::string> failures;
        for (const Cleanup& item : cleanup) {
            std::error_code error;
            std::filesystem::remove(item.mediaPath, error);
            if (error)
                failures.push_back("cannot remove cache media '" + item.mediaPath.string() + "': " + error.message());
            error.clear();
            std::filesystem::remove(item.metadataPath, error);
            if (error)
                failures.push_back("cannot remove cache metadata '" + item.metadataPath.string() +
                                   "': " + error.message());
        }
        if (!failures.empty()) {
            std::lock_guard lock(mutex);
            setErrorLocked(failures.front());
        }
    }
    void removeFilesNoThrow(const std::vector<Cleanup>& cleanup) noexcept {
        try {
            removeFiles(cleanup);
        } catch (...) {
            std::lock_guard lock(mutex);
            setErrorLocked("viewer cache cleanup failed");
        }
    }
    void removePathNoThrow(const std::filesystem::path& path) noexcept {
        if (path.empty())
            return;
        std::error_code error;
        std::filesystem::remove(path, error);
        if (error) {
            std::lock_guard lock(mutex);
            setErrorLocked("cannot remove cache path '" + path.string() + "': " + error.message());
        }
    }

    void loadIndexLocked() {
        std::error_code error;
        std::filesystem::create_directories(options.directory, error);
        if (error)
            throw std::runtime_error("viewer cache: cannot create directory '" + options.directory.string() +
                                     "': " + error.message());
        for (const auto& item : std::filesystem::directory_iterator(options.directory, error)) {
            if (error)
                break;
            error.clear();
            if (item.is_symlink(error) || error) {
                error.clear();
                continue;
            }
            if (!item.is_regular_file(error) || item.path().extension() != ".json" ||
                item.path().filename().string().rfind("chunk-", 0) != 0)
                continue;
            try {
                const std::uint64_t metadataBytes = std::filesystem::file_size(item.path());
                if (metadataBytes > 16ULL * 1024ULL * 1024ULL)
                    throw std::runtime_error("cache metadata exceeds the bounded index record size");
                std::ifstream input(item.path());
                if (!input)
                    throw std::runtime_error("cache metadata cannot be opened");
                json metadata;
                input >> metadata;
                if (metadata.value("format", std::string{}) != "nemo-viewer-cache-v2")
                    continue;
                const std::string file = metadata.at("file").get<std::string>();
                const std::filesystem::path mediaName(file);
                if (mediaName.filename() != mediaName || mediaName.extension() != ".mp4" ||
                    mediaName.filename().string().rfind("chunk-", 0) != 0)
                    throw std::runtime_error("media file is not an owned cache chunk");
                const auto mediaPath = options.directory / mediaName;
                std::error_code mediaError;
                if (std::filesystem::is_symlink(mediaPath, mediaError) || mediaError ||
                    !std::filesystem::is_regular_file(mediaPath, mediaError) || mediaError)
                    throw std::runtime_error("media file is missing or is not an owned regular file");
                const std::uint64_t bytes = std::filesystem::file_size(mediaPath);
                if (bytes == 0)
                    throw std::runtime_error("media file is empty");
                if (chunks.contains(mediaPath))
                    throw std::runtime_error("media file has duplicate finalized metadata");

                struct FrameRecord {
                    std::string identity;
                    std::size_t offset;
                    int width;
                    int height;
                };
                std::vector<FrameRecord> frames;
                const auto& frameList = metadata.at("frames");
                if (!frameList.is_array() || frameList.empty())
                    throw std::runtime_error("metadata has no frame index");
                const std::size_t encodedFrameCount = metadata.value("frameCount", frameList.size());
                if (encodedFrameCount == 0 || encodedFrameCount < frameList.size())
                    throw std::runtime_error("metadata has an invalid encoded frame count");
                frames.reserve(frameList.size());
                std::set<std::string> identities;
                for (const auto& frame : frameList) {
                    FrameRecord record{frame.at("identity").get<std::string>(), frame.at("offset").get<std::size_t>(),
                                       frame.at("width").get<int>(), frame.at("height").get<int>()};
                    if (record.identity.empty() || record.width <= 0 || record.height <= 0 ||
                        record.offset >= encodedFrameCount || !identities.insert(record.identity).second ||
                        entries.contains(record.identity))
                        throw std::runtime_error("metadata has an invalid or duplicate frame index");
                    frames.push_back(std::move(record));
                }
                count.diskBytes += bytes;
                if (entries.size() > options.maxMetadataEntries ||
                    frames.size() > options.maxMetadataEntries - entries.size()) {
                    ++count.admissionRejected;
                    count.lastError = "viewer cache metadata admission rejected chunk with " +
                                      std::to_string(frames.size()) + " identities (capacity " +
                                      std::to_string(options.maxMetadataEntries) + ")";
                    continue;
                }

                auto chunk =
                    std::make_shared<DiskChunk>(DiskChunk{mediaPath, item.path(), bytes, 0, 0, {}, false, false});
                chunks.emplace(mediaPath, chunk);
                for (const FrameRecord& frame : frames) {
                    entries.emplace(frame.identity,
                                    DiskEntry{chunk, frame.offset,
                                              ImageLayout{.width = frame.width,
                                                          .height = frame.height,
                                                          .color = ColorInterpretation::DisplayReferred}});
                    ++chunk->entryRefs;
                }

            } catch (const std::exception&) {
                ++count.invalidEntries;
            }
        }
        if (error)
            throw std::runtime_error("viewer cache: cannot scan directory '" + options.directory.string() +
                                     "': " + error.message());
    }
    [[nodiscard]] std::filesystem::path newMediaPath(std::uint64_t hash) {
        for (;;) {
            std::uint64_t id;
            {
                std::lock_guard lock(mutex);
                id = nextFileId++;
            }
            const std::string stem = "chunk-" + hexHash(hash) + "-" + std::to_string(id);
            const auto path = options.directory / (stem + ".mp4");
            const auto temporaryMedia = options.directory / (".pending-" + stem + ".mp4");
            const auto temporaryMetadata = options.directory / (".pending-" + stem + ".json");
            std::error_code error;
            const bool mediaExists = std::filesystem::exists(path, error);
            error.clear();
            const bool metadataExists = std::filesystem::exists(path.string() + ".json", error);
            error.clear();
            const bool temporaryMediaExists = std::filesystem::exists(temporaryMedia, error);
            error.clear();
            const bool temporaryMetadataExists = std::filesystem::exists(temporaryMetadata, error);
            if (!error && !mediaExists && !metadataExists && !temporaryMediaExists && !temporaryMetadataExists)
                return path;
        }
    }

    void process(std::vector<Job> jobs) {
        if (jobs.empty())
            return;
        const std::uint64_t hash = identityHash(jobs.front().chunkGroupKey);
        std::filesystem::path temporary;
        std::filesystem::path publishedMedia;
        std::filesystem::path finalMetadata;
        std::filesystem::path temporaryMetadata;
        std::uint64_t reserved = 0;
        bool mediaPublished = false;
        bool metadataPublished = false;
        try {
            {
                std::lock_guard lock(mutex);
                std::erase_if(jobs, [&](const Job& job) {
                    if (!staleLocked(job))
                        return false;
                    ++count.staleRejected;
                    retireGenerationLocked(job);
                    return true;
                });
                if (jobs.empty())
                    return;
            }
            std::vector<std::size_t> encodedOffsets(jobs.size());
            for (std::size_t index = 0; index < encodedOffsets.size(); ++index)
                encodedOffsets[index] = index;
            const std::size_t encodedFrameCount = jobs.size();
            const auto finalMedia = newMediaPath(hash);
            publishedMedia = finalMedia;
            finalMetadata = finalMedia.string() + ".json";
            const std::string stem = finalMedia.stem().string();
            temporary = options.directory / (".pending-" + stem + ".mp4");
            temporaryMetadata = options.directory / (".pending-" + stem + ".json");

            // Encoder setup and device submission intentionally happen
            // without the state lock. UI freshness calls only take the
            // short bookkeeping lock while this worker waits on the GPU.
            if (!encoder)
                encoder = std::make_unique<media::ViewerChunkEncoder>(instance, device, allocator, options.encoding);
            std::vector<media::DeviceViewerFrame> frames;
            frames.reserve(jobs.size());
            for (const Job& job : jobs)
                frames.push_back({job.image.get(), job.layout});
            {
                std::lock_guard lock(mutex);
                count.encodingFrames = frames.size();
            }
            const media::EncodeStats stats = encoder->encode(temporary.string(), std::span(frames));

            std::error_code fileError;
            const std::uint64_t bytes = std::filesystem::file_size(temporary, fileError);
            if (fileError || bytes == 0)
                throw std::runtime_error("encoder returned without a readable non-empty chunk");

            bool discard = false;
            {
                std::lock_guard lock(mutex);
                addEncodeStats(count.encode, stats);
                count.encodedFrames += static_cast<std::uint64_t>(std::max(stats.encodedFrames, 0));
                for (std::size_t index = jobs.size(); index-- > 0;) {
                    if (!staleLocked(jobs[index]))
                        continue;
                    ++count.staleRejected;
                    retireGenerationLocked(jobs[index]);
                    jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(index));
                    encodedOffsets.erase(encodedOffsets.begin() + static_cast<std::ptrdiff_t>(index));
                }
                if (jobs.empty()) {
                    discard = true;
                } else {
                    std::set<std::string> newIdentities;
                    for (const Job& job : jobs) {
                        if (!entries.contains(job.identity))
                            newIdentities.insert(job.identity);
                    }
                    if (entries.size() > options.maxMetadataEntries ||
                        newIdentities.size() > options.maxMetadataEntries - entries.size()) {
                        ++count.admissionRejected;
                        ++count.errors;
                        count.lastError = "viewer cache metadata admission rejected publication (capacity " +
                                          std::to_string(options.maxMetadataEntries) + ")";
                        retireGenerationsLocked(jobs);
                        discard = true;
                    } else {
                        const std::uint64_t resident = count.diskBytes + reservedDiskBytes;
                        if (bytes > options.maxDiskBytes || resident > options.maxDiskBytes - bytes) {
                            ++count.admissionRejected;
                            ++count.errors;
                            count.lastError = "viewer cache disk admission rejected chunk of " + std::to_string(bytes) +
                                              " bytes (limit " + std::to_string(options.maxDiskBytes) + ")";
                            retireGenerationsLocked(jobs);
                            discard = true;
                        } else {
                            reservedDiskBytes += bytes;
                            reserved = bytes;
                        }
                    }
                }
            }
            if (discard) {
                removePathNoThrow(temporary);
                return;
            }
            json frameMetadata = json::array();
            for (std::size_t index = 0; index < jobs.size(); ++index) {
                frameMetadata.push_back({{"identity", jobs[index].identity},
                                         {"representation", jobs[index].chunkGroupKey},
                                         {"offset", encodedOffsets[index]},
                                         {"width", jobs[index].layout.width},
                                         {"height", jobs[index].layout.height}});
            }
            const json metadata{{"format", "nemo-viewer-cache-v2"},
                                {"file", finalMedia.filename().string()},
                                {"frameCount", encodedFrameCount},
                                {"frames", frameMetadata}};
            {
                std::ofstream output(temporaryMetadata, std::ios::trunc);
                if (!output)
                    throw std::runtime_error("cannot create cache metadata");
                output << metadata.dump();
                output.close();
                if (!output)
                    throw std::runtime_error("cannot write cache metadata");
            }

            // The metadata is not visible until the finalized media exists.
            // A crash between these renames leaves only an ignored orphan.
            std::filesystem::rename(temporary, finalMedia, fileError);
            if (fileError)
                throw std::runtime_error("cannot publish encoded chunk: " + fileError.message());
            mediaPublished = true;
            std::filesystem::rename(temporaryMetadata, finalMetadata, fileError);
            if (fileError)
                throw std::runtime_error("cannot publish cache metadata: " + fileError.message());
            metadataPublished = true;

            auto newChunk =
                std::make_shared<DiskChunk>(DiskChunk{finalMedia, finalMetadata, bytes, 0, 0, {}, false, false});
            std::vector<Cleanup> cleanup;
            bool stale = false;
            {
                std::lock_guard lock(mutex);
                for (const Job& job : jobs) {
                    if (staleLocked(job)) {
                        stale = true;
                        ++count.staleRejected;
                    }
                }
                reservedDiskBytes -= std::min(reservedDiskBytes, reserved);
                if (!stale) {
                    if (!chunks.emplace(finalMedia, newChunk).second)
                        throw std::runtime_error("viewer cache chunk path was concurrently created");
                    for (std::size_t index = 0; index < jobs.size(); ++index) {
                        detachEntryLocked(jobs[index].identity, cleanup);
                        entries.insert_or_assign(jobs[index].identity,
                                                 DiskEntry{newChunk, encodedOffsets[index], jobs[index].layout});
                        ++newChunk->entryRefs;
                    }
                    count.diskBytes += bytes;
                    // `published` is a frame count; each finalized chunk may
                    // independently publish several indexed identities.
                    count.published += jobs.size();
                }
                retireGenerationsLocked(jobs);
            }
            removeFilesNoThrow(cleanup);
            if (stale)
                removeFilesNoThrow({Cleanup{finalMedia, finalMetadata}});
        } catch (const std::exception& error) {
            removePathNoThrow(temporary);
            removePathNoThrow(temporaryMetadata);
            if (mediaPublished)
                removePathNoThrow(publishedMedia);
            if (metadataPublished)
                removePathNoThrow(finalMetadata);
            std::lock_guard lock(mutex);
            if (reserved != 0)
                reservedDiskBytes -= std::min(reservedDiskBytes, reserved);
            retireGenerationsLocked(jobs);
            setErrorLocked(error.what());
        } catch (...) {
            removePathNoThrow(temporary);
            removePathNoThrow(temporaryMetadata);
            if (mediaPublished)
                removePathNoThrow(publishedMedia);
            if (metadataPublished)
                removePathNoThrow(finalMetadata);
            std::lock_guard lock(mutex);
            if (reserved != 0)
                reservedDiskBytes -= std::min(reservedDiskBytes, reserved);
            retireGenerationsLocked(jobs);
            setErrorLocked("viewer cache worker failed with an unknown exception");
        }
    }

    void run() {
        for (;;) {
            std::vector<Job> jobs;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !pending.empty(); });
                if (pending.empty() && stopping)
                    break;
                jobs.push_back(std::move(pending.front()));
                pending.pop_front();
                while (jobs.size() < options.chunkFrames) {
                    const auto next = std::find_if(pending.begin(), pending.end(), [&](const Job& candidate) {
                        return candidate.chunkGroupKey == jobs.front().chunkGroupKey &&
                               sameLayout(candidate.layout, jobs.front().layout);
                    });
                    if (next == pending.end())
                        break;
                    jobs.push_back(std::move(*next));
                    pending.erase(next);
                }
                active = true;
                count.pendingFrames = pending.size();
                count.activeFrames = jobs.size();
                count.peakPendingFrames = std::max(count.peakPendingFrames, count.pendingFrames + count.activeFrames);
            }
            process(std::move(jobs));
            {
                std::lock_guard lock(mutex);
                active = false;
                count.activeFrames = 0;
                count.encodingFrames = 0;
                if (pending.empty())
                    idle.notify_all();
            }
        }
        std::lock_guard lock(mutex);
        active = false;
        count.activeFrames = 0;
        count.encodingFrames = 0;
        idle.notify_all();
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
        releaseWriterLock();
    }
};

ViewerCache::ViewerCache(gpu::Instance& instance, gpu::Device& device, gpu::Allocator& allocator,
                         const std::filesystem::path& convertSpirv)
    : impl_(std::make_unique<Impl>(instance, device, allocator, convertSpirv)) {}

ViewerCache::~ViewerCache() {
    if (impl_)
        impl_->stop();
}

void ViewerCache::configure(const ViewerCacheOptions& options) {
    if (options.directory.empty())
        throw std::invalid_argument("viewer cache directory must not be empty");
    if (options.chunkFrames == 0 || options.maxPendingFrames == 0 || options.maxDecodedFrames == 0 ||
        options.maxMetadataEntries == 0)
        throw std::invalid_argument("viewer cache chunk, queue, and metadata bounds must be positive");
    if (options.maxDiskBytes == 0)
        throw std::invalid_argument("viewer cache disk bound must be positive");
    std::lock_guard lock(impl_->mutex);
    if (impl_->configured)
        throw std::logic_error("viewer cache is already configured");
    impl_->options = options;
    impl_->encodingFailure = options.encoding.injectedFailure
                                 ? std::optional<media::EncodeFailure>(*options.encoding.injectedFailure)
                                 : std::nullopt;
    impl_->options.encoding.injectedFailure = impl_->encodingFailure ? &*impl_->encodingFailure : nullptr;
    try {
        impl_->acquireWriterLockLocked();
        impl_->loadIndexLocked();
        impl_->configured = true;
        impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    } catch (...) {
        impl_->configured = false;
        impl_->entries.clear();
        impl_->chunks.clear();
        impl_->latestGenerationByIdentity.clear();
        impl_->latestRevisionByDestination.clear();
        impl_->latestGenerationByDestination.clear();
        impl_->count.diskBytes = 0;
        impl_->releaseWriterLock();
        throw;
    }
}

std::optional<ViewerCacheResult> ViewerCache::lookup(const std::string& identity, const ImageLayout& expected,
                                                     std::uint64_t timeout_ns) {
    return impl_->lookup(identity, expected, timeout_ns);
}

std::optional<ViewerCacheResult> ViewerCache::Impl::lookup(const std::string& identity, const ImageLayout& expected,
                                                           std::uint64_t timeout_ns) {
    std::filesystem::path path;
    std::size_t offset = 0;
    std::shared_ptr<DiskChunk> chunk;
    std::shared_ptr<const std::vector<std::uint8_t>> compressedHot;
    {
        std::lock_guard lock(mutex);
        if (!configured)
            return std::nullopt;
        const auto it = entries.find(identity);
        if (it == entries.end()) {
            ++count.misses;
            return std::nullopt;
        }
        if (!sameLayout(it->second.layout, expected)) {
            ++count.misses;
            return std::nullopt;
        }
        const auto hot = decodedHot.find(identity);
        if (hot != decodedHot.end() && sameLayout(hot->second.layout, expected)) {
            ++count.hits;
            ++count.decodedHotHits;
            return ViewerCacheResult{hot->second.image, expected};
        }
        if (decodedInFlight >= options.maxDecodedFrames) {
            ++count.misses;
            return std::nullopt;
        }
        ++decodedInFlight;
        count.decodedQueuePeak = std::max(count.decodedQueuePeak, decodedInFlight);
        chunk = it->second.chunk;
        compressedHot = chunk->compressedHot;
        if (compressedHot)
            ++count.compressedHotHits;
        path = chunk->mediaPath;
        offset = it->second.offset;
        ++chunk->lookupRefs;
    }

    const auto releaseDecoded = [&] {
        std::vector<Cleanup> cleanup;
        {
            std::lock_guard lock(mutex);
            if (decodedInFlight != 0)
                --decodedInFlight;
            releaseLookupLocked(chunk, cleanup);
        }
        removeFilesNoThrow(cleanup);
    };
    const auto invalidate = [&] {
        std::vector<Cleanup> cleanup;
        {
            std::lock_guard lock(mutex);
            const auto it = entries.find(identity);
            // outside the lock. Never invalidate that newer valid entry.
            if (it != entries.end() && it->second.chunk == chunk)
                detachChunkEntriesLocked(chunk, cleanup);
        }
        removeFilesNoThrow(cleanup);
    };
    const auto recordInvalid = [&] {
        std::lock_guard lock(mutex);
        ++count.misses;
        ++count.invalidEntries;
    };
    const auto recordTransient = [&](const std::string& message) {
        std::lock_guard lock(mutex);
        setErrorLocked(message);
    };

    try {
        if (!compressedHot) {
            const auto loaded = readCompressedHot(path, chunk->bytes);
            if (loaded) {
                std::lock_guard lock(mutex);
                if (!chunk->retired) {
                    retainCompressedHotLocked(chunk, loaded);
                    compressedHot = chunk->compressedHot;
                }
            }
        }
        auto decoder = compressedHot
                           ? media::ClipDecoder::openViewerMemory(instance, device, allocator, path.string(),
                                                                  compressedHot, convertSpirv)
                           : media::ClipDecoder::openViewer(instance, device, allocator, path.string(), convertSpirv);
        std::unique_ptr<gpu::Image> decodedImage;
        for (std::size_t index = 0; index <= offset; ++index) {
            decodedImage = decoder->nextViewer(timeout_ns);
            if (!decodedImage)
                throw media::MediaDecodeError(path.string(), "viewer chunk", "replay frame index is outside the chunk");
        }
        const bool hardwareReplay = decoder->decision().hardware;
        const std::string replayReason = decoder->decision().reason;
        const auto extent = decodedImage->extent();
        const auto codedWidth = (static_cast<std::uint32_t>(expected.width) + 1U) & ~1U;
        const auto codedHeight = (static_cast<std::uint32_t>(expected.height) + 1U) & ~1U;
        if (extent.width != codedWidth || extent.height != codedHeight)
            throw media::MediaDecodeError(path.string(), "viewer chunk",
                                          "coded dimensions do not match the cache index");
        if (extent.width != static_cast<std::uint32_t>(expected.width) ||
            extent.height != static_cast<std::uint32_t>(expected.height))
            *decodedImage = gpu::cropRgba32fImage(device.submissions(device.graphics_family()), allocator,
                                                  *decodedImage, expected.width, expected.height, timeout_ns);
        auto image = std::make_shared<gpu::Image>(std::move(*decodedImage));
        std::vector<Cleanup> cleanup;
        {
            std::lock_guard lock(mutex);
            const auto current = entries.find(identity);
            if (current != entries.end() && current->second.chunk == chunk)
                retainDecodedHotLocked(identity, expected, image);
            if (decodedInFlight != 0)
                --decodedInFlight;
            releaseLookupLocked(chunk, cleanup);
            ++count.decodedFrames;
            if (hardwareReplay)
                ++count.hardwareDecodedFrames;
            else {
                ++count.softwareDecodedFrames;
                if (!replayReason.empty())
                    count.replayFallbackReason = replayReason;
            }
            ++count.hits;
        }
        removeFilesNoThrow(cleanup);
        return ViewerCacheResult{std::move(image), expected};
    } catch (const gpu::GpuException& error) {
        releaseDecoded();
        recordTransient(std::string("viewer cache replay GPU failure: ") + error.what());
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        releaseDecoded();
        recordTransient("viewer cache replay allocation failed");
        return std::nullopt;
    } catch (const media::MediaDecodeError& error) {
        releaseDecoded();
        if (transientReplayFailure(error)) {
            recordTransient(std::string("viewer cache replay I/O failure: ") + error.what());
            return std::nullopt;
        }
        invalidate();
        recordInvalid();
        return std::nullopt;
    } catch (const std::exception& error) {
        releaseDecoded();
        if (transientReplayFailure(error)) {
            recordTransient(std::string("viewer cache replay transient failure: ") + error.what());
            return std::nullopt;
        }
        invalidate();
        recordInvalid();
        return std::nullopt;
    }
}

bool ViewerCache::enqueue(ViewerCachePublication publication) {
    std::lock_guard lock(impl_->mutex);
    return enqueueLocked(std::move(publication));
}

bool ViewerCache::enqueueBatch(std::span<ViewerCachePublication> publications) {
    std::lock_guard lock(impl_->mutex);
    if (publications.size() + impl_->count.activeFrames > impl_->options.maxPendingFrames)
        return false;
    bool accepted = true;
    for (auto& publication : publications)
        accepted = enqueueLocked(std::move(publication)) && accepted;
    return accepted;
}

bool ViewerCache::enqueueLocked(ViewerCachePublication publication) {
    if (publication.identity.empty() || publication.chunkGroupKey.empty() || !publication.image ||
        publication.layout.color != ColorInterpretation::DisplayReferred)
        return false;
    if (!impl_->configured || impl_->stopping)
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

    const bool alreadyIndexed = impl_->entries.contains(publication.identity);
    const bool alreadyPending =
        std::any_of(impl_->pending.begin(), impl_->pending.end(),
                    [&](const Impl::Job& pending) { return pending.identity == publication.identity; });
    if (!alreadyIndexed && !alreadyPending && impl_->entries.size() >= impl_->options.maxMetadataEntries) {
        ++impl_->count.admissionRejected;
        impl_->count.lastError = "viewer cache metadata capacity reached";
        return false;
    }
    std::erase_if(impl_->pending, [&](const Impl::Job& pending) {
        if (pending.identity != publication.identity)
            return false;
        impl_->retireGenerationLocked(pending);
        return true;
    });
    // The worker removes a whole batch from pending, so activeFrames is the
    // exact number of retained images and must be counted frame-for-frame.
    while (impl_->pending.size() + impl_->count.activeFrames >= impl_->options.maxPendingFrames) {
        if (impl_->pending.empty()) {
            ++impl_->count.admissionDropped;
            return false;
        }
        const Impl::Job dropped = std::move(impl_->pending.front());
        impl_->pending.pop_front();
        impl_->retireGenerationLocked(dropped);
        ++impl_->count.admissionDropped;
    }
    impl_->pending.push_back(std::move(publication));
    try {
        const auto& pending = impl_->pending.back();
        impl_->latestGenerationByIdentity[{pending.destination, pending.identity}] = pending.generation;
    } catch (...) {
        impl_->pending.pop_back();
        throw;
    }
    impl_->count.pendingFrames = impl_->pending.size();
    impl_->count.peakPendingFrames =
        std::max(impl_->count.peakPendingFrames, impl_->count.pendingFrames + impl_->count.activeFrames);
    impl_->wake.notify_one();
    return true;
}

void ViewerCache::supersede(std::uint64_t revision, std::uint64_t generation, ViewerDestination destination) {
    std::lock_guard lock(impl_->mutex);
    const auto current = impl_->latestGenerationByDestination.find(destination);
    if (current != impl_->latestGenerationByDestination.end() && generation < current->second)
        return;
    impl_->latestGenerationByDestination[destination] = generation;
    impl_->latestRevisionByDestination[destination] = revision;
}

const ViewerCacheOptions& ViewerCache::optionsForIdentity() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->options;
}

void ViewerCache::flush() {
    std::unique_lock lock(impl_->mutex);
    impl_->idle.wait(lock, [&] { return impl_->pending.empty() && !impl_->active; });
    if (impl_->count.errors != 0)
        throw std::runtime_error(impl_->count.lastError.empty() ? "viewer cache encoding failed"
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
