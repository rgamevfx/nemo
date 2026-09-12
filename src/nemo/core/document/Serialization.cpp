#include "nemo/core/document/Serialization.hpp"

#include "nemo/core/document/ParameterValueJson.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace nemo {
namespace {

// ---------------------------------------------------------------------------
// Format discriminator and required-feature metadata.
// ---------------------------------------------------------------------------

struct SupportedFeature {
    std::string_view id;
    int version;
};

// Authored processing features this build reads. A project file records the
// features it uses in "requiredFeatures"; an unknown id or a newer version is
// rejected rather than guessed.
constexpr std::array<SupportedFeature, 4> kSupportedFeatures{
    {{"networks", 1}, {"instances", 1}, {"animation", 1}, {"mediaLibrary", 1}}};

const SupportedFeature* supportedFeature(std::string_view id) {
    for (const auto& feature : kSupportedFeatures)
        if (feature.id == id)
            return &feature;
    return nullptr;
}

std::string supportedFeatureList() {
    std::string result;
    for (const auto& feature : kSupportedFeatures) {
        if (!result.empty())
            result += ", ";
        result += std::string(feature.id) + " v" + std::to_string(feature.version);
    }
    return result;
}

nlohmann::json requiredFeaturesJson(const Document& document) {
    nlohmann::json features = nlohmann::json::array();
    features.push_back({{"id", "networks"}, {"version", 1}});
    if (!document.instances().empty())
        features.push_back({{"id", "instances"}, {"version", 1}});
    if (!document.animationChannels().empty())
        features.push_back({{"id", "animation"}, {"version", 1}});
    if (!document.mediaCatalog.entries().empty() || !document.mediaCatalog.bins().empty() ||
        document.mediaCatalog.nextEntryId() > 1 || document.mediaCatalog.nextBinId() > 1)
        features.push_back({{"id", "mediaLibrary"}, {"version", 1}});
    return features;
}

void checkRequiredFeatures(const nlohmann::json& json) {
    const auto it = json.find("requiredFeatures");
    if (it == json.end())
        return;
    if (!it->is_array())
        throw DeserializeError("document 'requiredFeatures' must be an array");
    std::set<std::string> seen;
    for (std::size_t index = 0; index < it->size(); ++index) {
        const auto& entry = it->at(index);
        const std::string context = "document requiredFeatures[" + std::to_string(index) + "]";
        if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string() ||
            entry.at("id").get<std::string>().empty())
            throw DeserializeError(context + ": 'id' must be a nonempty string");
        if (!entry.contains("version") || !entry.at("version").is_number_integer())
            throw DeserializeError(context + ": 'version' must be a positive integer");
        const auto id = entry.at("id").get<std::string>();
        const auto version = entry.at("version").get<std::int64_t>();
        if (version <= 0)
            throw DeserializeError(context + ": 'version' must be a positive integer");
        if (!seen.insert(id).second)
            throw DeserializeError(context + ": duplicate feature '" + id + "'");
        const SupportedFeature* feature = supportedFeature(id);
        if (feature == nullptr)
            throw DeserializeError("document requires unsupported project feature '" + id + "' version " +
                                   std::to_string(version) + "; this build supports " + supportedFeatureList());
        if (version > feature->version)
            throw DeserializeError("document requires project feature '" + id + "' version " + std::to_string(version) +
                                   ", but this build supports version " + std::to_string(feature->version));
    }
}

// ---------------------------------------------------------------------------
// Lossless preservation of authored JSON this build does not model.
// ---------------------------------------------------------------------------

// Returns null when nothing is preserved so a record with no unrecognized
// fields compares equal to its default-constructed model counterpart.
nlohmann::json collectUnknownFields(const nlohmann::json& object, std::initializer_list<std::string_view> known) {
    nlohmann::json result;
    if (!object.is_object())
        return result;
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool recognized = false;
        for (const auto key : known) {
            if (it.key() == key) {
                recognized = true;
                break;
            }
        }
        if (!recognized)
            result[it.key()] = it.value();
    }
    return result;
}

void applyUnknownFields(nlohmann::json& target, const nlohmann::json& unknown) {
    if (!unknown.is_object())
        return;
    for (auto it = unknown.begin(); it != unknown.end(); ++it)
        target[it.key()] = it.value();
}

// ---------------------------------------------------------------------------
// Scalar helpers.
// ---------------------------------------------------------------------------

std::uint64_t requiredId(const nlohmann::json& object, const char* field, const std::string& context) {
    const auto it = object.find(field);
    if (it == object.end() || (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)))
        throw DeserializeError(context + ": '" + field + "' must be a nonnegative integer");
    const auto value = it->get<std::uint64_t>();
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max())
        throw DeserializeError(context + ": '" + field + "' must be nonzero and below the identity limit");
    return value;
}

std::optional<std::uint64_t> optionalId(const nlohmann::json& object, const char* field, const std::string& context) {
    if (!object.contains(field))
        return std::nullopt;
    return requiredId(object, field, context);
}

std::string stringField(const nlohmann::json& object, const char* field, const std::string& context) {
    const auto it = object.find(field);
    if (it == object.end())
        return {};
    if (!it->is_string())
        throw DeserializeError(context + ": '" + field + "' must be a string");
    return it->get<std::string>();
}

std::uint32_t port(const nlohmann::json& endpoint, const std::string& context) {
    const auto it = endpoint.find("port");
    if (it == endpoint.end() ||
        (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)) ||
        it->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
        throw DeserializeError(context + ": 'port' must be an unsigned 32-bit integer");
    return it->get<std::uint32_t>();
}

std::uint64_t endpointNode(const nlohmann::json& endpoint, const std::string& context) {
    if (!endpoint.is_object())
        throw DeserializeError(context + " must be an object");
    return requiredId(endpoint, "node", context);
}

std::optional<std::uint64_t> watermark(const nlohmann::json& object, const char* field, const char* context) {
    if (!object.contains(field))
        return std::nullopt;
    return requiredId(object, field, context);
}

PortKind parseKind(const nlohmann::json& value, const std::string& context) {
    if (!value.is_string())
        throw DeserializeError(context + ": port kind must be a string");
    const auto kind = value.get<std::string>();
    if (kind == "image")
        return PortKind::Image;
    if (kind == "mask")
        return PortKind::Mask;
    if (kind == "media")
        return PortKind::Media;
    throw DeserializeError(context + ": unknown port kind '" + kind + "'");
}

std::int64_t signedValue(const nlohmann::json& value, const std::string& context) {
    if (value.is_number_unsigned()) {
        if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw DeserializeError(context + " must be a signed 64-bit integer");
        return static_cast<std::int64_t>(value.get<std::uint64_t>());
    }
    if (!value.is_number_integer())
        throw DeserializeError(context + " must be a signed 64-bit integer");
    return value.get<std::int64_t>();
}

std::uint64_t unsignedValue(const nlohmann::json& value, const std::string& context) {
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
        throw DeserializeError(context + " must be a nonnegative integer");
    return value.get<std::uint64_t>();
}

const char* kindName(PortKind kind) {
    switch (kind) {
    case PortKind::Image:
        return "image";
    case PortKind::Mask:
        return "mask";
    case PortKind::Media:
        return "media";
    }
    return "image";
}

LayoutPosition parseLayout(const nlohmann::json& value, const std::string& context) {
    if (!value.is_object() || !value.contains("x") || !value.contains("y") || !value.at("x").is_number() ||
        !value.at("y").is_number())
        throw DeserializeError(context + ": layout must contain numeric x and y");
    const double x = value.at("x").get<double>();
    const double y = value.at("y").get<double>();
    if (!std::isfinite(x) || !std::isfinite(y))
        throw DeserializeError(context + ": layout coordinates must be finite");
    return LayoutPosition{x, y};
}

