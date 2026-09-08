#include "nemo/core/evaluation/Reuse.hpp"

#include <algorithm>
#include <map>

namespace nemo {

namespace {

// FNV-1a 64 over a byte stream; hashBytes/hashFinish mirror the CPU image
// identity helpers (content addressing stays one convention in the repo).
void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
}

void hashText(std::uint64_t& hash, const std::string& text) {
    hashBytes(hash, text.data(), text.size());
    const unsigned char separator = 0x1F;
    hashBytes(hash, &separator, 1);
}

[[nodiscard]] std::string canonicalParams(const Node& node) {
    // Node::params is a std::map, so iteration is already canonical.
    std::string canonical;
    for (const auto& [key, value] : node.params) {
        canonical += key;
        canonical.push_back('=');
        canonical += value;
        canonical.push_back('\x1E');  // record separator within the params field
    }
    return canonical;
}

}  // namespace

std::uint64_t implementationVersion(const std::string& nodeType) {
    // Bump a type's version when its evaluation semantics change in a way
    // keys must observe (see Reuse.hpp).
    static const std::map<std::string, std::uint64_t> versions{{"testpattern", 1},
                                                               {"constcolor", 1},
                                                               {"merge", 1},
                                                               {"output", 1}};
    const auto it = versions.find(nodeType);
    return it != versions.end() ? it->second : 1;
}

ResultKey nodeResultKey(const Document& document, const Node& node, const std::vector<std::uint64_t>& inputKeyHashes,
                        const EvaluationRequest& request, const KeyContext& context) {
    // Canonical form (field separators cannot appear in names/values):
    //   impl|type|params|inputs|time|region|channels|quality|working|tag
    // Input identity enters through the inputs' key hashes in port order,
    // so a change anywhere upstream changes every downstream key while
    // unrelated branches keep theirs (spec section 10.3: reuse follows
    // effective dependencies).
    std::string canonical;
    canonical.reserve(64 + node.params.size() * 16);
    canonical += "impl:" + std::to_string(implementationVersion(node.type));
    canonical.push_back('\x1F');
    canonical += "type:" + node.type;
    canonical.push_back('\x1F');
    canonical += "params:" + canonicalParams(node);
    canonical.push_back('\x1F');
    canonical += "inputs:";
    for (const std::uint64_t inputHash : inputKeyHashes) {
        canonical += std::to_string(inputHash);
        canonical.push_back(',');
    }
    canonical.push_back('\x1F');
    canonical += "time:" + std::to_string(request.localTime);
    canonical.push_back('\x1F');
    canonical += "region:" + std::to_string(request.region.x) + ',' + std::to_string(request.region.y) + ',' +
                 std::to_string(request.region.width) + ',' + std::to_string(request.region.height);
    canonical.push_back('\x1F');
    canonical += "channels:" + request.channels;
    canonical.push_back('\x1F');
    canonical += "quality:" + std::string(qualityName(request.quality));
    canonical.push_back('\x1F');
    canonical += "working:" + document.color.workingSpace;
    canonical.push_back('\x1F');
    canonical += "tag:" + std::to_string(context.implementationTag);

    ResultKey key;
    key.canonical = std::move(canonical);
    key.hash = 14695981039346656037ULL;
    hashText(key.hash, key.canonical);
    return key;
}

ResultKey viewerResultKey(const ResultKey& sceneLinearKey, const ColorPolicy& policy) {
    // Viewer representations bake the viewing transform (and delivery
    // interpretation for delivery outputs); upstream scene-linear results
    // must stay valid when only viewing state changes (ADR-0004).
    const std::string canonical = sceneLinearKey.canonical + "\x1F" "view:" + policy.viewerTransform +
                                  "\x1F" "delivery:" + policy.deliveryTransform;
    ResultKey key;
    key.canonical = canonical;
    key.hash = 14695981039346656037ULL;
    hashText(key.hash, key.canonical);
    return key;
}

}  // namespace nemo
