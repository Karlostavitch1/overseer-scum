#include "util/pattern.h"

#include <cstring>

namespace overseer::pattern {
namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Matches(const uint8_t* at, const Pattern& pattern) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern.mask[i] && at[i] != pattern.bytes[i]) return false;
    }
    return true;
}

}  // namespace

std::optional<Pattern> Parse(std::string_view text) {
    Pattern pattern;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '?') {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(false);
            ++i;
            if (i < text.size() && text[i] == '?') ++i;
        } else {
            if (i + 1 >= text.size()) return std::nullopt;
            int hi = HexValue(text[i]);
            int lo = HexValue(text[i + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            pattern.bytes.push_back(static_cast<uint8_t>(hi << 4 | lo));
            pattern.mask.push_back(true);
            i += 2;
        }
        // Tokens must be separated by whitespace.
        if (i < text.size() && text[i] != ' ' && text[i] != '\t') return std::nullopt;
    }
    if (pattern.bytes.empty()) return std::nullopt;
    // A pattern of only wildcards would match everywhere — reject it.
    bool any = false;
    for (bool m : pattern.mask) any = any || m;
    if (!any) return std::nullopt;
    return pattern;
}

const uint8_t* Find(std::span<const uint8_t> haystack, const Pattern& pattern) {
    if (pattern.size() == 0 || haystack.size() < pattern.size()) return nullptr;

    // Anchor on the first concrete byte and let memchr do the heavy lifting.
    size_t anchor = 0;
    while (!pattern.mask[anchor]) ++anchor;
    const uint8_t first = pattern.bytes[anchor];

    const uint8_t* begin = haystack.data();
    const uint8_t* last = begin + (haystack.size() - pattern.size());
    const uint8_t* p = begin;
    while (p <= last) {
        auto* hit = static_cast<const uint8_t*>(
            std::memchr(p + anchor, first, static_cast<size_t>(last - p) + 1));
        if (!hit) return nullptr;
        const uint8_t* candidate = hit - anchor;
        if (Matches(candidate, pattern)) return candidate;
        p = candidate + 1;
    }
    return nullptr;
}

std::vector<const uint8_t*> FindAll(std::span<const uint8_t> haystack, const Pattern& pattern,
                                    size_t limit) {
    std::vector<const uint8_t*> results;
    size_t offset = 0;
    while (results.size() < limit && offset < haystack.size()) {
        const uint8_t* hit = Find(haystack.subspan(offset), pattern);
        if (!hit) break;
        results.push_back(hit);
        offset = static_cast<size_t>(hit - haystack.data()) + pattern.size();
    }
    return results;
}

const uint8_t* ResolveRelative(const uint8_t* instruction, size_t dispOffset,
                               size_t instructionLength) {
    int32_t displacement;
    std::memcpy(&displacement, instruction + dispOffset, sizeof(displacement));
    return instruction + instructionLength + displacement;
}

}  // namespace overseer::pattern