nlohmann::json layoutJson(const LayoutPosition& layout) {
    return {{"x", layout.x}, {"y", layout.y}};
}

std::vector<PortSpec> parsePorts(const nlohmann::json& value, const std::string& context) {
    if (!value.is_array())
        throw DeserializeError(context + " must be an array");
    std::vector<PortSpec> ports;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto& entry = value.at(index);
        if (!entry.is_object() || !entry.contains("kind") || !entry.contains("name") || !entry.at("name").is_string())
            throw DeserializeError(context + "[" + std::to_string(index) + "]: kind and string name are required");
        ports.push_back(PortSpec{parseKind(entry.at("kind"), context), entry.at("name").get<std::string>()});
    }

    return ports;
}

ParameterValues parseParameterValues(const nlohmann::json& value, int schema, std::string_view type,
                                     const NodeCatalog& catalog, const std::string& context) {
    if (!value.is_object())
        throw DeserializeError(context + ": 'params' must be an object");
    ParameterValues params;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string fieldContext = context + " key '" + it.key() + "'";
        try {
            ParameterValue parsed;
            if (schema >= 3) {
                parsed = parameterValueFromJson(it.value());
            } else {
                if (!it.value().is_string())
                    throw DeserializeError(fieldContext + ": legacy parameter must be a string");
                parsed = catalog.parseParameterText(type, it.key(), it.value().get<std::string>());
            }
            if (const auto problem = catalog.validateParameter(type, it.key(), parsed))
                throw DeserializeError(fieldContext + ": " + *problem);
            params.emplace(it.key(), std::move(parsed));
        } catch (const DeserializeError&) {
            throw;
        } catch (const std::exception& error) {
            throw DeserializeError(fieldContext + ": " + error.what());
        }
    }
    return params;
}

// Parameter records of an unavailable node type. Records this build cannot type
// are kept verbatim in `opaque` instead of failing the load, so missing node
// state survives a save/reopen rather than silently disappearing.
ParameterValues parseUnavailableParameterValues(const nlohmann::json& value, int schema, std::string_view type,
                                                const NodeCatalog& catalog, const std::string& context,
                                                nlohmann::json& opaque) {
    if (!value.is_object())
        throw DeserializeError(context + ": 'params' must be an object");
    ParameterValues params;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (schema >= 3) {
            const auto& raw = it.value();
            // A tag this build does not know is preserved verbatim; a known tag
            // with a malformed payload stays an error even for an unavailable
            // type so unsafe data cannot silently survive.
            if (raw.is_object() && raw.contains("type") && raw.at("type").is_string() &&
                !parameterValueTypeTagKnown(raw.at("type").get<std::string>())) {
                opaque[it.key()] = raw;
                continue;
            }
            ParameterValue parsed;
            try {
                parsed = parameterValueFromJson(raw);
            } catch (const std::exception& error) {
                throw DeserializeError(context + " key '" + it.key() + "': " + error.what());
            }
            if (const auto problem = catalog.validateParameter(type, it.key(), parsed))
                throw DeserializeError(context + " key '" + it.key() + "': " + *problem);
            params.emplace(it.key(), std::move(parsed));
            continue;
        }
        try {
            if (!it.value().is_string())
                throw std::invalid_argument("legacy parameter must be a string");
            const ParameterValue parsed = catalog.parseParameterText(type, it.key(), it.value().get<std::string>());
            if (const auto problem = catalog.validateParameter(type, it.key(), parsed))
                throw std::invalid_argument(*problem);
            params.emplace(it.key(), parsed);
        } catch (const std::exception&) {
            opaque[it.key()] = it.value();
        }
    }
    return params;
}

nlohmann::json parameterValuesJson(const ParameterValues& params) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [key, value] : params)
        result[key] = parameterValueToJson(value);
    return result;
}

const char* interpolationName(KeyInterpolation value) {
    switch (value) {
    case KeyInterpolation::Hold:
        return "hold";
    case KeyInterpolation::Linear:
        return "linear";
    case KeyInterpolation::Bezier:
        return "bezier";
    }
    throw std::logic_error("invalid animation interpolation");
}

double animationNumber(const nlohmann::json& value, const std::string& context) {
    if (!value.is_number())
        throw DeserializeError(context + " must be numeric");
    const auto number = value.get<double>();
    if (!std::isfinite(number))
        throw DeserializeError(context + " must be finite");
    return number;
}

std::array<double, 4> animationSlopes(const nlohmann::json& value, const std::string& context) {
    if (!value.is_array() || value.size() != 4)
        throw DeserializeError(context + " must contain four slope components");
    std::array<double, 4> slopes;
    for (std::size_t i = 0; i < slopes.size(); ++i)
        slopes[i] = animationNumber(value.at(i), context + "[" + std::to_string(i) + "]");
    return slopes;
}

// ---------------------------------------------------------------------------
// Media library.
// ---------------------------------------------------------------------------

const char* mediaKindName(MediaKind kind) {
    switch (kind) {
    case MediaKind::Unknown:
        return "unknown";
    case MediaKind::Image:
        return "image";
    case MediaKind::Video:
        return "video";
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Sequence:
        return "sequence";
    case MediaKind::Other:
        return "other";
    }
    return "unknown";
}

MediaKind parseMediaKind(const nlohmann::json& value, const std::string& context) {
    if (!value.is_string())
        throw DeserializeError(context + ": media kind must be a string");
    const auto kind = value.get<std::string>();
    if (kind == "unknown")
        return MediaKind::Unknown;
    if (kind == "image")
        return MediaKind::Image;
    if (kind == "video")
        return MediaKind::Video;
    if (kind == "audio")
        return MediaKind::Audio;
    if (kind == "sequence")
        return MediaKind::Sequence;
    if (kind == "other")
        return MediaKind::Other;
    throw DeserializeError(context + ": unknown media kind '" + kind + "'");
}

const char* probeStatusName(MediaProbeStatus status) {
    switch (status) {
    case MediaProbeStatus::Unknown:
        return "unknown";
    case MediaProbeStatus::Pending:
        return "pending";
    case MediaProbeStatus::Ready:
        return "ready";
    case MediaProbeStatus::Failed:
        return "failed";
    }
    return "unknown";
}

MediaProbeStatus parseProbeStatus(const nlohmann::json& value, const std::string& context) {
    if (!value.is_string())
        throw DeserializeError(context + ": probe status must be a string");
    const auto status = value.get<std::string>();
    if (status == "unknown")
        return MediaProbeStatus::Unknown;
    if (status == "pending")
        return MediaProbeStatus::Pending;
    if (status == "ready")
        return MediaProbeStatus::Ready;
    if (status == "failed")
        return MediaProbeStatus::Failed;
    throw DeserializeError(context + ": unknown probe status '" + status + "'");
}

nlohmann::json probeJson(const MediaProbeMetadata& probe) {
    nlohmann::json value{{"width", probe.width},
                         {"height", probe.height},
                         {"duration", probe.duration},
                         {"codec", probe.codec},
                         {"colorPrimaries", probe.colorPrimaries},
                         {"colorTransfer", probe.colorTransfer},
                         {"colorMatrix", probe.colorMatrix},
                         {"provenance", probe.provenance},
                         {"status", probeStatusName(probe.status)}};
    applyUnknownFields(value, probe.extension);
    return value;
}

