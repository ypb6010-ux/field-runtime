#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>

#include "core/codec/BuiltinCodecs.h"
#include "core/dp/Datapoint.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/module/PollRange.h"
#include "core/sched/SchedulerTypes.h"
#include "core/transport/TransportTypes.h"
#include "mocks/MockTransport.h"

using namespace core;
using Catch::Matchers::WithinAbs;

namespace {

dp::DatapointSpec dpSpec(QString const& id,
                          dp::ScalarType  type,
                          int             address,
                          dp::WordOrder   wo = dp::WordOrder::ABCD,
                          std::optional<int> bit = std::nullopt) {
    dp::DatapointSpec spec;
    spec.id   = id;
    spec.kind = dp::Kind::Status;
    spec.type = type;
    dp::PortRef src;
    src.transport = "mock";
    src.table     = QModbusDataUnit::HoldingRegisters;
    src.address   = address;
    src.bit       = bit;
    src.wordOrder = wo;
    spec.source   = src;
    return spec;
}

transport::ReadRequest readRange(int start, int count) {
    return {QModbusDataUnit::HoldingRegisters, start, count};
}

} // namespace

TEST_CASE("PollRange submits exactly one read through the scheduler per tick",
          "[poll][basic]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock.range"),
                           mock, readRange(0, 4), /*periodMs*/ 100);

    mock.enqueueReadValues({0x0001, 0x0002, 0x0003, 0x0004});

    auto r = poll.pollOnce();
    REQUIRE(r.kind == sched::ResultKind::Ok);
    REQUIRE(mock.readCount() == 1);
    REQUIRE(mock.capturedReads().first().startAddress == 0);
    REQUIRE(mock.capturedReads().first().count       == 4);
}

TEST_CASE("PollRange dispatches decoded values into bound datapoints",
          "[poll][dispatch]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(100, 4), 100);

    // Two datapoints bound:
    //   - U16 at addr 100 → offset 0
    //   - S16 at addr 102 → offset 2 (scale 0.1, offset -40)
    auto specA = dpSpec("a", dp::ScalarType::U16, 100);
    auto specB = dpSpec("b", dp::ScalarType::S16, 102);
    specB.source->scale  = 0.1;
    specB.source->offset = -40.0;

    auto a = std::make_shared<dp::Datapoint>(specA);
    auto b = std::make_shared<dp::Datapoint>(specB);

    poll.bind(a, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::U16), 0);
    poll.bind(b, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::S16), 2);

    mock.enqueueReadValues({0x0042, 0x0000, 600, 0x0000});

    auto r = poll.pollOnce();
    REQUIRE(r.kind == sched::ResultKind::Ok);
    REQUIRE(a->value().value<quint16>() == 0x42);
    REQUIRE_THAT(b->value().toDouble(), WithinAbs(20.0, 1e-9));
    REQUIRE(a->state() == dp::DpState::Ok);
    REQUIRE(b->state() == dp::DpState::Ok);
}

TEST_CASE("PollRange decodes multi-register datapoints honouring word order",
          "[poll][f32]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(0x100, 2), 100);

    auto spec = dpSpec("speed", dp::ScalarType::F32, 0x100, dp::WordOrder::CDAB);
    auto dpt  = std::make_shared<dp::Datapoint>(spec);
    poll.bind(dpt, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::F32), 0);

    // 23.5f under CDAB ⇒ [0x0000, 0x41BC]
    mock.enqueueReadValues({0x0000, 0x41BC});

    REQUIRE(poll.pollOnce().kind == sched::ResultKind::Ok);
    REQUIRE_THAT(dpt->value().toDouble(), WithinAbs(23.5, 1e-6));
}

TEST_CASE("PollRange propagates transport errors to bound datapoints",
          "[poll][error]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(0, 2), 100);

    auto spec = dpSpec("a", dp::ScalarType::U16, 0);
    auto dpt  = std::make_shared<dp::Datapoint>(spec);
    dpt->setValue(quint16(5));   // pre-existing Ok state
    REQUIRE(dpt->state() == dp::DpState::Ok);

    poll.bind(dpt, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::U16), 0);

    mock.enqueueReadError(QStringLiteral("ServerDeviceBusy"));

    auto r = poll.pollOnce();
    REQUIRE(r.kind == sched::ResultKind::Error);
    REQUIRE(r.errorMessage == "ServerDeviceBusy");
    REQUIRE(dpt->state() == dp::DpState::Error);
}

