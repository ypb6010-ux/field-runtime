// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QList>
#include <QString>
#include <QTemporaryDir>

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

std::shared_ptr<LuaCodec> loadBcd(QString* err) {
    return LuaCodec::fromFile(QStringLiteral("bcd_datetime"),
                              QStringLiteral(CORE_LUA_BCD_SCRIPT), {}, err);
}

} // namespace

TEST_CASE("LuaCodec loads the shipped bcd_datetime script", "[codec][lua]") {
    QString err;
    auto codec = loadBcd(&err);
    if (!codec) {
        // Built without Lua (CORE_BUILD_LUA=OFF) — fromFile must say so and the
        // round-trip cases below cannot run.
        REQUIRE(err.contains(QStringLiteral("disabled")));
        SKIP("Core built without Lua codec support: " + err.toStdString());
    }
    REQUIRE(codec->id() == QStringLiteral("bcd_datetime"));
}

TEST_CASE("LuaCodec bcd_datetime decode", "[codec][lua]") {
    QString err;
    auto codec = loadBcd(&err);
    if (!codec) SKIP("Lua disabled: " + err.toStdString());

    PortRef ref;
    QVariant const v = codec->decode(kRaw, ref);
    REQUIRE(v.toString() == kStr);
}

TEST_CASE("LuaCodec bcd_datetime encode round-trips", "[codec][lua]") {
    QString err;
    auto codec = loadBcd(&err);
    if (!codec) SKIP("Lua disabled: " + err.toStdString());

    PortRef ref;
    core::RegisterWords const back = codec->encode(kStr, ref);
    REQUIRE(back == kRaw);
    // decode(encode(x)) == x
    REQUIRE(codec->decode(back, ref).toString() == kStr);
}

TEST_CASE("LuaCodec tolerates malformed input without crashing", "[codec][lua]") {
    QString err;
    auto codec = loadBcd(&err);
    if (!codec) SKIP("Lua disabled: " + err.toStdString());

    PortRef ref;
    // Too few registers → script returns nil → invalid QVariant.
    REQUIRE_FALSE(codec->decode(core::RegisterWords{0x0024}, ref).isValid());
    // Unparseable datetime string → script returns {} → empty register list.
    REQUIRE(codec->encode(QStringLiteral("not-a-date"), ref).empty());
}

TEST_CASE("LuaCodec.fromFile reports a missing script", "[codec][lua]") {
    QString err;
    auto codec = LuaCodec::fromFile(QStringLiteral("x"),
                                    QStringLiteral("does/not/exist.lua"), {}, &err);
    REQUIRE(codec == nullptr);
    REQUIRE_FALSE(err.isEmpty());
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

    QString err;
    auto hi = LuaCodec::fromFile(QStringLiteral("c1"), path, QStringLiteral("high"), &err);
    auto lo = LuaCodec::fromFile(QStringLiteral("c2"), path, QStringLiteral("low"),  &err);
    if (!hi) SKIP("Lua disabled: " + err.toStdString());

    PortRef ref;
    REQUIRE(hi->decode(core::RegisterWords{0}, ref).toString() == QStringLiteral("high"));
    REQUIRE(lo->decode(core::RegisterWords{0}, ref).toString() == QStringLiteral("low"));
}
