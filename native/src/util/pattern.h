// Byte-pattern scanning for locating engine functions without hard-coded
// offsets. Patterns use IDA style: "48 8B 05 ?? ?? ?? ?? E8 ?" where ?/?? is a
// wildcard byte.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace overseer::pattern {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;  // true = byte must match, false = wildcard

    size_t size() const { return bytes.size(); }
};

// Returns nullopt for empty or malformed patterns.
std::optional<Pattern> Parse(std::string_view text);

// First match in `haystack`, or nullptr.
const uint8_t* Find(std::span<const uint8_t> haystack, const Pattern& pattern);

// All non-overlapping matches, up to `limit`.
std::vector<const uint8_t*> FindAll(std::span<const uint8_t> haystack, const Pattern& pattern,
                                    size_t limit = 64);

// Resolves an x64 RIP-relative operand: the target is the address of the next
// instruction plus the signed 32-bit displacement found at `dispOffset`.
// E.g. `E8 xx xx xx xx` (call) -> ResolveRelative(p, 1, 5);
//      `48 8B 05 xx xx xx xx` (mov rax,[rip+x]) -> ResolveRelative(p, 3, 7).
const uint8_t* ResolveRelative(const uint8_t* instruction, size_t dispOffset,
                               size_t instructionLength);

}  // namespace overseer::pattern