MediaProbeMetadata parseProbe(const nlohmann::json& value, const std::string& context) {
    if (!value.is_object())
        throw DeserializeError(context + " must be an object");
    MediaProbeMetadata probe;
    if (value.contains("width"))
        probe.width = signedValue(value.at("width"), context + " width");
    if (value.contains("height"))
        probe.height = signedValue(value.at("height"), context + " height");
    if (value.contains("duration"))
        probe.duration = signedValue(value.at("duration"), context + " duration");
    probe.codec = stringField(value, "codec", context);
    probe.colorPrimaries = stringField(value, "colorPrimaries", context);
    probe.colorTransfer = stringField(value, "colorTransfer", context);
    probe.colorMatrix = stringField(value, "colorMatrix", context);
    probe.provenance = stringField(value, "provenance", context);
    if (value.contains("status"))
        probe.status = parseProbeStatus(value.at("status"), context + " status");
    probe.extension = collectUnknownFields(value, {"width", "height", "duration", "codec", "colorPrimaries",
                                                   "colorTransfer", "colorMatrix", "provenance", "status"});
    return probe;
}

nlohmann::json metadataJson(const MediaMetadata& metadata) {
    nlohmann::json value{{"userName", metadata.userName}, {"description", metadata.description},
                         {"tags", metadata.tags},         {"label", metadata.label},
                         {"offline", metadata.offline},   {"kind", mediaKindName(metadata.kind)}};
    if (metadata.committedProbe)
        value["committedProbe"] = probeJson(*metadata.committedProbe);
    applyUnknownFields(value, metadata.extension);
    return value;
}

MediaMetadata parseMetadata(const nlohmann::json& value, const std::string& context) {
    if (!value.is_object())
        throw DeserializeError(context + " must be an object");
    MediaMetadata metadata;
    metadata.userName = stringField(value, "userName", context);
    metadata.description = stringField(value, "description", context);
    metadata.label = stringField(value, "label", context);
    if (value.contains("tags")) {
        if (!value.at("tags").is_array())
            throw DeserializeError(context + ": tags must be an array");
        for (const auto& tag : value.at("tags")) {
            if (!tag.is_string())
                throw DeserializeError(context + ": tags must be strings");
            metadata.tags.push_back(tag.get<std::string>());
        }
    }
    if (value.contains("offline")) {
        if (!value.at("offline").is_boolean())
            throw DeserializeError(context + ": offline must be boolean");
        metadata.offline = value.at("offline").get<bool>();
    }
    if (value.contains("kind"))
        metadata.kind = parseMediaKind(value.at("kind"), context + " kind");
    if (value.contains("committedProbe"))
        metadata.committedProbe = parseProbe(value.at("committedProbe"), context + " committedProbe");
    metadata.extension =
        collectUnknownFields(value, {"userName", "description", "tags", "label", "offline", "kind", "committedProbe"});
    return metadata;
}

nlohmann::json queryJson(const MediaQueryDescriptor& query) {
    nlohmann::json value{{"text", query.text}};
    if (query.kind)
        value["kind"] = mediaKindName(*query.kind);
    if (query.offline)
        value["offline"] = *query.offline;
    if (query.unused)
        value["unused"] = *query.unused;
    applyUnknownFields(value, query.extension);
    return value;
}

std::optional<MediaQueryDescriptor> parseQuery(const nlohmann::json& value, const std::string& context) {
    if (!value.is_object())
        throw DeserializeError(context + " must be an object");
    MediaQueryDescriptor query;
    query.text = stringField(value, "text", context);
    if (value.contains("kind"))
        query.kind = parseMediaKind(value.at("kind"), context + " kind");
    const auto optionalBool = [&](const char* field) -> std::optional<bool> {
        const auto it = value.find(field);
        if (it == value.end())
            return std::nullopt;
        if (!it->is_boolean())
            throw DeserializeError(context + ": '" + field + "' must be boolean");
        return it->get<bool>();
    };
    query.offline = optionalBool("offline");
    query.unused = optionalBool("unused");
    query.extension = collectUnknownFields(value, {"text", "kind", "offline", "unused"});
    return query;
}

MediaMarkRange parseMark(const nlohmann::json& value, const std::string& context) {
    if (!value.is_object())
        throw DeserializeError(context + " must be an object");
    MediaMarkRange mark;
    if (value.contains("inFrame"))
        mark.inFrame = signedValue(value.at("inFrame"), context + " inFrame");
    if (value.contains("outFrame"))
        mark.outFrame = signedValue(value.at("outFrame"), context + " outFrame");
    mark.extension = collectUnknownFields(value, {"inFrame", "outFrame"});
    return mark;
}

nlohmann::json mediaCatalogJson(const MediaCatalog& catalog) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : catalog.entries()) {
        nlohmann::json value{{"id", entry.id},
                             {"sourceKey", entry.sourceKey},
                             {"metadata", metadataJson(entry.metadata)}};
        if (entry.parent != kInvalidMediaBin)
            value["parent"] = entry.parent;
        value["marks"] = nlohmann::json::array();
        for (const auto& mark : entry.marks) {
            nlohmann::json encoded = nlohmann::json::object();
            if (mark.inFrame)
                encoded["inFrame"] = *mark.inFrame;
            if (mark.outFrame)
                encoded["outFrame"] = *mark.outFrame;
            applyUnknownFields(encoded, mark.extension);
            value["marks"].push_back(std::move(encoded));
        }
        applyUnknownFields(value, entry.extension);
        entries.push_back(std::move(value));
    }
    nlohmann::json bins = nlohmann::json::array();
    for (const auto& bin : catalog.bins()) {
        nlohmann::json value{{"id", bin.id}, {"name", bin.name}};
        if (bin.parent != kInvalidMediaBin)
            value["parent"] = bin.parent;
        if (bin.query)
            value["query"] = queryJson(*bin.query);
        applyUnknownFields(value, bin.extension);
        bins.push_back(std::move(value));
    }
    return {{"entries", std::move(entries)},
            {"bins", std::move(bins)},
            {"nextEntryId", catalog.nextEntryId()},
            {"nextBinId", catalog.nextBinId()}};
}

