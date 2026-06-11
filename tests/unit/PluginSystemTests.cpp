// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <QDateTime>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/ValueQt.h"
#include "core/plugin/InPort.h"
#include "core/plugin/OutPort.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"

using namespace core;

namespace {

dp::DatapointSpec specFor(QString const& id,
                          dp::Kind kind = dp::Kind::Status,
                          dp::ScalarType type = dp::ScalarType::U16) {
    dp::DatapointSpec s;
    s.id = id;
    s.kind = kind;
    s.type = type;
    return s;
}

} // namespace

TEST_CASE("PortRegistry.bindIn delivers DpChanged to an InPort", "[plugin][port]") {
    bus::EventBus bus;
    dp::DatapointRegistry dps;
    dps.registerDp(std::make_shared<dp::Datapoint>(specFor("a.b")));

    plugin::PortRegistry reg(dps, bus);
    plugin::InPort<int> in;
    int got = -1;
    int calls = 0;
    in.onChanged([&](int v) { got = v; ++calls; });
    reg.bindIn(in, QStringLiteral("a.b"));

    bus.publish(bus::DpChanged{QStringLiteral("a.b"), QVariant(42),
                               QDateTime::currentDateTime()});
    REQUIRE(calls == 1);
    REQUIRE(got == 42);

    // A change to a different datapoint must not reach this port.
    bus.publish(bus::DpChanged{QStringLiteral("other"), QVariant(7),
                               QDateTime::currentDateTime()});
    REQUIRE(calls == 1);
}

TEST_CASE("PortRegistry.bindOut writes an OutPort value to its datapoint",
          "[plugin][port]") {
    bus::EventBus bus;
    dp::DatapointRegistry dps;
    auto cmd = std::make_shared<dp::Datapoint>(
        specFor("x.cmd", dp::Kind::Command, dp::ScalarType::U16));
    QVariant written;
    int writes = 0;
    cmd->setWriter([&](core::dp::Value const& v) { written = core::dp::toQVariant(v); ++writes; });
    dps.registerDp(cmd);

    plugin::PortRegistry reg(dps, bus);
    plugin::OutPort<int> out;
    reg.bindOut(out, QStringLiteral("x.cmd"));

    out.send(0x1234);
    REQUIRE(writes == 1);
    REQUIRE(written.toInt() == 0x1234);
}

TEST_CASE("PluginRegistry.load fails cleanly on a missing DLL", "[plugin]") {
    plugin::PluginRegistry reg;
    REQUIRE_FALSE(reg.load(QStringLiteral("no_such_plugin_xyz")));
    REQUIRE(reg.all().isEmpty());
}
