// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <string>

#include "core/codec/LuaCodec.h"
#include "core/dp/PortRef.h"

using core::codec::LuaCodec;
using core::dp::PortRef;

// CORE_LUA_BCD_SCRIPT is injected by tests/CMakeLists.txt → the real shipped
// data/codec/bcd_datetime.lua, so this exercises the script we ship, not a copy.
#ifndef CORE_LUA_BCD_SCRIPT
#define CORE_LUA_BCD_SCRIPT ""
#endif

namespace {

// 2024-06-03 14:30:45 in the layout documented in bcd_datetime.lua:
//   raw[0]=0x0024 year, raw[1]=0x0603 MM/DD, raw[2]=0x1430 hh/mm, raw[3]=0x4500 ss
core::RegisterWords const kRaw{0x0024, 0x0603, 0x1430, 0x4500};
QString const        kStr = QStringLiteral("2024-06-03 14:30:45");

std::shared_ptr<LuaCodec> loadBcd(std::string* err) {
    return LuaCodec::fromFile("bcd_datetime", CORE_LUA_BCD_SCRIPT, {}, err);
}

} // namespace

TEST_CASE("LuaCodec loads the shipped bcd_datetime script", "[codec][lua]") {
    std::string err;
    auto codec = loadBcd(&err);
    if (!codec) {
        // Built without Lua (CORE_BUILD_LUA=OFF) — fromFile must say so and the
        // round-trip cases below cannot run.
        REQUIRE(err.find("disabled") != std::string::npos);
        SKIP("Core built without Lua codec support: " + err);
    }
    REQUIRE(codec->id() == "bcd_datetime");
}

TEST_CASE("LuaCodec bcd_datetime decode", "[codec][lua]") {
    std::string err;
    auto codec = loadBcd(&err);
    if (!codec) SKIP("Lua disabled: " + err);

    PortRef ref;
    auto const v = codec->decode(kRaw, ref);
    REQUIRE(QString::fromStdString(core::dp::toString(v)) == kStr);
}

TEST_CASE("LuaCodec bcd_datetime encode round-trips", "[codec][lua]") {
    std::string err;
    auto codec = loadBcd(&err);
    if (!codec) SKIP("Lua disabled: " + err);

    PortRef ref;
    core::RegisterWords const back = codec->encode(core::dp::Value(kStr.toStdString()), ref);
    REQUIRE(back == kRaw);
    // decode(encode(x)) == x
    REQUIRE(QString::fromStdString(core::dp::toString(codec->decode(back, ref))) == kStr);
}

TEST_CASE("LuaCodec tolerates malformed input without crashing", "[codec][lua]") {
    std::string err;
    auto codec = loadBcd(&err);
    if (!codec) SKIP("Lua disabled: " + err);

    PortRef ref;
    // Too few registers → script returns nil → null Value.
    REQUIRE(core::dp::isNull(codec->decode(core::RegisterWords{0x0024}, ref)));
    // Unparseable datetime string → script returns {} → empty register list.
    REQUIRE(codec->encode(core::dp::Value(std::string("not-a-date")), ref).empty());
}

TEST_CASE("LuaCodec.fromFile reports a missing script", "[codec][lua]") {
    std::string err;
    auto codec = LuaCodec::fromFile("x", "does/not/exist.lua", {}, &err);
    REQUIRE(codec == nullptr);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("LuaCodec passes the config arg to the script as ctx.arg",
          "[codec][lua]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QString const path = dir.filePath(QStringLiteral("echo_arg.lua"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        // Decode ignores the registers and just returns the selector — so two
        // codecs sharing one script, with different args, produce different values.
        f.write("return { decode = function(raw, ctx) return ctx.arg end,"
                "          encode = function(v, ctx) return {} end }");
    }

    std::string err;
    auto hi = LuaCodec::fromFile("c1", path.toStdString(), "high", &err);
    auto lo = LuaCodec::fromFile("c2", path.toStdString(), "low",  &err);
    if (!hi) SKIP("Lua disabled: " + err);

    PortRef ref;
    REQUIRE(QString::fromStdString(core::dp::toString(hi->decode(core::RegisterWords{0}, ref))) == QStringLiteral("high"));
    REQUIRE(QString::fromStdString(core::dp::toString(lo->decode(core::RegisterWords{0}, ref))) == QStringLiteral("low"));
}
