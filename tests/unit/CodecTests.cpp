#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <memory>
#include <unordered_map>

#include "core/codec/BuiltinCodecs.h"
#include "core/codec/CodecRegistry.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"

using namespace core::codec;
using namespace core::dp;
using Catch::Matchers::WithinAbs;

namespace {

PortRef portFor(WordOrder wo               = WordOrder::ABCD,
                std::optional<int> bit     = std::nullopt,
                int       shift            = 0,
                quint64   mask             = 0xFFFFFFFFFFFFFFFFull,
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

} // namespace

// ===========================================================================
// 16-bit
// ===========================================================================

TEST_CASE("U16 round-trips raw register values", "[codec][u16]") {
    BuiltinScalarCodec c(ScalarType::U16);
    auto ref = portFor();

    auto regs = c.encode(quint16(0xABCD), ref);
    REQUIRE(regs == QList<quint16>{0xABCD});

    auto v = c.decode(regs, ref);
    REQUIRE(v.value<quint16>() == 0xABCD);
}

TEST_CASE("S16 sign-extends negative values", "[codec][s16]") {
    BuiltinScalarCodec c(ScalarType::S16);
    auto ref = portFor();

    auto regs = c.encode(qint16(-2), ref);
    REQUIRE(regs == QList<quint16>{0xFFFE});

    auto v = c.decode(QList<quint16>{0xFFFE}, ref);
    REQUIRE(v.value<qint16>() == -2);
}

TEST_CASE("S16 applies scale and offset linear transform", "[codec][s16][scale]") {
    BuiltinScalarCodec c(ScalarType::S16);
    auto ref = portFor(WordOrder::ABCD, std::nullopt, 0, 0xFFFFu, 0.1, -40.0);

    // raw 600 → 600 * 0.1 + (-40) = 20.0
    auto v = c.decode(QList<quint16>{600}, ref);
    REQUIRE_THAT(v.toDouble(), WithinAbs(20.0, 1e-9));

    // 20.0 → (20 - (-40)) / 0.1 = 600
    auto regs = c.encode(20.0, ref);
    REQUIRE(regs == QList<quint16>{600});
}

TEST_CASE("U16 shift+mask isolates bit field", "[codec][u16][bits]") {
    BuiltinScalarCodec c(ScalarType::U16);
    auto ref = portFor(WordOrder::ABCD, std::nullopt,
                       /*shift*/4, /*mask*/0x000Fu);

    // raw 0x00F0 → (0x00F0 >> 4) & 0x0F = 0x0F
    auto v = c.decode(QList<quint16>{0x00F0}, ref);
    REQUIRE(v.value<quint16>() == 0x0F);

    // 0x0F → (0x0F & 0x0F) << 4 = 0x00F0
    auto regs = c.encode(quint16(0x0F), ref);
    REQUIRE(regs == QList<quint16>{0x00F0});
}

// ===========================================================================
// 32-bit and word order
// ===========================================================================

TEST_CASE("U32 ABCD round-trips a known constant", "[codec][u32][abcd]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::ABCD);

    auto regs = c.encode(quint32(0x12345678u), ref);
    REQUIRE(regs == QList<quint16>{0x1234, 0x5678});

    auto v = c.decode(regs, ref);
    REQUIRE(v.value<quint32>() == 0x12345678u);
}

TEST_CASE("U32 CDAB swaps the word pair", "[codec][u32][cdab]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(quint32(0x12345678u), ref);
    REQUIRE(regs == QList<quint16>{0x5678, 0x1234});

    auto v = c.decode(regs, ref);
    REQUIRE(v.value<quint32>() == 0x12345678u);
}

TEST_CASE("U32 BADC swaps bytes within each word", "[codec][u32][badc]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::BADC);

    auto regs = c.encode(quint32(0x12345678u), ref);
    REQUIRE(regs == QList<quint16>{0x3412, 0x7856});

    auto v = c.decode(regs, ref);
    REQUIRE(v.value<quint32>() == 0x12345678u);
}

TEST_CASE("U32 DCBA fully reverses bytes", "[codec][u32][dcba]") {
    BuiltinScalarCodec c(ScalarType::U32);
    auto ref = portFor(WordOrder::DCBA);

    auto regs = c.encode(quint32(0x12345678u), ref);
    REQUIRE(regs == QList<quint16>{0x7856, 0x3412});

    auto v = c.decode(regs, ref);
    REQUIRE(v.value<quint32>() == 0x12345678u);
}

TEST_CASE("S32 sign-extends after permutation", "[codec][s32]") {
    BuiltinScalarCodec c(ScalarType::S32);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(qint32(-1), ref);
    auto v    = c.decode(regs, ref);
    REQUIRE(v.value<qint32>() == -1);
}

// ===========================================================================
// IEEE float
// ===========================================================================

TEST_CASE("F32 ABCD round-trips with IEEE bit pattern", "[codec][f32]") {
    BuiltinScalarCodec c(ScalarType::F32);
    auto ref = portFor(WordOrder::ABCD);

    // 23.5 → 0x41BC0000 IEEE-754
    auto regs = c.encode(23.5, ref);
    REQUIRE(regs.size() == 2);
    REQUIRE(regs[0] == 0x41BC);
    REQUIRE(regs[1] == 0x0000);

    auto v = c.decode(regs, ref);
    REQUIRE_THAT(v.toDouble(), WithinAbs(23.5, 1e-6));
}

TEST_CASE("F32 CDAB places the high word second", "[codec][f32][cdab]") {
    BuiltinScalarCodec c(ScalarType::F32);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(23.5, ref);
    REQUIRE(regs == QList<quint16>{0x0000, 0x41BC});

    auto v = c.decode(regs, ref);
    REQUIRE_THAT(v.toDouble(), WithinAbs(23.5, 1e-6));
}

TEST_CASE("F32 with scale handles milli-unit physical sensors", "[codec][f32][scale]") {
    BuiltinScalarCodec c(ScalarType::F32);
    auto ref = portFor(WordOrder::CDAB, std::nullopt, 0, 0xFFFFFFFFu,
                       /*scale*/0.001, /*offset*/0.0);

    auto raw  = c.encode(7.234, ref);   // expressed as milli-unit on the wire
    auto back = c.decode(raw,  ref);
    REQUIRE_THAT(back.toDouble(), WithinAbs(7.234, 1e-4));
}

// ===========================================================================
// 64-bit
// ===========================================================================

TEST_CASE("F64 round-trips with ABCD ordering", "[codec][f64]") {
    BuiltinScalarCodec c(ScalarType::F64);
    auto ref = portFor(WordOrder::ABCD);

    auto regs = c.encode(3.141592653589793, ref);
    REQUIRE(regs.size() == 4);

    auto v = c.decode(regs, ref);
    REQUIRE_THAT(v.toDouble(), WithinAbs(3.141592653589793, 1e-12));
}

TEST_CASE("U64 round-trips with CDAB ordering", "[codec][u64]") {
    BuiltinScalarCodec c(ScalarType::U64);
    auto ref = portFor(WordOrder::CDAB);

    auto regs = c.encode(QVariant::fromValue<quint64>(0x0123456789ABCDEFull), ref);
    auto v    = c.decode(regs, ref);
    REQUIRE(v.value<quint64>() == 0x0123456789ABCDEFull);
}

// ===========================================================================
// Bool
// ===========================================================================

TEST_CASE("Bool encodes only the requested bit", "[codec][bool]") {
    BuiltinScalarCodec c(ScalarType::Bool);
    auto ref = portFor(WordOrder::ABCD, /*bit*/3);

    auto regs = c.encode(true, ref);
    REQUIRE(regs == QList<quint16>{0x0008});

    auto v = c.decode(QList<quint16>{0x0008}, ref);
    REQUIRE(v.toBool());

    auto v0 = c.decode(QList<quint16>{0x0000}, ref);
    REQUIRE_FALSE(v0.toBool());
}

TEST_CASE("Bool decodes from a register with multiple bits set",
          "[codec][bool]") {
    BuiltinScalarCodec c(ScalarType::Bool);
    auto refB3 = portFor(WordOrder::ABCD, /*bit*/3);
    auto refB7 = portFor(WordOrder::ABCD, /*bit*/7);

    auto src = QList<quint16>{0x0088};   // bits 3 and 7 set
    REQUIRE(c.decode(src, refB3).toBool());
    REQUIRE(c.decode(src, refB7).toBool());
}

// ===========================================================================
// EnumU16Codec
// ===========================================================================

TEST_CASE("EnumU16Codec maps raw values to names", "[codec][enum]") {
    EnumU16Codec c(QStringLiteral("belt_state"), {
        {0, QStringLiteral("Stopped")},
        {1, QStringLiteral("Starting")},
        {2, QStringLiteral("Running")},
        {4, QStringLiteral("Fault")},
    });
    auto ref = portFor();

    REQUIRE(c.decode(QList<quint16>{0}, ref).toString() == "Stopped");
    REQUIRE(c.decode(QList<quint16>{2}, ref).toString() == "Running");
    REQUIRE(c.decode(QList<quint16>{4}, ref).toString() == "Fault");

    // Unknown keys produce a stable placeholder, never empty.
    REQUIRE(c.decode(QList<quint16>{99}, ref).toString() == "Unknown(99)");
}

TEST_CASE("EnumU16Codec encodes by name and by numeric fallback",
          "[codec][enum]") {
    EnumU16Codec c(QStringLiteral("belt_state"), {
        {0, QStringLiteral("Stopped")},
        {2, QStringLiteral("Running")},
    });
    auto ref = portFor();

    REQUIRE(c.encode(QStringLiteral("Running"), ref) == QList<quint16>{2});
    REQUIRE(c.encode(QStringLiteral("Stopped"), ref) == QList<quint16>{0});

    // Unknown name → fallback to numeric coercion of the QVariant
    REQUIRE(c.encode(7, ref) == QList<quint16>{7});
}

TEST_CASE("EnumU16Codec masks before lookup", "[codec][enum][mask]") {
    EnumU16Codec c(QStringLiteral("low_nibble"), {
        {0, QStringLiteral("Idle")},
        {1, QStringLiteral("Run")},
    });
    auto ref = portFor(WordOrder::ABCD, std::nullopt, 0, 0x000Fu);

    REQUIRE(c.decode(QList<quint16>{0xFFF0}, ref).toString() == "Idle");
    REQUIRE(c.decode(QList<quint16>{0x00F1}, ref).toString() == "Run");
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

    REQUIRE(reg.find(QStringLiteral("nope")) == nullptr);
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
