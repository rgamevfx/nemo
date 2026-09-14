#include "nemo/media/SequenceDiscovery.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace nemo::media {
namespace {

// A frame number wider than this cannot be an int64 and is not a plausible
// frame index; such a run is ignored rather than misparsed.
constexpr std::size_t kMaxFrameDigits = 18;

[[nodiscard]] SequenceDiscovery stillAt(std::string path) {
    SequenceDiscovery result;
    result.status = SequenceDiscoveryStatus::Still;
    result.pattern = std::move(path);
    return result;
}

[[nodiscard]] SequenceDiscovery failed(std::string detail) {
    SequenceDiscovery result;
    result.status = SequenceDiscoveryStatus::Failed;
    result.detail = std::move(detail);
    return result;
}

[[nodiscard]] bool isDigit(const char character) {
    return std::isdigit(static_cast<unsigned char>(character)) != 0;
}

struct Run {
    std::size_t start{0};
    std::size_t length{0};
};

// The first '#'/'@' marker run of a name, if any: the explicit pattern form.
[[nodiscard]] std::optional<Run> firstMarkerRun(const std::string& text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char marker = text[index];
        if (marker != '#' && marker != '@') {
            continue;
        }
        std::size_t end = index;
        while (end < text.size() && text[end] == marker) {
            ++end;
        }
        return Run{index, end - index};
    }
    return std::nullopt;
}

// Every digit run of one literal file name, as a candidate numbering. Which run
// is the frame is decided by MATCHING-MEMBER EVIDENCE below, never by run
// width: "plate_v00012.1001.exr" has a five-digit version run and a four-digit
// frame run, and only the frame run has sibling members.
[[nodiscard]] std::vector<Run> digitRuns(const std::string& text) {
    std::vector<Run> runs;
    std::size_t index = 0;
    while (index < text.size()) {
        if (!isDigit(text[index])) {
            ++index;
            continue;
        }
        const std::size_t start = index;
        while (index < text.size() && isDigit(text[index])) {
            ++index;
        }
        const Run run{start, index - start};
        if (run.length <= kMaxFrameDigits) {
            runs.push_back(run);
        }
    }
    return runs;
}

