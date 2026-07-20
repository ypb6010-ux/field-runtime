// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <memory>
#include <stdexcept>
#include <utility>

#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"

using namespace core::dp;

namespace {

DatapointSpec specFor(QString const& id,
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
    REQUIRE_FALSE(d.value().isValid());
}

TEST_CASE("Datapoint.setValue transitions to Ok and emits valueChanged",
          "[dp]") {
    Datapoint d(specFor("foo"));
    QSignalSpy valueSpy(&d, &Datapoint::valueChanged);
    QSignalSpy stateSpy(&d, &Datapoint::stateChanged);

    d.setValue(42);

    REQUIRE(d.value().toInt() == 42);
    REQUIRE(d.valid());
    REQUIRE(d.state() == DpState::Ok);
    REQUIRE(valueSpy.count() == 1);
    REQUIRE(stateSpy.count() == 1);
}

TEST_CASE("Datapoint.setValue does not re-emit when value is unchanged",
          "[dp]") {
    Datapoint d(specFor("foo"));
    QDateTime const t0(QDate(2026, 7, 17), QTime(10, 0, 0));
    d.setValue(7, t0);

    QSignalSpy valueSpy(&d, &Datapoint::valueChanged);
    QSignalSpy timestampSpy(&d, &Datapoint::timestampChanged);

    d.setValue(7, t0.addMSecs(100));   // identical value, fresh sample
    REQUIRE(valueSpy.count() == 0);
    REQUIRE(timestampSpy.count() == 1);
    REQUIRE(d.timestamp() == t0.addMSecs(100));

    d.setValue(8, t0.addMSecs(200));
    REQUIRE(valueSpy.count() == 1);
    REQUIRE(timestampSpy.count() == 2);
}

TEST_CASE("Datapoint.setValue re-notifies state when validity recovers",
          "[dp]") {
    Datapoint d(specFor("foo"));
    d.setValue(7);
    d.setState(DpState::Stale);

    QSignalSpy valueSpy(&d, &Datapoint::valueChanged);
    QSignalSpy stateSpy(&d, &Datapoint::stateChanged);

    d.setValue(7);

    REQUIRE(d.valid());
    REQUIRE(valueSpy.count() == 0);
    REQUIRE(stateSpy.count() == 1);
}

TEST_CASE("Datapoint.setState changes state and emits both signals",
          "[dp]") {
    Datapoint d(specFor("foo"));
    d.setValue(1);

    QSignalSpy valueSpy(&d, &Datapoint::valueChanged);
    QSignalSpy stateSpy(&d, &Datapoint::stateChanged);

    d.setState(DpState::Stale);
    REQUIRE(d.state() == DpState::Stale);
    REQUIRE(d.stateText() == "Stale");
    REQUIRE_FALSE(d.valid());
    REQUIRE(stateSpy.count() == 1);
    REQUIRE(valueSpy.count() == 0);
}

TEST_CASE("Datapoint.write invokes the registered writer", "[dp][write]") {
    Datapoint d(specFor("cmd", Kind::Command, ScalarType::Bool));

    QVariant received;
    int calls = 0;
    d.setWriter([&](QVariant const& v) { received = v; ++calls; });

    d.write(true);
    REQUIRE(calls == 1);
    REQUIRE(received.toBool());

    d.write(false);
    REQUIRE(calls == 2);
    REQUIRE_FALSE(received.toBool());
}

TEST_CASE("Datapoint.write without a writer is a no-op", "[dp][write]") {
    Datapoint d(specFor("cmd", Kind::Command));
    REQUIRE_NOTHROW(d.write(123));
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

TEST_CASE("DatapointRegistry rejects duplicate ids to preserve QML pointer lifetime",
          "[dp][registry]") {
    DatapointRegistry reg;
    auto first  = std::make_shared<Datapoint>(specFor("dup"));
    auto second = std::make_shared<Datapoint>(specFor("dup"));
    REQUIRE(reg.registerDp(first));
    REQUIRE_FALSE(reg.registerDp(second));

    REQUIRE(reg.find("dup").get() == first.get());
    REQUIRE(reg.all().size() == 1);
}

TEST_CASE("Datapoint id is immutable after assignment", "[dp][spec][lifetime]") {
    Datapoint point(specFor("stable"));
    auto changed = specFor("changed");
    REQUIRE_THROWS_AS(point.setSpec(std::move(changed)), std::invalid_argument);
    REQUIRE(point.id() == "stable");
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