TEST_CASE("PollRange ignores out-of-bound bindings instead of crashing",
          "[poll][safety]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(0, 2), 100);

    auto spec = dpSpec("offrange", dp::ScalarType::U32, 4);
    auto dpt  = std::make_shared<dp::Datapoint>(spec);
    poll.bind(dpt, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::U32),
              /*registerOffset*/ 4);    // 4..6 exceeds range size 2

    mock.enqueueReadValues({1, 2});

    REQUIRE(poll.pollOnce().kind == sched::ResultKind::Ok);
    REQUIRE(dpt->state() == dp::DpState::Error);   // marked, did not segfault
}

TEST_CASE("PollRange marks datapoints Error when scheduler reports failure",
          "[poll][sched]") {
    // The default MockTransport scheduler is healthy; we instead exercise
    // the cancelled / circuit-open paths by tripping its scheduler before
    // the tick runs.
    test::MockTransport mock;
    auto& sched = static_cast<core::sched::SerialScheduler&>(mock.scheduler());
    for (int i = 0; i < 10; ++i) sched.recordFailureForTesting();
    REQUIRE(sched.stats().circuitState == core::sched::CircuitState::Open);

    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(0, 2), 100);
    auto spec = dpSpec("a", dp::ScalarType::U16, 0);
    auto dpt  = std::make_shared<dp::Datapoint>(spec);
    dpt->setValue(quint16(1));
    poll.bind(dpt, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::U16), 0);

    auto r = poll.pollOnce();
    REQUIRE(r.kind == core::sched::ResultKind::CircuitOpen);
    REQUIRE(dpt->state() == dp::DpState::Stale);
    REQUIRE(mock.readCount() == 0);   // no I/O attempted while circuit open
}

TEST_CASE("PollRange.stop() pauses ticks and resume() restarts them",
          "[poll][lifecycle]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(0, 1), 100);

    auto spec = dpSpec("a", dp::ScalarType::U16, 0);
    auto dpt  = std::make_shared<dp::Datapoint>(spec);
    poll.bind(dpt, std::make_shared<codec::BuiltinScalarCodec>(dp::ScalarType::U16), 0);

    poll.stop();
    auto r1 = poll.pollOnce();
    REQUIRE(r1.kind == core::sched::ResultKind::Cancelled);
    REQUIRE(mock.readCount() == 0);

    poll.resume();
    mock.enqueueReadValues({0x77});
    auto r2 = poll.pollOnce();
    REQUIRE(r2.kind == core::sched::ResultKind::Ok);
    REQUIRE(dpt->value().value<quint16>() == 0x77);
}

TEST_CASE("PollRange uses its moduleId for the scheduler tag",
          "[poll][tag]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock.unique"),
                           mock, readRange(0, 1), 100);

    poll.pollOnce();
    REQUIRE(poll.id() == "poll.mock.unique");
    // Round-robin & cancelModule both pivot on moduleId; verifying its
    // propagation here keeps PollRange compatible with those scheduler
    // behaviours (already covered by SerialScheduler tests).
    auto cancelled = mock.scheduler().cancelModule(QStringLiteral("poll.mock.unique"));
    REQUIRE(cancelled == 0);   // none pending — we just exercise the lookup
}

TEST_CASE("PollRange supports no bindings (raw poll-only mode)",
          "[poll][empty]") {
    test::MockTransport mock;
    module::PollRange poll(QStringLiteral("poll.mock"),
                           mock, readRange(0, 4), 100);

    mock.enqueueReadValues({1, 2, 3, 4});

    auto r = poll.pollOnce();
    REQUIRE(r.kind == core::sched::ResultKind::Ok);
    REQUIRE(mock.readCount() == 1);
    REQUIRE(poll.bindingCount() == 0);
}