[[nodiscard]] std::optional<std::int64_t> parseFrame(std::string_view digits) {
    if (digits.empty() || digits.size() > kMaxFrameDigits) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char character : digits) {
        const int digit = character - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

// One candidate numbering of a literal selection: the name is
// prefix + <digits of at least `padding` width> + suffix.
struct Candidate {
    std::string prefix;
    std::string suffix;
    std::size_t padding{0};
    std::size_t runStart{0};
    std::size_t runLength{0};
};

// The frames of `names` this candidate selects, sorted and deduplicated.
// `cancel` is polled while matching, so a superseded request stops early.
[[nodiscard]] std::vector<std::int64_t> membersOf(const std::vector<std::string>& names, const Candidate& candidate,
                                                  const std::atomic<bool>* cancel) {
    std::vector<std::int64_t> frames;
    for (const std::string& name : names) {
        if (cancel != nullptr && cancel->load()) {
            return {};
        }
        if (name.size() <= candidate.prefix.size() + candidate.suffix.size()) {
            continue;
        }
        if (name.compare(0, candidate.prefix.size(), candidate.prefix) != 0 ||
            name.compare(name.size() - candidate.suffix.size(), candidate.suffix.size(), candidate.suffix) != 0) {
            continue;
        }
        const std::string digits =
            name.substr(candidate.prefix.size(), name.size() - candidate.prefix.size() - candidate.suffix.size());
        if (digits.size() < candidate.padding || !std::all_of(digits.begin(), digits.end(), isDigit)) {
            continue;
        }
        if (const auto frame = parseFrame(digits)) {
            frames.push_back(*frame);
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

// The pattern text of one candidate, rooted at its directory.
[[nodiscard]] std::string patternOf(const std::filesystem::path& directory, const Candidate& candidate) {
    return (directory / (candidate.prefix + std::string(candidate.padding, '#') + candidate.suffix)).string();
}

}  // namespace

SequenceDiscovery discoverSequenceRange(const std::string& path, const std::atomic<bool>* cancel,
                                        const SequenceDiscoveryLimits& limits) {
    if (path.empty()) {
        return failed("sequence discovery: empty path");
    }
    const std::filesystem::path selected(path);
    const std::string filename = selected.filename().string();
    if (filename.empty()) {
        return failed("sequence discovery: '" + path + "' names no file");
    }
    const std::filesystem::path directory =
        selected.parent_path().empty() ? std::filesystem::path(".") : selected.parent_path();

    std::vector<Candidate> candidates;
    bool explicitMarker = false;
    if (const auto marker = firstMarkerRun(filename)) {
        explicitMarker = true;
        Candidate candidate;
        candidate.prefix = filename.substr(0, marker->start);
        candidate.suffix = filename.substr(marker->start + marker->length);
        candidate.padding = marker->length;
        candidate.runStart = marker->start;
        candidate.runLength = marker->length;
        candidates.push_back(std::move(candidate));
    } else {
        const std::string extension = selected.extension().string();
        const std::string stem = filename.substr(0, filename.size() - extension.size());
        for (const Run& run : digitRuns(stem)) {
            if (!parseFrame(stem.substr(run.start, run.length))) {
                continue;  // a run too wide to be a frame index is not a candidate
            }
            Candidate candidate;
            candidate.prefix = stem.substr(0, run.start);
            candidate.suffix = stem.substr(run.start + run.length) + extension;
            candidate.padding = run.length;
            candidate.runStart = run.start;
            candidate.runLength = run.length;
            candidates.push_back(std::move(candidate));
        }
        if (candidates.empty()) {
            return stillAt(path);  // no numbered frame in this name
        }
    }

    // One bounded, cancellable directory listing serves every candidate, so the
    // evidence comparison costs a single pass and never grows per candidate.
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return failed("sequence discovery: cannot read directory '" + directory.string() + "': " + error.message());
    }
    std::vector<std::string> names;
    std::size_t examined = 0;
    for (const std::filesystem::directory_entry& entry : iterator) {
        if (cancel != nullptr && cancel->load()) {
            return failed("sequence discovery: cancelled");
        }
        if (++examined > limits.maxDirectoryEntries) {
            return failed("sequence discovery: '" + directory.string() + "' exceeds the " +
                          std::to_string(limits.maxDirectoryEntries) + " entry scan bound");
        }
        std::error_code typeError;
        if (!entry.is_regular_file(typeError)) {
            continue;
        }
        names.push_back(entry.path().filename().string());
    }

    if (cancel != nullptr && cancel->load()) {
        return failed("sequence discovery: cancelled");
    }

    // Which run is the frame is decided by members that are NOT the selected
    // file: a run with siblings is the sequence numbering; a run that only ever
    // selects the selected file is the file name itself (or a version token).
    std::vector<std::vector<std::int64_t>> members;
    members.reserve(candidates.size());
    std::vector<std::size_t> supported;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        members.push_back(membersOf(names, candidates[index], cancel));
        if (members.back().size() >= 2) {
            supported.push_back(index);
        }
    }
    if (cancel != nullptr && cancel->load()) {
        return failed("sequence discovery: cancelled");
    }

    std::size_t chosen = 0;
    if (supported.size() == 1) {
        chosen = supported.front();
    } else if (supported.size() > 1) {
        // Two different numberings both select real members: guessing would
        // silently reinterpret one of them (e.g. a version token as the frame),
        // so the selection is reported ambiguous and an explicit '#'/'@'
        // pattern is required.
        std::string detail = "sequence discovery: '" + path + "' has an ambiguous numbered run (";
        for (std::size_t index = 0; index < supported.size(); ++index) {
            if (index > 0) {
                detail += ", ";
            }
            const Candidate& candidate = candidates[supported[index]];
            detail += "'" + candidate.prefix + std::string(candidate.padding, '#') + candidate.suffix + "'";
        }
        detail += "); use an explicit '#' or '@' pattern";
        SequenceDiscovery ambiguous;
        ambiguous.status = SequenceDiscoveryStatus::Ambiguous;
        ambiguous.pattern = path;
        ambiguous.detail = std::move(detail);
        return ambiguous;
    } else if (explicitMarker) {
        // An authored '#'/'@' pattern with no matching member is an error naming
        // the pattern: the artist asked for a sequence and there is none.
        return failed("sequence discovery: no files match '" + path + "'");
    } else {
        // No run has a sibling: the selection denotes exactly one file, so the
        // widest (ties: last) run is only used to describe it and the result is
        // a still — a one-frame delivery is never reinterpreted as a sequence.
        std::size_t best = 0;
        for (std::size_t index = 1; index < candidates.size(); ++index) {
            if (candidates[index].runLength >= candidates[best].runLength) {
                best = index;
            }
        }
        chosen = best;
        if (members[chosen].size() < 2) {
            return stillAt(path);
        }
    }

    const Candidate& candidate = candidates[chosen];
    std::vector<std::int64_t>& frames = members[chosen];
    if (frames.empty()) {
        if (explicitMarker) {
            return failed("sequence discovery: no files match '" + path + "'");
        }
        return stillAt(path);
    }
    if (!explicitMarker && frames.size() == 1) {
        return stillAt(path);
    }

    SequenceDiscovery result;
    result.status = SequenceDiscoveryStatus::Sequence;
    result.pattern = explicitMarker ? path : patternOf(directory, candidate);
    result.first = frames.front();
    result.last = frames.back();
    result.availableCount = static_cast<std::int64_t>(frames.size());
    const std::int64_t span = result.last - result.first + 1;
    if (span > limits.maxFrameSpan) {
        return failed("sequence discovery: '" + path + "' spans " + std::to_string(span) + " frames, exceeding the " +
                      std::to_string(limits.maxFrameSpan) + " frame bound");
    }
    result.missingCount = span - result.availableCount;
    for (std::size_t index = 1; index < frames.size(); ++index) {
        if (frames[index] > frames[index - 1] + 1) {
            result.holes.push_back(SequenceFrameRange{frames[index - 1] + 1, frames[index] - 1});
        }
    }
    return result;
}

}  // namespace nemo::media
