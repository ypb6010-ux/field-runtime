// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "core/codec/BuiltinCodecs.h"
#include "core/codec/CodecRegistry.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/dp/Value.h"
#include "core/dp/WordOrder.h"

using namespace core::codec;
using namespace core::dp;
using Catch::Matchers::WithinAbs;

namespace {

PortRef portFor(WordOrder wo               = WordOrder::ABCD,
                std::optional<int> bit     = std::nullopt,
                int       shift            = 0,
                std::uint64_t mask         = 0xFFFFFFFFFFFFFFFFull,
                double    scale            = 1.0,
                double    offset           = 0.0) {
    PortRef r;
    r.wordOrder = wo;
    r.bit       = bit;
    r.shift     = shift;
    r.mask      = mask;
    r.scale     = scale;
    r.offset    = offset;
    return r;
}

// Build a dp::Value from a typed literal, picking the exact variant alternative
// (a bare int / uint16_t would be an ambiguous variant construction).
template <class T>
Value V(T x) {
    if constexpr (std::is_same_v<T, bool>)          return x;
    else if constexpr (std::is_floating_point_v<T>) return double(x);
    else if constexpr (std::is_signed_v<T>)         return std::int64_t(x);
    else                                            return std::uint64_t(x);
}
inline Value V(char const* s) { return std::string(s); }
inline Value V(std::string const& s) { return s; }

} // namespace

// ===========================================================================
// 16-bit
// ===========================================================================

TEST_CASE("U16 round-trips raw register values", "[codec][u16]") {
    BuiltinScalarCodec c(ScalarType::U16);
    auto ref = portFor();

    auto regs = c.encode(V(std::uint16_t(0xABCD)), ref);
    REQUIRE(regs == core::RegisterWords{0xABCD});

    auto v = c.decode(regs, ref);
    REQUIRE(toUInt64(v) == 0xABCD);
}

TEST_CASE("S16 sign-extends negative values", "[codec][s16]") {
    BuiltinScalarCodec c(ScalarType::S16);
    auto ref = portFor();

    auto regs = c.encode(V(std::int16_t(-2)), ref);
    REQUIRE(regs == core::RegisterWords{0xFFFE});

    auto v = c.decode(core::RegisterWords{0xFFFE}, ref);
    REQUIRE(toInt64(v) == -2);
}

TEST_CASE("S16 applies scale and offset linear transform", "[codec][s16][scale]") {
    BuiltinScalarCodec c(ScalarType::S16);
    auto ref = portFor(WordOrder::ABCD, std::nullopt, 0, 0xFFFFu, 0.1, -40.0);

    // raw 600 → 600 * 0.1 + (-40) = 20.0
    auto v = c.decode(core::RegisterWords{600}, ref);
    REQUIRE_THAT(toDouble(v), WithinAbs(20.0, 1e-9));

    // 20.0 → (20 - (-40)) / 0.1 = 600
    auto regs = c.encode(V(20.0), ref);
    REQUIRE(regs == core::RegisterWords{600});
}

TEST_CASE("U16 shift+mask isolates bit field", "[codec][u16][bits]") {
    BuiltinScalarCodec c(ScalarType::U16);
    auto ref = portFor(WordOrder::ABCD, std::nullopt,
                       /*shift*/4, /*mask*/0x000Fu);

    // raw 0x00F0 → (0x00F0 >> 4) & 0x0F = 0x0F
    auto v = c.decode(core::RegisterWords{0x00F0}, ref);
    REQUIRE(toUInt64(v) == 0x0F);

    // 0x0F → (0x0F & 0x0F) << 4 = 0x00F0
    auto regs = c.encode(V(std::uint16_t(0x0F)), ref);
    REQUIRE(regs == core::RegisterWords{0x00F0});
}

// ===========================================================================
// 32-bit and word order
// ===========================================================================

TEST_CASE("U32 ABCD round-trips a known constant", "[codec][u32][abcd]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::ABCD);

    auto regs = c.encode(V(std::uint32_t(0x12345678u)), ref);
    REQUIRE(regs == core::RegisterWords{0x1234, 0x5678});

    auto v = c.decode(regs, ref);
    REQUIRE(toUInt64(v) == 0x12345678u);
}