void loadMediaCatalog(const nlohmann::json& value, Document& document) {
    if (!value.is_object())
        throw DeserializeError("document 'mediaCatalog' must be an object");
    auto& catalog = document.mediaCatalog;
    if (value.contains("bins")) {
        const auto& entries = value.at("bins");
        if (!entries.is_array())
            throw DeserializeError("document mediaCatalog 'bins' must be an array");
        struct PendingBin {
            MediaBinId id{kInvalidMediaBin};
            std::string name;
            MediaBinId parent{kInvalidMediaBin};
            std::optional<MediaQueryDescriptor> query;
            nlohmann::json extension;
        };
        std::vector<PendingBin> pending;
        pending.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& b = entries.at(index);
            const std::string context = "mediaCatalog bins[" + std::to_string(index) + "]";
            if (!b.is_object())
                throw DeserializeError(context + " must be an object");
            PendingBin bin;
            bin.id = requiredId(b, "id", context);
            bin.name = stringField(b, "name", context);
            if (bin.name.empty())
                throw DeserializeError(context + ": 'name' must not be empty");
            bin.parent = optionalId(b, "parent", context).value_or(kInvalidMediaBin);
            if (b.contains("query"))
                bin.query = parseQuery(b.at("query"), context + " query");
            bin.extension = collectUnknownFields(b, {"id", "name", "parent", "query"});
            pending.push_back(std::move(bin));
        }
        // Bins may be authored in any order; parents are inserted first because
        // the catalog validates that a parent already exists. A residual entry
        // means the file has a missing or cyclic parent.
        while (!pending.empty()) {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->parent != kInvalidMediaBin && catalog.bin(it->parent) == nullptr) {
                    ++it;
                    continue;
                }
                try {
                    (void)catalog.addBin(it->name, it->parent, std::move(it->query), it->id);
                } catch (const std::exception& error) {
                    throw DeserializeError("mediaCatalog bin " + std::to_string(it->id) + ": " + error.what());
                }
                catalog.bin(it->id)->extension = std::move(it->extension);
                it = pending.erase(it);
                progress = true;
            }
            if (!progress)
                throw DeserializeError("mediaCatalog bins reference a missing or cyclic parent bin");
        }
    }
    if (value.contains("entries")) {
        const auto& entries = value.at("entries");
        if (!entries.is_array())
            throw DeserializeError("document mediaCatalog 'entries' must be an array");
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& e = entries.at(index);
            const std::string context = "mediaCatalog entries[" + std::to_string(index) + "]";
            if (!e.is_object())
                throw DeserializeError(context + " must be an object");
            const auto id = requiredId(e, "id", context);
            const auto sourceKey = stringField(e, "sourceKey", context);
            if (sourceKey.empty())
                throw DeserializeError(context + ": 'sourceKey' must be a nonempty string");
            const auto parent = optionalId(e, "parent", context).value_or(kInvalidMediaBin);
            MediaMetadata metadata;
            if (e.contains("metadata"))
                metadata = parseMetadata(e.at("metadata"), context + " metadata");
            std::vector<MediaMarkRange> marks;
            if (e.contains("marks")) {
                if (!e.at("marks").is_array())
                    throw DeserializeError(context + ": marks must be an array");
                for (std::size_t m = 0; m < e.at("marks").size(); ++m)
                    marks.push_back(parseMark(e.at("marks").at(m), context + " mark " + std::to_string(m)));
            }
            nlohmann::json extension = collectUnknownFields(e, {"id", "sourceKey", "parent", "metadata", "marks"});
            try {
                (void)catalog.addEntry(sourceKey, parent, std::move(metadata), std::move(marks), id);
            } catch (const std::exception& error) {
                throw DeserializeError(context + ": " + error.what());
            }
            catalog.entry(id)->extension = std::move(extension);
        }
    }
    const auto nextEntry =
        value.contains("nextEntryId") ? requiredId(value, "nextEntryId", "mediaCatalog") : catalog.nextEntryId();
    const auto nextBin =
        value.contains("nextBinId") ? requiredId(value, "nextBinId", "mediaCatalog") : catalog.nextBinId();
    try {
        document.restoreMediaIdentityHighWatermarks(nextEntry, nextBin);
    } catch (const std::exception& error) {
        throw DeserializeError("mediaCatalog: " + std::string(error.what()));
    }
}

// ---------------------------------------------------------------------------
// Networks.
// ---------------------------------------------------------------------------

void loadAnimation(const nlohmann::json& json, LoadResult& result, int schema) {
    Document& document = result.document;
    if (schema < 4) {
        if (json.contains("animationChannels") || json.contains("nextAnimationChannelId") ||
            json.contains("nextKeyframeId"))
            throw DeserializeError("animation data requires document schema 4");
        return;
    }
    std::vector<AnimationChannel> channels;
    if (json.contains("animationChannels")) {
        const auto& entries = json.at("animationChannels");
        if (!entries.is_array())
            throw DeserializeError("document animationChannels must be an array");
        channels.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const std::string context = "animationChannels[" + std::to_string(index) + "]";
            try {
                const auto& entry = entries.at(index);
                AnimationChannel channel;
                channel.id = requiredId(entry, "id", context);
                const auto& address = entry.at("address");
                channel.address.network = requiredId(address, "network", context + " address");
                channel.address.node = requiredId(address, "node", context + " address");
                channel.address.key = address.at("key").get<std::string>();
                if (address.contains("instance"))
                    channel.address.instance = requiredId(address, "instance", context + " address");
                const auto& keys = entry.at("keys");
                if (!keys.is_array())
                    throw DeserializeError("keys must be an array");
                channel.keys.reserve(keys.size());
                bool hasOpaqueValue = false;
                for (const auto& value : keys) {
                    Keyframe key;
                    key.id = requiredId(value, "id", context + " key");
                    const auto keyContext = context + " key " + std::to_string(key.id);
                    key.time = animationNumber(value.at("time"), keyContext + " time");
                    const auto& rawValue = value.at("value");
                    if (!rawValue.is_object() || !rawValue.contains("type") || !rawValue.at("type").is_string() ||
                        !rawValue.contains("value"))
                        throw DeserializeError(keyContext +
                                               ": value must be a tagged object containing type and value");
                    // A known tag is parsed strictly (malformed payloads are an
                    // error); an unknown tag is preserved verbatim so authored
                    // animation survives for a node type this build cannot read.
                    if (parameterValueTypeTagKnown(rawValue.at("type").get<std::string>())) {
                        key.value = parameterValueFromJson(rawValue);
                    } else {
                        key.opaqueValue = rawValue;
                        hasOpaqueValue = true;
                    }
                    const auto interpolation = value.at("interpolation").get<std::string>();
                    if (interpolation == "hold")
                        key.interpolation = KeyInterpolation::Hold;
                    else if (interpolation == "linear")
                        key.interpolation = KeyInterpolation::Linear;
                    else if (interpolation == "bezier")
                        key.interpolation = KeyInterpolation::Bezier;
                    else
                        throw DeserializeError(keyContext + ": unknown interpolation '" + interpolation + "'");
                    const auto mode = value.at("tangentMode").get<std::string>();
                    if (mode == "smooth")
                        key.tangentMode = TangentMode::Smooth;
                    else if (mode == "broken")
                        key.tangentMode = TangentMode::Broken;
                    else
                        throw DeserializeError(keyContext + ": unknown tangent mode '" + mode + "'");
                    key.inSlope = animationSlopes(value.at("inSlope"), keyContext + " incoming tangent");
                    key.outSlope = animationSlopes(value.at("outSlope"), keyContext + " outgoing tangent");
                    key.extension = collectUnknownFields(
                        value, {"id", "time", "value", "interpolation", "tangentMode", "inSlope", "outSlope"});
                    channel.keys.push_back(std::move(key));
                }
                channel.extension = collectUnknownFields(entry, {"id", "address", "keys"});
                if (hasOpaqueValue)
                    result.warnings.push_back(context +
                                              " has key values this build cannot interpret; preserved verbatim");
                channels.push_back(std::move(channel));
            } catch (const std::exception& error) {
                throw DeserializeError(context + ": " + error.what());
            }
        }
    }
    const auto nextId = [&](const char* field) {
        if (!json.contains(field))
            return std::uint64_t{1};
        const auto value = unsignedValue(json.at(field), field);
        if (value == 0)
            throw DeserializeError(std::string(field) + " must be nonzero");
        return value;
    };
    try {
        // Restore validates the entire set, including catalog/address/type,
        // identities, collisions and tangents, before installing any channels.
        document.restoreAnimationChannels(std::move(channels), nextId("nextAnimationChannelId"),
                                          nextId("nextKeyframeId"));
    } catch (const std::exception& error) {
        throw DeserializeError("document animation: " + std::string(error.what()));
    }
}

void clearNetwork(Network& network) {
    std::vector<NodeId> nodes;
    for (const auto& node : network.graph().nodes())
        nodes.push_back(node.id);
    for (const auto id : nodes)
        network.graph().removeNode(id);
}

