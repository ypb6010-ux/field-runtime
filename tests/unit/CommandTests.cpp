// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
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
        {core::RegisterTable::HoldingRegister, 400, 0xDEAD},
        {core::RegisterTable::HoldingRegister, 401, 0xBEEF},
    };
    module::Command cmd(cfg, mock);

    auto r = cmd.execute();
    REQUIRE(r.kind == sched::ResultKind::Ok);

    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0].startAddress == 400);
    REQUIRE(writes[0].values == core::RegisterWords{0xDEAD});
    REQUIRE(writes[1].startAddress == 401);
    REQUIRE(writes[1].values == core::RegisterWords{0xBEEF});
}

TEST_CASE("Command.execute reports the first failure but continues other writes",
          "[command][error]") {
    test::MockTransport mock;
    module::Command::Config cfg;
    cfg.moduleId = "cmd";
    cfg.writes = {
        {core::RegisterTable::HoldingRegister, 1, 1},
        {core::RegisterTable::HoldingRegister, 2, 2},
        {core::RegisterTable::HoldingRegister, 3, 3},
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
        {core::RegisterTable::HoldingRegister, 400, 0xDEAD},
        {core::RegisterTable::HoldingRegister, 401, 0xBEEF},
    };
    module::Command cmd(cfg, mock);

    cmd.executeAsync();   // sync mock async → serialised writes complete inline

    auto const writes = mock.capturedWrites();
    REQUIRE(writes.size() == 2);
    REQUIRE(writes[0].startAddress == 400);
    REQUIRE(writes[0].values == core::RegisterWords{0xDEAD});
    REQUIRE(writes[1].startAddress == 401);
    REQUIRE(writes[1].values == core::RegisterWords{0xBEEF});

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


TEST_CASE("Command async feedback preserves errors and reports rejection", "[command][async][feedback]") {
    test::MockTransport mock;
    std::vector<bus::WriteCompleted> results;
    module::Command::Config cfg;
    cfg.moduleId = "cmd.feedback";
    cfg.writes = {{RegisterTable::HoldingRegister, 7, 42}};
    cfg.onResult = [&](auto const& result) { results.push_back(result); };
    module::Command cmd(cfg, mock);
    mock.enqueueWriteResult({false, "peer write timeout"});
    cmd.executeAsync();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].kind == sched::ResultKind::Error);
    REQUIRE(results[0].errorMessage == "peer write timeout");
    REQUIRE(results[0].batch.startAddress == 7);
    cmd.executeAsync();
    REQUIRE(results.size() == 2);
    REQUIRE(results[1].kind == sched::ResultKind::Ok);
    mock.scheduler().stopAsync();
    cmd.executeAsync();
    REQUIRE(results.size() == 3);
    REQUIRE(results[2].kind == sched::ResultKind::Cancelled);
    REQUIRE(mock.capturedWrites().size() == 2);
}

TEST_CASE("Command replaces only queued writes to the same target across modules", "[command][async][coalesce]") {
    test::MockTransport mock;
    mock.setDeferAsync(true);
    std::vector<bus::WriteCompleted> results;
    module::Command::Config cfg;
    cfg.moduleId = "first";
    cfg.writes = {{RegisterTable::HoldingRegister, 7, 1}};
    cfg.onResult = [&](auto const& r) { results.push_back(r); };
    module::Command first(cfg, mock);
    first.executeAsync(); // already dispatched: cannot be replaced
    cfg.moduleId = "old";
    cfg.writes[0].value = 2;
    module::Command old(cfg, mock);
    old.executeAsync();
    cfg.moduleId = "latest";
    cfg.writes[0].value = 3;
    module::Command latest(cfg, mock);
    latest.executeAsync();
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].moduleId == "old");
    REQUIRE(results[0].kind == sched::ResultKind::Superseded);
    REQUIRE(mock.capturedWrites().size() == 1);
    REQUIRE(mock.completeNextWrite());
    REQUIRE(mock.capturedWrites().size() == 2);
    REQUIRE(mock.capturedWrites()[1].values[0] == 3);
    REQUIRE(mock.completeNextWrite());
    REQUIRE(results.size() == 3);
    REQUIRE(results[1].moduleId == "first");
    REQUIRE(results[2].moduleId == "latest");
}
