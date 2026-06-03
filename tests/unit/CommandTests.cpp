#include <catch2/catch_test_macros.hpp>

#include "core/module/Command.h"
#include "core/sched/SchedulerTypes.h"
#include "mocks/MockTransport.h"

using namespace core;

TEST_CASE("Command.execute writes each configured entry exactly once",
          "[command][basic]") {
    test::MockTransport mock;
    module::Command::Config cfg;
    cfg.moduleId = "cmd.estop";
    cfg.priority = sched::Priority::Critical;
    cfg.writes = {
        {QModbusDataUnit::HoldingRegisters, 400, 0xDEAD},
        {QModbusDataUnit::HoldingRegisters, 401, 0xBEEF},
    };
    module::Command cmd(cfg, mock);

    auto r = cmd.execute();
    REQUIRE(r.kind == sched::ResultKind::Ok);

    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0].startAddress == 400);
    REQUIRE(writes[0].values == QList<quint16>{0xDEAD});
    REQUIRE(writes[1].startAddress == 401);
    REQUIRE(writes[1].values == QList<quint16>{0xBEEF});
}

TEST_CASE("Command.execute reports the first failure but continues other writes",
          "[command][error]") {
    test::MockTransport mock;
    module::Command::Config cfg;
    cfg.moduleId = "cmd";
    cfg.writes = {
        {QModbusDataUnit::HoldingRegisters, 1, 1},
        {QModbusDataUnit::HoldingRegisters, 2, 2},
        {QModbusDataUnit::HoldingRegisters, 3, 3},
    };
    module::Command cmd(cfg, mock);

    transport::WriteResult fail; fail.ok = false; fail.errorMessage = "fail2";
    mock.enqueueWriteResult({true, {}});
    mock.enqueueWriteResult(fail);
    mock.enqueueWriteResult({true, {}});

    auto r = cmd.execute();
    REQUIRE(r.kind == sched::ResultKind::Error);
    REQUIRE(r.errorMessage == "fail2");
    REQUIRE(mock.capturedWrites().size() == 3);   // all 3 attempted
}

TEST_CASE("Command.executeAsync writes each entry via the async scheduler path",
          "[command][async]") {
    test::MockTransport mock;
    module::Command::Config cfg;
    cfg.moduleId = "cmd.async";
    cfg.priority = sched::Priority::Critical;
    cfg.writes = {
        {QModbusDataUnit::HoldingRegisters, 400, 0xDEAD},
        {QModbusDataUnit::HoldingRegisters, 401, 0xBEEF},
    };
    module::Command cmd(cfg, mock);

    cmd.executeAsync();   // sync mock async → serialised writes complete inline

    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0].startAddress == 400);
    REQUIRE(writes[0].values == QList<quint16>{0xDEAD});
    REQUIRE(writes[1].startAddress == 401);
    REQUIRE(writes[1].values == QList<quint16>{0xBEEF});

    // The scheduler is now in async mode; a synchronous execute() is rejected.
    REQUIRE(cmd.execute().kind == sched::ResultKind::Error);
}

TEST_CASE("Command exposes its configured priority", "[command][priority]") {
    test::MockTransport mock;
    module::Command::Config cfg;
    cfg.moduleId = "cmd";
    cfg.priority = sched::Priority::Critical;
    module::Command cmd(cfg, mock);
    REQUIRE(cmd.priority() == sched::Priority::Critical);
}