void loadNetwork(const nlohmann::json& entry, Network& network, LoadResult& result, int schema) {
    std::string context = "network '" + network.name() + "'";
    if (!entry.is_object())
        throw DeserializeError(context + " must be an object");
    if (entry.contains("name")) {
        if (!entry.at("name").is_string())
            throw DeserializeError(context + ": 'name' must be a string");
        network.rename(entry.at("name").get<std::string>());
    }
    context = "network '" + network.name() + "'";
    clearNetwork(network);
    if (entry.contains("inputs")) {
        if (!entry.at("inputs").is_array())
            throw DeserializeError(context + ": 'inputs' must be an array");
        for (std::size_t i = 0; i < entry.at("inputs").size(); ++i) {
            const auto& p = entry.at("inputs").at(i);
            if (!p.is_object() || !p.contains("id") || !p.contains("name") || !p.contains("kind") ||
                !p.at("name").is_string())
                throw DeserializeError(context + ": malformed input terminal");
            if (p.contains("allowFanOut") && !p.at("allowFanOut").is_boolean())
                throw DeserializeError(context + ": allowFanOut must be boolean");
            const bool allowFanOut = p.value("allowFanOut", true);
            const auto terminalId =
                network.addInput(p.at("name").get<std::string>(), parseKind(p.at("kind"), context + " input"),
                                 requiredId(p, "id", context + " input"), allowFanOut);
            network.restorePortExtension(PortDirection::Input, terminalId,
                                         collectUnknownFields(p, {"id", "name", "kind", "allowFanOut"}));
        }
    }
    if (entry.contains("outputs")) {
        if (!entry.at("outputs").is_array())
            throw DeserializeError(context + ": 'outputs' must be an array");
        for (std::size_t i = 0; i < entry.at("outputs").size(); ++i) {
            const auto& p = entry.at("outputs").at(i);
            if (!p.is_object() || !p.contains("id") || !p.contains("name") || !p.contains("kind") ||
                !p.at("name").is_string())
                throw DeserializeError(context + ": malformed output terminal");
            if (p.contains("allowFanOut") && !p.at("allowFanOut").is_boolean())
                throw DeserializeError(context + ": allowFanOut must be boolean");
            const bool allowFanOut = p.value("allowFanOut", true);
            const auto terminalId =
                network.addOutput(p.at("name").get<std::string>(), parseKind(p.at("kind"), context + " output"),
                                  requiredId(p, "id", context + " output"), allowFanOut);
            network.restorePortExtension(PortDirection::Output, terminalId,
                                         collectUnknownFields(p, {"id", "name", "kind", "allowFanOut"}));
        }
    }
    const auto nodes = entry.find("nodes");
    if (nodes != entry.end() && !nodes->is_array())
        throw DeserializeError(context + ": 'nodes' must be an array");
    const auto edges = entry.find("edges");
    if (edges != entry.end() && !edges->is_array())
        throw DeserializeError(context + ": 'edges' must be an array");
    std::set<EdgeId> seenEdges;
    std::set<NodeId> seenNodes;
    std::set<NodeId> declaredNodes;
    std::map<NodeId, NodeId> idMap;
    const auto& nodeEntries = nodes == entry.end() ? nlohmann::json::array() : *nodes;
    for (const auto& n : nodeEntries) {
        if (n.is_object() && n.contains("id")) {
            const auto id = requiredId(n, "id", context + " node");
            if (!declaredNodes.insert(id).second)
                throw DeserializeError("duplicate node id in file: " + std::to_string(id));
        }
    }
    for (std::size_t i = 0; i < nodeEntries.size(); ++i) {
        const auto& n = nodeEntries.at(i);
        const std::string nc = context + " node " + std::to_string(i);
        if (!n.is_object() || !n.contains("type") || !n.contains("name") || !n.at("type").is_string() ||
            !n.at("name").is_string())
            throw DeserializeError(nc + ": string 'type' and 'name' are required");
        const std::string type = n.at("type").get<std::string>();
        std::optional<NodeId> persistedId;
        if (n.contains("id"))
            persistedId = requiredId(n, "id", nc);
        NodeId id = persistedId.value_or(1);
        while (!persistedId && (seenNodes.contains(id) || declaredNodes.contains(id))) {
            if (id == std::numeric_limits<NodeId>::max())
                throw DeserializeError(nc + ": no available identity for legacy node");
            ++id;
        }
        if (!seenNodes.insert(id).second)
            throw DeserializeError("duplicate node id in file: " + std::to_string(id));
        ParameterValues params;
        nlohmann::json opaqueParams = nlohmann::json::object();
        if (n.contains("params")) {
            const std::string paramContext = nc + " id " + std::to_string(id) + " parameters";
            if (network.graph().descriptor(type) != nullptr)
                params = parseParameterValues(n.at("params"), schema, type, network.graph().catalog(), paramContext);
            else
                params = parseUnavailableParameterValues(n.at("params"), schema, type, network.graph().catalog(),
                                                         paramContext, opaqueParams);
        }
        LayoutPosition layout;
        if (n.contains("layout"))
            layout = parseLayout(n.at("layout"), nc);
        const auto definition = n.contains("definition") ? requiredId(n, "definition", nc) : kInvalidNetwork;
        const auto instance = n.contains("instance") ? requiredId(n, "instance", nc) : kInvalidNetworkInstance;
        nlohmann::json extension = collectUnknownFields(
            n, {"id", "type", "name", "params", "layout", "definition", "instance", "inputPorts", "outputPorts"});
        const bool hasOpaqueParams = !opaqueParams.empty();
        try {
            (void)network.graph().addNodeWithId(id, type, n.at("name").get<std::string>(), std::move(params), layout,
                                                definition, instance);
            if (n.contains("inputPorts") || n.contains("outputPorts")) {
                if (!n.contains("inputPorts") || !n.contains("outputPorts"))
                    throw DeserializeError(nc + ": both inputPorts and outputPorts are required");
                network.graph().setPortContract(id, parsePorts(n.at("inputPorts"), nc + " inputPorts"),
                                                parsePorts(n.at("outputPorts"), nc + " outputPorts"));
            }
            network.graph().restoreNodeExtension(id, std::move(extension), std::move(opaqueParams));
        } catch (const GraphException& error) {
            throw DeserializeError(nc + ": " + error.what());
        }
        idMap.emplace(persistedId.value_or(id), id);
        if (!persistedId)
            result.warnings.push_back(nc + " has no id; allocated a stable identity");
        if (!network.graph().descriptor(type) && definition == kInvalidNetwork)
            result.warnings.push_back("unknown node type '" + type + "' (node '" + n.at("name").get<std::string>() +
                                      "'); retained as data, not evaluated");
        if (hasOpaqueParams)
            result.warnings.push_back(nc + " has parameters this build cannot interpret; preserved verbatim");
    }
    const auto& edgeEntries = edges == entry.end() ? nlohmann::json::array() : *edges;
    std::set<EdgeId> declaredEdges;
    for (const auto& e : edgeEntries)
        if (e.is_object() && e.contains("id"))
            declaredEdges.insert(requiredId(e, "id", context + " edge"));
    for (std::size_t i = 0; i < edgeEntries.size(); ++i) {
        const auto& e = edgeEntries.at(i);
        const std::string ec = context + " edge " + std::to_string(i);
        if (!e.is_object() || !e.contains("from") || !e.contains("to"))
            throw DeserializeError(ec + ": from and to are required");
        const auto persistedId = optionalId(e, "id", ec);
        EdgeId id = persistedId.value_or(network.graph().nextEdgeId());
        while (!persistedId && (seenEdges.contains(id) || declaredEdges.contains(id))) {
            if (id == std::numeric_limits<EdgeId>::max())
                throw DeserializeError(ec + ": no available identity for legacy edge");
            ++id;
        }
        if (!seenEdges.insert(id).second)
            throw DeserializeError("duplicate edge id in file: " + std::to_string(id));
        if (!persistedId)
            result.warnings.push_back(ec + " has no id; allocated a stable identity");
        const auto& from = e.at("from");
        const auto& to = e.at("to");
        const auto sourceFileId = endpointNode(from, ec + " from");
        const auto destinationFileId = endpointNode(to, ec + " to");
        const auto sourceIt = idMap.find(sourceFileId);
        const auto destinationIt = idMap.find(destinationFileId);
        if (sourceIt == idMap.end() || destinationIt == idMap.end()) {
            result.warnings.push_back(ec + " references a node that is not present in the file; dropped");
            continue;
        }
        const PortRef source{sourceIt->second, port(from, ec + " from")};
        const PortRef destination{destinationIt->second, port(to, ec + " to")};
        const NodeInstance* sourceNode = network.graph().node(source.node);
        const NodeInstance* destinationNode = network.graph().node(destination.node);
        const bool unavailableEndpoint =
            (sourceNode != nullptr && network.graph().descriptor(sourceNode->type) == nullptr) ||
            (destinationNode != nullptr && network.graph().descriptor(destinationNode->type) == nullptr);
        try {
            // A connection authored against an unavailable node type cannot be
            // port-validated here; it is restored exactly instead of dropped.
            if (unavailableEndpoint)
                (void)network.graph().restoreEdgeWithId(id, source, destination);
            else
                (void)network.graph().connectWithId(id, source, destination);
            if (e.contains("route")) {
                if (!e.at("route").is_array())
                    throw DeserializeError(ec + ": route must be an array");
                std::vector<LayoutPosition> route;
                for (std::size_t r = 0; r < e.at("route").size(); ++r)
                    route.push_back(parseLayout(e.at("route").at(r), ec + " route"));
                network.graph().setRoute(id, std::move(route));
            }
            network.graph().restoreEdgeExtension(id, collectUnknownFields(e, {"id", "from", "to", "route"}));
        } catch (const GraphException& error) {
            result.warnings.push_back(ec + " rejected: " + error.what());
        }
    }
    if (entry.contains("inputConnections")) {
        if (!entry.at("inputConnections").is_array())
            throw DeserializeError(context + ": inputConnections must be an array");
        for (const auto& c : entry.at("inputConnections")) {
            if (!c.is_object() || !c.contains("terminal") || !c.contains("node"))
                throw DeserializeError(context + ": malformed input connection");
            const auto t = requiredId(c, "terminal", context + " input connection");
            const auto& node = c.at("node");
            try {
                network.connectInput(t, PortRef{endpointNode(node, context + " input connection"),
                                                port(node, context + " input connection")});
            } catch (const GraphException& error) {
                throw DeserializeError(context + " input connection: " + std::string(error.what()));
            }
        }
    }
    if (entry.contains("outputConnections")) {
        if (!entry.at("outputConnections").is_array())
            throw DeserializeError(context + ": outputConnections must be an array");
        for (const auto& c : entry.at("outputConnections")) {
            if (!c.is_object() || !c.contains("terminal") || !c.contains("node"))
                throw DeserializeError(context + ": malformed output connection");
            const auto t = requiredId(c, "terminal", context + " output connection");
            const auto& node = c.at("node");
            try {
                network.connectOutput(PortRef{endpointNode(node, context + " output connection"),
                                              port(node, context + " output connection")},
                                      t);
            } catch (const GraphException& error) {
                throw DeserializeError(context + " output connection: " + std::string(error.what()));
            }
        }
    }
    if (entry.contains("defaultOutput")) {
        const auto selected = unsignedValue(entry.at("defaultOutput"), context + " defaultOutput");
        if (selected != kInvalidNode) {
            try {
                network.setDefaultOutput(selected);
            } catch (const GraphException& error) {
                throw DeserializeError(context + ": " + std::string(error.what()));
            }
        }
    }
    network.setExtension(
        collectUnknownFields(entry, {"id", "name", "defaultOutput", "nextNodeId", "nextEdgeId", "nextInterfacePortId",
                                     "inputs", "outputs", "nodes", "edges", "inputConnections", "outputConnections"}));
    network.restoreIdentityHighWatermarks(
        watermark(entry, "nextNodeId", "network").value_or(network.graph().nextNodeId()),
        watermark(entry, "nextEdgeId", "network").value_or(network.graph().nextEdgeId()),
        watermark(entry, "nextInterfacePortId", "network").value_or(network.nextInterfacePortId()));
}

