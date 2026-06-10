// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/Version.h"
#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"

TEST_CASE("Core exposes a non-empty version string", "[core][version]") {
    auto const* v = core::versionString();
    REQUIRE(v != nullptr);
    REQUIRE(std::string_view{v}.size() > 0);
}

TEST_CASE("ScalarType register counts", "[dp][scalar]") {
    using core::dp::ScalarType;
    using core::dp::registerCountFor;
    using core::dp::isMultiRegister;

    REQUIRE(registerCountFor(ScalarType::Bool)    == 1);
    REQUIRE(registerCountFor(ScalarType::U16)     == 1);
    REQUIRE(registerCountFor(ScalarType::S16)     == 1);
    REQUIRE(registerCountFor(ScalarType::EnumU16) == 1);
    REQUIRE(registerCountFor(ScalarType::U32)     == 2);
    REQUIRE(registerCountFor(ScalarType::S32)     == 2);
    REQUIRE(registerCountFor(ScalarType::F32)     == 2);
    REQUIRE(registerCountFor(ScalarType::U64)     == 4);
    REQUIRE(registerCountFor(ScalarType::F64)     == 4);

    REQUIRE_FALSE(isMultiRegister(ScalarType::U16));
    REQUIRE(isMultiRegister(ScalarType::U32));
    REQUIRE(isMultiRegister(ScalarType::F64));
}

TEST_CASE("WordOrder byte permutation tables", "[dp][wordorder]") {
    using core::dp::WordOrder;
    using core::dp::permutationFor;

    // 0x12345678 encoded as two 16-bit Modbus regs is [0x12,0x34,0x56,0x78]
    // in network order. Each permutation tells us where each byte of the
    // host-order value should be sourced from.
    SECTION("32-bit ABCD = identity") {
        auto p = permutationFor(WordOrder::ABCD, 4);
        REQUIRE(p.order[0] == 0);
        REQUIRE(p.order[1] == 1);
        REQUIRE(p.order[2] == 2);
        REQUIRE(p.order[3] == 3);
    }
    SECTION("32-bit CDAB swaps the two 16-bit words") {
        auto p = permutationFor(WordOrder::CDAB, 4);
        REQUIRE(p.order[0] == 2);
        REQUIRE(p.order[1] == 3);
        REQUIRE(p.order[2] == 0);
        REQUIRE(p.order[3] == 1);
    }
    SECTION("32-bit BADC swaps bytes within each word") {
        auto p = permutationFor(WordOrder::BADC, 4);
        REQUIRE(p.order[0] == 1);
        REQUIRE(p.order[1] == 0);
        REQUIRE(p.order[2] == 3);
        REQUIRE(p.order[3] == 2);
    }
    SECTION("32-bit DCBA fully reverses") {
        auto p = permutationFor(WordOrder::DCBA, 4);
        REQUIRE(p.order[0] == 3);
        REQUIRE(p.order[1] == 2);
        REQUIRE(p.order[2] == 1);
        REQUIRE(p.order[3] == 0);
    }
    SECTION("64-bit CDAB matches the documented pairing") {
        auto p = permutationFor(WordOrder::CDAB, 8);
        REQUIRE(p.order[0] == 6);
        REQUIRE(p.order[7] == 1);
    }
}