TEST_CASE("U32 CDAB swaps the word pair", "[codec][u32][cdab]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(V(std::uint32_t(0x12345678u)), ref);
    REQUIRE(regs == core::RegisterWords{0x5678, 0x1234});

    auto v = c.decode(regs, ref);
    REQUIRE(toUInt64(v) == 0x12345678u);
}

TEST_CASE("U32 BADC swaps bytes within each word", "[codec][u32][badc]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::BADC);

    auto regs = c.encode(V(std::uint32_t(0x12345678u)), ref);
    REQUIRE(regs == core::RegisterWords{0x3412, 0x7856});

    auto v = c.decode(regs, ref);
    REQUIRE(toUInt64(v) == 0x12345678u);
}

TEST_CASE("U32 DCBA fully reverses bytes", "[codec][u32][dcba]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::DCBA);

    auto regs = c.encode(V(std::uint32_t(0x12345678u)), ref);
    REQUIRE(regs == core::RegisterWords{0x7856, 0x3412});

    auto v = c.decode(regs, ref);
    REQUIRE(toUInt64(v) == 0x12345678u);
}

TEST_CASE("S32 sign-extends after permutation", "[codec][s32]") {
    BuiltinScalarCodec c(ScalarType::S32);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(V(std::int32_t(-1)), ref);
    auto v    = c.decode(regs, ref);
    REQUIRE(toInt64(v) == -1);
}

// ===========================================================================
// IEEE float
// ===========================================================================

TEST_CASE("F32 ABCD round-trips with IEEE bit pattern", "[codec][f32]") {
    BuiltinScalarCodec c(ScalarType::F32);
    auto ref = portFor(WordOrder::ABCD);

    // 23.5 → 0x41BC0000 IEEE-754
    auto regs = c.encode(V(23.5), ref);
    REQUIRE(regs.size() == 2);
    REQUIRE(regs[0] == 0x41BC);
    REQUIRE(regs[1] == 0x0000);

    auto v = c.decode(regs, ref);
    REQUIRE_THAT(toDouble(v), WithinAbs(23.5, 1e-6));
}

TEST_CASE("F32 CDAB places the high word second", "[codec][f32][cdab]") {
    BuiltinScalarCodec c(ScalarType::F32);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(V(23.5), ref);
    REQUIRE(regs == core::RegisterWords{0x0000, 0x41BC});

    auto v = c.decode(regs, ref);
    REQUIRE_THAT(toDouble(v), WithinAbs(23.5, 1e-6));
}

TEST_CASE("F32 with scale handles milli-unit physical sensors", "[codec][f32][scale]") {
    BuiltinScalarCodec c(ScalarType::F32);
    auto ref = portFor(WordOrder::CDAB, std::nullopt, 0, 0xFFFFFFFFu,
                       /*scale*/0.001, /*offset*/0.0);

    auto raw  = c.encode(V(7.234), ref);   // expressed as milli-unit on the wire
    auto back = c.decode(raw,  ref);
    REQUIRE_THAT(toDouble(back), WithinAbs(7.234, 1e-4));
}

// ===========================================================================
// 64-bit
// ===========================================================================

TEST_CASE("F64 round-trips with ABCD ordering", "[codec][f64]") {
    BuiltinScalarCodec c(ScalarType::F64);
    auto ref = portFor(WordOrder::ABCD);

    auto regs = c.encode(V(3.141592653589793), ref);
    REQUIRE(regs.size() == 4);

    auto v = c.decode(regs, ref);
    REQUIRE_THAT(toDouble(v), WithinAbs(3.141592653589793, 1e-12));
}

TEST_CASE("U64 round-trips with CDAB ordering", "[codec][u64]") {
    BuiltinScalarCodec c(ScalarType::U64);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(V(std::uint64_t(0x0123456789ABCDEFull)), ref);
    auto v    = c.decode(regs, ref);
    REQUIRE(toUInt64(v) == 0x0123456789ABCDEFull);
}

// ===========================================================================
// Bool
// ===========================================================================

TEST_CASE("Bool encodes only the requested bit", "[codec][bool]") {
    BuiltinScalarCodec c(ScalarType::Bool);
    auto ref = portFor(WordOrder::ABCD, /*bit*/3);

    auto regs = c.encode(V(true), ref);
    REQUIRE(regs == core::RegisterWords{0x0008});

    auto v = c.decode(core::RegisterWords{0x0008}, ref);
    REQUIRE(toBool(v));

    auto v0 = c.decode(core::RegisterWords{0x0000}, ref);
    REQUIRE_FALSE(toBool(v0));
}