// Merges opaque parameter records under typed ones; a later typed edit of the
// same key wins so an authoring change is never overridden by preserved state.
nlohmann::json mergedParameterJson(const ParameterValues& params, const nlohmann::json& opaque) {
    nlohmann::json result = parameterValuesJson(params);
    if (!opaque.is_object())
        return result;
    for (auto it = opaque.begin(); it != opaque.end(); ++it)
        if (!result.contains(it.key()))
            result[it.key()] = it.value();
    return result;
}

}  // namespace

nlohmann::json saveDocument(const Document& document) {
    nlohmann::json networks = nlohmann::json::array();
    for (const auto& network : document.networks()) {
        nlohmann::json nodes = nlohmann::json::array();
        for (const auto& node : network.graph().nodes()) {
            nlohmann::json value{{"id", node.id},
                                 {"type", node.type},
                                 {"name", node.name},
                                 {"params", mergedParameterJson(node.params, node.opaqueParams)},
                                 {"layout", layoutJson(node.layout)}};
            if (node.definition != kInvalidNetwork)
                value["definition"] = node.definition;
            if (node.instance != kInvalidNetworkInstance)
                value["instance"] = node.instance;
            if (node.hasPortContract) {
                value["inputPorts"] = nlohmann::json::array();
                for (const auto& p : node.inputPorts)
                    value["inputPorts"].push_back({{"kind", kindName(p.kind)}, {"name", p.name}});
                value["outputPorts"] = nlohmann::json::array();
                for (const auto& p : node.outputPorts)
                    value["outputPorts"].push_back({{"kind", kindName(p.kind)}, {"name", p.name}});
            }
            applyUnknownFields(value, node.extension);
            nodes.push_back(std::move(value));
        }
        nlohmann::json edges = nlohmann::json::array();
        for (const auto& edge : network.graph().edges()) {
            nlohmann::json value{{"id", edge.id},
                                 {"from", {{"node", edge.from.node}, {"port", edge.from.port}}},
                                 {"to", {{"node", edge.to.node}, {"port", edge.to.port}}}};
            if (!edge.route.empty()) {
                value["route"] = nlohmann::json::array();
                for (const auto& p : edge.route)
                    value["route"].push_back(layoutJson(p));
            }
            applyUnknownFields(value, edge.extension);
            edges.push_back(std::move(value));
        }
        auto formal = [](const std::vector<FormalPort>& ports) {
            nlohmann::json result = nlohmann::json::array();
            for (const auto& p : ports) {
                nlohmann::json value{{"id", p.id},
                                     {"name", p.name},
                                     {"kind", kindName(p.kind)},
                                     {"allowFanOut", p.allowFanOut}};
                applyUnknownFields(value, p.extension);
                result.push_back(std::move(value));
            }
            return result;
        };
        nlohmann::json value{{"id", network.id()},
                             {"name", network.name()},
                             {"defaultOutput", network.defaultOutput()},
                             {"nextNodeId", network.graph().nextNodeId()},
                             {"nextEdgeId", network.graph().nextEdgeId()},
                             {"nextInterfacePortId", network.nextInterfacePortId()},
                             {"inputs", formal(network.inputs())},
                             {"outputs", formal(network.outputs())},
                             {"nodes", nodes},
                             {"edges", edges}};
        value["inputConnections"] = nlohmann::json::array();
        for (const auto& c : network.inputConnections())
            value["inputConnections"].push_back(
                {{"terminal", c.terminal}, {"node", {{"node", c.node.node}, {"port", c.node.port}}}});
        value["outputConnections"] = nlohmann::json::array();
        for (const auto& c : network.outputConnections())
            value["outputConnections"].push_back(
                {{"terminal", c.terminal}, {"node", {{"node", c.node.node}, {"port", c.node.port}}}});
        applyUnknownFields(value, network.extension());
        networks.push_back(std::move(value));
    }
    nlohmann::json sources = nlohmann::json::object();
    for (const auto& [key, source] : document.sources) {
        nlohmann::json value{{"path", source.path},
                             {"frameOffset", source.frameOffset},
                             {"frameStep", source.frameStep},
                             {"revision", source.revision},
                             {"interpretation", source.interpretation}};
        applyUnknownFields(value, source.extension);
        sources[key] = std::move(value);
    }
    nlohmann::json instances = nlohmann::json::array();
    for (const auto& instance : document.instances()) {
        nlohmann::json instanceParams = nlohmann::json::object();
        for (const auto& [target, values] : instance.params)
            instanceParams[std::to_string(target)] = parameterValuesJson(values);
        for (const auto& [target, opaque] : instance.opaqueParams) {
            nlohmann::json& merged = instanceParams[std::to_string(target)];
            if (!merged.is_object())
                merged = nlohmann::json::object();
            for (auto it = opaque.begin(); it != opaque.end(); ++it)
                if (!merged.contains(it.key()))
                    merged[it.key()] = it.value();
        }
        nlohmann::json value{{"id", instance.id},
                             {"parentNetwork", instance.parentNetwork},
                             {"definition", instance.definition},
                             {"node", instance.node},
                             {"name", instance.name},
                             {"params", std::move(instanceParams)}};
        value["inputBindings"] = nlohmann::json::array();
        for (const auto& [terminal, source] : instance.inputBindings)
            value["inputBindings"].push_back(
                {{"terminal", terminal}, {"node", {{"node", source.node}, {"port", source.port}}}});
        applyUnknownFields(value, instance.extension);
        instances.push_back(std::move(value));
    }
    nlohmann::json animation = nlohmann::json::array();
    for (const auto& channel : document.animationChannels()) {
        nlohmann::json address{{"network", channel.address.network},
                               {"node", channel.address.node},
                               {"key", channel.address.key}};
        if (channel.address.instance != kInvalidNetworkInstance)
            address["instance"] = channel.address.instance;
        nlohmann::json keys = nlohmann::json::array();
        for (const auto& key : channel.keys) {
            nlohmann::json encoded{
                {"id", key.id},
                {"time", key.time},
                {"value", key.opaqueValue.is_null() ? parameterValueToJson(key.value) : key.opaqueValue},
                {"interpolation", interpolationName(key.interpolation)},
                {"tangentMode", key.tangentMode == TangentMode::Smooth ? "smooth" : "broken"},
                {"inSlope", key.inSlope},
                {"outSlope", key.outSlope}};
            applyUnknownFields(encoded, key.extension);
            keys.push_back(std::move(encoded));
        }
        nlohmann::json encoded{{"id", channel.id}, {"address", std::move(address)}, {"keys", std::move(keys)}};
        applyUnknownFields(encoded, channel.extension);
        animation.push_back(std::move(encoded));
    }
    nlohmann::json color{{"workingSpace", document.color.workingSpace},
                         {"viewerTransform", document.color.viewerTransform},
                         {"deliveryTransform", document.color.deliveryTransform}};
    applyUnknownFields(color, document.color.extension);
    nlohmann::json result{{"format", std::string(kProjectFormat)},
                          {"schema", Document::kSchemaVersion},
                          {"requiredFeatures", requiredFeaturesJson(document)},
                          {"name", document.name},
                          {"color", std::move(color)},
                          {"sources", std::move(sources)},
                          {"mediaCatalog", mediaCatalogJson(document.mediaCatalog)},
                          {"rootNetworkId", document.rootNetworkId()},
                          {"nextNetworkId", document.nextNetworkId()},
                          {"nextInstanceId", document.nextInstanceId()},
                          {"networks", std::move(networks)},
                          {"instances", std::move(instances)},
                          {"animationChannels", std::move(animation)},
                          {"nextAnimationChannelId", document.nextAnimationChannelId()},
                          {"nextKeyframeId", document.nextKeyframeId()}};
    applyUnknownFields(result, document.extension);
    return result;
}

