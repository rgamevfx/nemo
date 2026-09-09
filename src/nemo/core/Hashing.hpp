#pragma once

// FNV-1a 64 content hashing helpers. One convention for the repo: content
// addressing (image identity, reuse keys, library fingerprints) and
// freshness tokens all mix bytes with the same primitive.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nemo {

inline constexpr std::uint64_t kFnv1a64Basis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

inline void hashMix(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnv1a64Prime;
    }
}

// Field terminator: 0x1F cannot appear in decimal numbers and is length-
// prefixed out of the way for arbitrary strings.
inline constexpr unsigned char kHashFieldSeparator = 0x1F;

// Canonical field: label:byte-count:value<sep>. Labels are fixed schema
// names; values may contain arbitrary bytes without aliasing other fields.
inline void appendCanonicalField(std::string& output, std::string_view label, std::string_view value) {
    output += label;
    output.push_back(':');
    output += std::to_string(value.size());
    output.push_back(':');
    output += value;
    output.push_back(static_cast<char>(kHashFieldSeparator));
}

// Text plus an explicit separator, so distinct field sequences cannot alias.
inline void hashMixText(std::uint64_t& hash, const std::string& text) {
    hashMix(hash, text.data(), text.size());
    hashMix(hash, &kHashFieldSeparator, 1);
}

// 64-bit word, 8 little-endian bytes.
inline void hashMixWord(std::uint64_t& hash, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        const unsigned char byte = static_cast<unsigned char>(value & 0xFFU);
        hashMix(hash, &byte, 1);
        value >>= 8;
    }
}

}  // namespace nemo