TEST_CASE("Bool decodes from a register with multiple bits set",
          "[codec][bool]") {
    BuiltinScalarCodec c(ScalarType::Bool);
    auto refB3 = portFor(WordOrder::ABCD, /*bit*/3);
    auto refB7 = portFor(WordOrder::ABCD, /*bit*/7);

    auto src = core::RegisterWords{0x0088};   // bits 3 and 7 set
    REQUIRE(toBool(c.decode(src, refB3)));
    REQUIRE(toBool(c.decode(src, refB7)));
}

// ===========================================================================
// EnumU16Codec
// ===========================================================================

TEST_CASE("EnumU16Codec maps raw values to names", "[codec][enum]") {
    EnumU16Codec c("belt_state", {
        {0, "Stopped"},
        {1, "Starting"},
        {2, "Running"},
        {4, "Fault"},
    });
    auto ref = portFor();

    REQUIRE(toString(c.decode(core::RegisterWords{0}, ref)) == "Stopped");
    REQUIRE(toString(c.decode(core::RegisterWords{2}, ref)) == "Running");
    REQUIRE(toString(c.decode(core::RegisterWords{4}, ref)) == "Fault");

    // Unknown keys produce a stable placeholder, never empty.
    REQUIRE(toString(c.decode(core::RegisterWords{99}, ref)) == "Unknown(99)");
}

TEST_CASE("EnumU16Codec encodes by name and by numeric fallback",
          "[codec][enum]") {
    EnumU16Codec c("belt_state", {
        {0, "Stopped"},
        {2, "Running"},
    });
    auto ref = portFor();

    REQUIRE(c.encode(V("Running"), ref) == core::RegisterWords{2});
    REQUIRE(c.encode(V("Stopped"), ref) == core::RegisterWords{0});

    // Unknown name → fallback to numeric coercion of the value
    REQUIRE(c.encode(V(7), ref) == core::RegisterWords{7});
}

TEST_CASE("EnumU16Codec masks before lookup", "[codec][enum][mask]") {
    EnumU16Codec c("low_nibble", {
        {0, "Idle"},
        {1, "Run"},
    });
    auto ref = portFor(WordOrder::ABCD, std::nullopt, 0, 0x000Fu);

    REQUIRE(toString(c.decode(core::RegisterWords{0xFFF0}, ref)) == "Idle");
    REQUIRE(toString(c.decode(core::RegisterWords{0x00F1}, ref)) == "Run");
}

// ===========================================================================
// CodecRegistry
// ===========================================================================

TEST_CASE("CodecRegistry stores and retrieves codecs", "[codec][registry]") {
    CodecRegistry reg;
    auto codec = std::make_shared<BuiltinScalarCodec>(ScalarType::U16);
    reg.registerCodec(codec);

    auto found = reg.find(BuiltinScalarCodec::idFor(ScalarType::U16));
    REQUIRE(found.get() == codec.get());

    REQUIRE(reg.find("nope") == nullptr);
}

TEST_CASE("CodecRegistry.loadBuiltins registers all scalar types",
          "[codec][registry]") {
    CodecRegistry reg;
    reg.loadBuiltins();

    for (auto t : {
        ScalarType::Bool, ScalarType::U16, ScalarType::S16,
        ScalarType::U32, ScalarType::S32, ScalarType::F32,
        ScalarType::U64, ScalarType::S64, ScalarType::F64,
        ScalarType::EnumU16,
    }) {
        auto c = reg.find(BuiltinScalarCodec::idFor(t));
        REQUIRE(c != nullptr);
        REQUIRE(c->id() == BuiltinScalarCodec::idFor(t));
    }
}

TEST_CASE("CodecRegistry replaces codec on re-register", "[codec][registry]") {
    CodecRegistry reg;
    auto first  = std::make_shared<BuiltinScalarCodec>(ScalarType::U16);
    auto second = std::make_shared<BuiltinScalarCodec>(ScalarType::U16);
    reg.registerCodec(first);
    reg.registerCodec(second);

    REQUIRE(reg.find(first->id()).get() == second.get());
}