LoadResult loadDocument(const nlohmann::json& json, std::shared_ptr<const NodeCatalog> catalog) {
    if (!json.is_object())
        throw DeserializeError("document root is not an object");
    if (json.contains("format")) {
        if (!json.at("format").is_string())
            throw DeserializeError("document 'format' must be a string");
        const auto format = json.at("format").get<std::string>();
        if (format != kProjectFormat)
            throw DeserializeError("unsupported project format '" + format + "'; this build reads '" +
                                   std::string(kProjectFormat) + "'");
    }
    if (!json.contains("schema") || !json.at("schema").is_number_integer())
        throw DeserializeError("document has no integer 'schema' field");
    const int schema = json.at("schema").get<int>();
    if (schema > Document::kSchemaVersion)
        throw DeserializeError("document schema " + std::to_string(schema) + " is newer than this build supports (" +
                               std::to_string(Document::kSchemaVersion) + ")");
    checkRequiredFeatures(json);
    LoadResult result{Document(std::move(catalog)), {}};
    // "presentation" belongs to the session/file envelope; the codec neither
    // reads, preserves nor writes it.
    result.document.extension = collectUnknownFields(json, {"format",
                                                            "schema",
                                                            "requiredFeatures",
                                                            "presentation",
                                                            "name",
                                                            "color",
                                                            "sources",
                                                            "mediaCatalog",
                                                            "rootNetworkId",
                                                            "nextNetworkId",
                                                            "nextInstanceId",
                                                            "networks",
                                                            "instances",
                                                            "animationChannels",
                                                            "nextAnimationChannelId",
                                                            "nextKeyframeId",
                                                            "nodes",
                                                            "edges",
                                                            "nextNodeId",
                                                            "nextEdgeId"});
    result.document.name = json.value("name", std::string{});
    if (auto color = json.find("color"); color != json.end()) {
        if (!color->is_object()) {
            result.warnings.push_back("document 'color' field is not an object; using default color policy");
        } else {
            result.document.color.workingSpace = color->value("workingSpace", result.document.color.workingSpace);
            result.document.color.viewerTransform =
                color->value("viewerTransform", result.document.color.viewerTransform);
            result.document.color.deliveryTransform =
                color->value("deliveryTransform", result.document.color.deliveryTransform);
            result.document.color.extension =
                collectUnknownFields(*color, {"workingSpace", "viewerTransform", "deliveryTransform"});
        }
    }
    if (auto sources = json.find("sources"); sources != json.end()) {
        if (!sources->is_object())
            throw DeserializeError("document 'sources' field must be an object");
        for (auto it = sources->begin(); it != sources->end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("path") || !it.value().at("path").is_string())
                throw DeserializeError("malformed source '" + it.key() + "'");
            SourceReference source;
            const auto& e = it.value();
            source.path = e.at("path").get<std::string>();
            if (source.path.empty())
                throw DeserializeError("source '" + it.key() + "' has an empty path");
            if (e.contains("frameOffset"))
                source.frameOffset = signedValue(e.at("frameOffset"), "source frameOffset");
            if (e.contains("frameStep"))
                source.frameStep = signedValue(e.at("frameStep"), "source frameStep");
            if (e.contains("revision"))
                source.revision = unsignedValue(e.at("revision"), "source revision");
            if (source.frameStep == 0)
                throw DeserializeError("source '" + it.key() + "': frameStep must not be zero");
            if (e.contains("interpretation")) {
                if (!e.at("interpretation").is_object())
                    throw DeserializeError("source interpretation must be an object");
                for (auto tag = e.at("interpretation").begin(); tag != e.at("interpretation").end(); ++tag) {
                    if (!tag.value().is_string())
                        throw DeserializeError("source interpretation values must be strings");
                    source.interpretation[tag.key()] = tag.value().get<std::string>();
                }
            }
            source.extension =
                collectUnknownFields(e, {"path", "frameOffset", "frameStep", "revision", "interpretation"});
            result.document.sources[it.key()] = std::move(source);
        }
    }
    if (auto mediaCatalog = json.find("mediaCatalog"); mediaCatalog != json.end())
        loadMediaCatalog(*mediaCatalog, result.document);
    if (json.contains("networks")) {
        const auto& entries = json.at("networks");
        if (!entries.is_array() || entries.empty())
            throw DeserializeError("document 'networks' must be a nonempty array");
        const auto rootId = requiredId(json, "rootNetworkId", "document");
        const auto initialRoot = result.document.rootNetworkId();
        std::set<NetworkId> seen;
        std::set<std::string> names;
        for (const auto& entry : entries) {
            const auto id = requiredId(entry, "id", "network");
            if (!seen.insert(id).second)
                throw DeserializeError("duplicate network id in file: " + std::to_string(id));
            if (!entry.contains("name") || !entry.at("name").is_string())
                throw DeserializeError("network name must be a string");
            if (!names.insert(entry.at("name").get<std::string>()).second)
                throw DeserializeError("duplicate network name in file");
        }
        if (!seen.contains(rootId))
            throw DeserializeError("document networks do not contain root " + std::to_string(rootId));
        std::string temporaryName = "__loading_root__";
        while (names.contains(temporaryName))
            temporaryName += '_';
        result.document.network(initialRoot).rename(std::move(temporaryName));
        for (const auto& entry : entries) {
            const auto id = requiredId(entry, "id", "network");
            if (id != initialRoot)
                (void)result.document.addNetworkWithId(id, entry.at("name").get<std::string>());
            loadNetwork(entry, result.document.network(id), result, schema);
        }
        result.document.setRootNetworkId(rootId);
        if (!seen.contains(initialRoot))
            result.document.removeNetwork(initialRoot);
    } else if (schema < 2) {
        // Schema 1 used the root graph directly. Migrate it into the default root network.
        nlohmann::json legacy{{"name", "Root"},
                              {"nodes", json.value("nodes", nlohmann::json::array())},
                              {"edges", json.value("edges", nlohmann::json::array())}};
        if (json.contains("nextNodeId"))
            legacy["nextNodeId"] = json.at("nextNodeId");
        if (json.contains("nextEdgeId"))
            legacy["nextEdgeId"] = json.at("nextEdgeId");
        loadNetwork(legacy, result.document.network(result.document.rootNetworkId()), result, schema);
        for (const auto& node : result.document.network(result.document.rootNetworkId()).graph().nodes()) {
            const auto* descriptor =
                result.document.network(result.document.rootNetworkId()).graph().descriptor(node.type);
            if (descriptor && descriptor->isOutput) {
                result.document.network(result.document.rootNetworkId()).setDefaultOutput(node.id);
                break;
            }
        }
        result.warnings.push_back("legacy single-graph document migrated to the root network");
    } else {
        throw DeserializeError("schema " + std::to_string(schema) + " document has no 'networks' field");
    }
    if (json.contains("instances")) {
        if (!json.at("instances").is_array())
            throw DeserializeError("document 'instances' field must be an array");
        for (std::size_t i = 0; i < json.at("instances").size(); ++i) {
            const auto& e = json.at("instances").at(i);
            const std::string context = "instance " + std::to_string(i);
            const auto id = requiredId(e, "id", context);
            const auto parent = requiredId(e, "parentNetwork", context);
            const auto definition = requiredId(e, "definition", context);
            const auto node = requiredId(e, "node", context);
            if (!e.contains("name") || !e.at("name").is_string())
                throw DeserializeError(context + ": name is required");
            std::map<InterfacePortId, PortRef> bindings;
            if (e.contains("inputBindings")) {
                if (!e.at("inputBindings").is_array())
                    throw DeserializeError(context + ": inputBindings must be an array");
                for (const auto& b : e.at("inputBindings")) {
                    const auto terminal = requiredId(b, "terminal", context);
                    const auto& ref = b.at("node");
                    bindings.emplace(terminal, PortRef{endpointNode(ref, context), port(ref, context)});
                }
            }
            std::map<NodeId, ParameterValues> params;
            std::map<NodeId, nlohmann::json> opaqueParams;
            if (e.contains("params")) {
                if (!e.at("params").is_object())
                    throw DeserializeError(context + ": params must be an object");
                for (auto p = e.at("params").begin(); p != e.at("params").end(); ++p) {
                    NodeId target{};
                    try {
                        std::size_t consumed = 0;
                        target = static_cast<NodeId>(std::stoull(p.key(), &consumed));
                        if (consumed != p.key().size())
                            throw std::invalid_argument("not a node id");
                    } catch (...) {
                        throw DeserializeError(context + ": parameter target must be a node id");
                    }
                    if (target == kInvalidNode || target == std::numeric_limits<NodeId>::max() ||
                        !p.value().is_object())
                        throw DeserializeError(context + " node " + std::to_string(target) +
                                               ": malformed parameter target");
                    const NodeInstance* targetNode = nullptr;
                    try {
                        targetNode = result.document.network(definition).graph().node(target);
                    } catch (const std::exception&) {
                    }
                    const std::string type = targetNode == nullptr ? std::string{} : targetNode->type;
                    const auto& targetCatalog =
                        result.document.network(result.document.rootNetworkId()).graph().catalog();
                    const std::string paramContext =
                        context + " id " + std::to_string(id) + " node " + std::to_string(target) + " parameters";
                    const bool unavailable =
                        targetNode != nullptr &&
                        result.document.network(definition).graph().descriptor(targetNode->type) == nullptr;
                    if (unavailable) {
                        nlohmann::json opaque = nlohmann::json::object();
                        params.emplace(target, parseUnavailableParameterValues(p.value(), schema, type, targetCatalog,
                                                                               paramContext, opaque));
                        if (!opaque.empty())
                            opaqueParams.emplace(target, std::move(opaque));
                    } else {
                        params.emplace(target,
                                       parseParameterValues(p.value(), schema, type, targetCatalog, paramContext));
                    }
                }
            }
            nlohmann::json extension = collectUnknownFields(
                e, {"id", "parentNetwork", "definition", "node", "name", "params", "inputBindings"});
            const bool hasOpaqueParams = !opaqueParams.empty();
            try {
                (void)result.document.addInstanceWithId(id, parent, definition, node, e.at("name").get<std::string>(),
                                                        std::move(bindings), std::move(params));
                result.document.restoreInstanceExtension(id, std::move(extension), std::move(opaqueParams));
            } catch (const std::exception& error) {
                throw DeserializeError(context + ": " + error.what());
            }
            if (hasOpaqueParams)
                result.warnings.push_back(context + " has parameters this build cannot interpret; preserved verbatim");
        }
    }
    for (const auto& network : result.document.networks()) {
        for (const auto& node : network.graph().nodes()) {
            if (node.definition == kInvalidNetwork)
                continue;
            const auto* occurrence = result.document.instance(node.instance);
            if (!occurrence || occurrence->parentNetwork != network.id() || occurrence->definition != node.definition ||
                occurrence->node != node.id)
                throw DeserializeError("network " + std::to_string(network.id()) + " node " + std::to_string(node.id) +
                                       " references an invalid instance " + std::to_string(node.instance));
        }
    }
    loadAnimation(json, result, schema);
    result.document.synchronizeReferences();
    result.document.restoreIdentityHighWatermarks(
        watermark(json, "nextNetworkId", "document").value_or(result.document.nextNetworkId()),
        watermark(json, "nextInstanceId", "document").value_or(result.document.nextInstanceId()));
    return result;
}

}  // namespace nemo
