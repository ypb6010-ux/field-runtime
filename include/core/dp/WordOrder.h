#pragma once

#include <array>
#include <cstdint>

#include "core/core_global.h"

namespace core::dp {

// Byte/word layout for multi-register integers and floats.
//
// Using 0x12345678 as a 32-bit example, occupying two Modbus registers
// (reg[N], reg[N+1]):
//
//   WordOrder      reg[N]   reg[N+1]   bytes-on-wire    typical vendor
//   ─────────────  ───────  ─────────  ──────────────   ─────────────────
//   ABCD (BE)      0x1234   0x5678     12 34 56 78      Modbus standard
//   CDAB (WS_BE)   0x5678   0x1234     56 78 12 34      Schneider / ABB
//   BADC (BS_BE)   0x3412   0x7856     34 12 78 56      uncommon
//   DCBA (LE)      0x7856   0x3412     78 56 34 12      Modicon Quantum
enum class WordOrder {
    ABCD,
    CDAB,
    BADC,
    DCBA,
};

CORE_EXPORT const char* wordOrderName(WordOrder w) noexcept;

// Byte permutation table for an 8-byte buffer. Length 4 used for 32-bit
// values, length 8 for 64-bit. Index `permutation[i]` gives the source byte
// (in the network-order [A][B][C][D]... layout) that should end up at
// position `i` in the host-order value.
struct BytePermutation {
    std::array<std::uint8_t, 8> order;
};

CORE_EXPORT BytePermutation permutationFor(WordOrder w, int byteCount) noexcept;

} // namespace core::dp
