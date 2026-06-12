// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/Value.h"

using namespace core::dp;

namespace {

DatapointSpec specFor(std::string const& id,
                       Kind            kind = Kind::Status,
                       ScalarType      type = ScalarType::U16) {
    DatapointSpec s;
    s.id   = id;
    s.kind = kind;
    s.type = type;
    return s;
}

} // namespace

TEST_CASE("Datapoint starts in Missing state with no value", "[dp]") {
    Datapoint d(specFor("foo"));
    REQUIRE(d.id() == "foo");
    REQUIRE(d.state() == DpState::Missing);
    REQUIRE(d.stateText() == "Missing");
    REQUIRE_FALSE(d.valid());
    REQUIRE(isNull(d.value()));
}

TEST_CASE("Datapoint.setValue transitions to Ok and fires onValueChanged",
          "[dp]") {
    Datapoint d(specFor("foo"));
    int valueCount = 0;
    int stateCount = 0;
    d.setOnValueChanged([&] { ++valueCount; });
    d.setOnStateChanged([&] { ++stateCount; });

    d.setValue(std::int64_t(42));

    REQUIRE(toInt64(d.value()) == 42);
    REQUIRE(d.valid());
    REQUIRE(d.state() == DpState::Ok);
    REQUIRE(valueCount == 1);
    REQUIRE(stateCount == 1);
}

TEST_CASE("Datapoint.setValue does not re-notify when value is unchanged",
          "[dp]") {
    Datapoint d(specFor("foo"));
    d.setValue(std::int64_t(7));

    int valueCount = 0;
    d.setOnValueChanged([&] { ++valueCount; });

    d.setValue(std::int64_t(7));   // identical value
    REQUIRE(valueCount == 0);

    d.setValue(std::int64_t(8));
    REQUIRE(valueCount == 1);
}

TEST_CASE("Datapoint.setState changes state and fires both callbacks",
          "[dp]") {
    Datapoint d(specFor("foo"));
    d.setValue(std::int64_t(1));

    int valueCount = 0;
    int stateCount = 0;
    d.setOnValueChanged([&] { ++valueCount; });
    d.setOnStateChanged([&] { ++stateCount; });

    d.setState(DpState::Stale);
    REQUIRE(d.state() == DpState::Stale);
    REQUIRE(d.stateText() == "Stale");
    REQUIRE_FALSE(d.valid());
    REQUIRE(stateCount == 1);
    REQUIRE(valueCount == 1);   // a `valid` QML binding also rebinds
}

TEST_CASE("Datapoint.write invokes the registered writer", "[dp][write]") {
    Datapoint d(specFor("cmd", Kind::Command, ScalarType::Bool));

    Value received;
    int calls = 0;
    d.setWriter([&](Value const& v) { received = v; ++calls; });

    d.write(true);
    REQUIRE(calls == 1);
    REQUIRE(toBool(received));

    d.write(false);
    REQUIRE(calls == 2);
    REQUIRE_FALSE(toBool(received));
}

TEST_CASE("Datapoint.write without a writer is a no-op", "[dp][write]") {
    Datapoint d(specFor("cmd", Kind::Command));
    REQUIRE_NOTHROW(d.write(std::int64_t(123)));
}

TEST_CASE("DatapointRegistry stores and retrieves datapoints",
          "[dp][registry]") {
    DatapointRegistry reg;
    auto a = std::make_shared<Datapoint>(specFor("a"));
    auto b = std::make_shared<Datapoint>(specFor("b"));

    reg.registerDp(a);
    reg.registerDp(b);

    REQUIRE(reg.find("a").get() == a.get());
    REQUIRE(reg.find("b").get() == b.get());
    REQUIRE(reg.find("missing") == nullptr);
    REQUIRE(reg.all().size() == 2);
}

TEST_CASE("DatapointRegistry replaces dp when re-registered with same id",
          "[dp][registry]") {
    DatapointRegistry reg;
    auto first  = std::make_shared<Datapoint>(specFor("dup"));
    auto second = std::make_shared<Datapoint>(specFor("dup"));
    reg.registerDp(first);
    reg.registerDp(second);

    REQUIRE(reg.find("dup").get() == second.get());
    REQUIRE(reg.all().size() == 1);
}

TEST_CASE("DatapointSpec exposes static config through getters", "[dp][spec]") {
    DatapointSpec spec;
    spec.id         = "belt2.speed";
    spec.kind       = Kind::Status;
    spec.type       = ScalarType::F32;
    spec.uiBinding  = "attributes.belt2Speed";
    spec.persistTag = "telemetry.belt2.speed";
    PortRef src;
    src.transport = "modbus:PLC1";
    src.address   = 0x0103;
    src.wordOrder = WordOrder::CDAB;
    spec.source   = src;

    Datapoint d(spec);
    REQUIRE(d.id()         == "belt2.speed");
    REQUIRE(d.kind()       == Kind::Status);
    REQUIRE(d.type()       == ScalarType::F32);
    REQUIRE(d.uiBinding()  == "attributes.belt2Speed");
    REQUIRE(d.persistTag() == "telemetry.belt2.speed");
    REQUIRE(d.source().has_value());
    REQUIRE(d.source()->transport == "modbus:PLC1");
    REQUIRE(d.source()->wordOrder == WordOrder::CDAB);
    REQUIRE_FALSE(d.sink().has_value());
}
