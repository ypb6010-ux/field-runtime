// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/transport/ModbusTcpClientTransport.h"
#include "mocks/ModbusTestServer.h"

using namespace core::transport;
using namespace std::chrono_literals;

namespace {

// Each test uses a slightly different port to avoid cross-test interference
// on slow OS port reclaim.
quint16 nextPort() {
    static quint16 port = 51500;
    return ++port;
}

ModbusTcpClientTransport::Config cfgFor(quint16 port,
                                         QString host = "127.0.0.1") {
    ModbusTcpClientTransport::Config c;
    c.id               = QStringLiteral("test.tcp");
    c.host             = std::move(host);
    c.port             = port;
    c.slaveId          = 1;
    c.connectTimeoutMs = 500;
    c.requestTimeoutMs = 500;
    return c;
}

} // namespace

TEST_CASE("ModbusTcpClientTransport reports kind and id from config",
          "[transport][modbus]") {
    ModbusTcpClientTransport t(cfgFor(nextPort()));
    REQUIRE(t.id()    == "test.tcp");
    REQUIRE(t.kind()  == TransportKind::ModbusTcpClient);
    REQUIRE(t.state() == ConnectionState::Disconnected);
}

TEST_CASE("Read on a disconnected transport returns 'not connected'",
          "[transport][modbus][disconnected]") {
    ModbusTcpClientTransport t(cfgFor(nextPort()));
    auto r = t.read({core::RegisterTable::HoldingRegister, 0, 4});
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.errorMessage.contains("not connected"));
}

TEST_CASE("Connect to an unreachable host fails within the timeout",
          "[transport][modbus][unreachable]") {
    auto cfg = cfgFor(nextPort());
    cfg.host = "127.0.0.1";   // nothing listening on the random port
    cfg.connectTimeoutMs = 300;
    ModbusTcpClientTransport t(std::move(cfg));

    auto result = t.connect();
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Transport reads and writes against a real QModbusTcpServer",
          "[transport][modbus][integration]") {
    auto port = nextPort();
    core::test::ModbusTestServer srv(port);
    REQUIRE(srv.listening());

    srv.setData(core::RegisterTable::HoldingRegister,  0, 0x1234);
    srv.setData(core::RegisterTable::HoldingRegister,  1, 0x5678);
    srv.setData(core::RegisterTable::HoldingRegister,  2, 42);
    srv.setData(core::RegisterTable::HoldingRegister,  3, 9999);

    ModbusTcpClientTransport t(cfgFor(port));
    auto connected = t.connect();
    REQUIRE(connected.has_value());
    REQUIRE(t.state() == ConnectionState::Connected);

    auto r = t.read({core::RegisterTable::HoldingRegister, 0, 4});
    REQUIRE(r.ok);
    REQUIRE(r.values == QList<quint16>{0x1234, 0x5678, 42, 9999});

    auto w = t.writeBatch({core::RegisterTable::HoldingRegister, 10, {0xAAAA, 0xBBBB}});
    REQUIRE(w.ok);
    REQUIRE(srv.getData(core::RegisterTable::HoldingRegister, 10) == 0xAAAA);
    REQUIRE(srv.getData(core::RegisterTable::HoldingRegister, 11) == 0xBBBB);

    t.disconnect();
    REQUIRE(t.state() == ConnectionState::Disconnected);
}

TEST_CASE("readAsync/writeAsync are non-blocking and deliver the reply",
          "[transport][modbus][integration][async]") {
    auto port = nextPort();
    core::test::ModbusTestServer srv(port);
    REQUIRE(srv.listening());
    srv.setData(core::RegisterTable::HoldingRegister, 0, 0x1111);
    srv.setData(core::RegisterTable::HoldingRegister, 1, 0x2222);

    ModbusTcpClientTransport t(cfgFor(port));
    REQUIRE(t.connect().has_value());

    auto waitFor = [](std::atomic<bool>& flag) {
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (!flag.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        return flag.load();
    };

    std::atomic<bool> readDone{false};
    ReadResult rr;
    t.readAsync({core::RegisterTable::HoldingRegister, 0, 2},
                [&](ReadResult r) { rr = std::move(r); readDone.store(true); });
    // readAsync returned without blocking on the reply; result arrives later.
    REQUIRE(waitFor(readDone));
    REQUIRE(rr.ok);
    REQUIRE(rr.values == QList<quint16>{0x1111, 0x2222});

    std::atomic<bool> writeDone{false};
    WriteResult wres;
    t.writeAsync({core::RegisterTable::HoldingRegister, 5, {0xABCD}},
                 [&](WriteResult w) { wres = std::move(w); writeDone.store(true); });
    REQUIRE(waitFor(writeDone));
    REQUIRE(wres.ok);
    REQUIRE(srv.getData(core::RegisterTable::HoldingRegister, 5) == 0xABCD);

    t.disconnect();
}

TEST_CASE("readAsync on a disconnected transport reports 'not connected'",
          "[transport][modbus][async][disconnected]") {
    ModbusTcpClientTransport t(cfgFor(nextPort()));
    std::atomic<bool> done{false};
    ReadResult rr;
    t.readAsync({core::RegisterTable::HoldingRegister, 0, 4},
                [&](ReadResult r) { rr = std::move(r); done.store(true); });
    REQUIRE(done.load());   // synchronous fast-fail path, no event loop needed
    REQUIRE_FALSE(rr.ok);
    REQUIRE(rr.errorMessage.contains("not connected"));
}

TEST_CASE("Transport.scheduler() returns a usable SerialScheduler instance",
          "[transport][modbus][scheduler]") {
    ModbusTcpClientTransport t(cfgFor(nextPort()));
    auto& s = t.scheduler();

    bool ran = false;
    core::sched::RequestTag tag;
    tag.moduleId = "probe";
    auto r = s.submit(tag, [&] { ran = true; });
    REQUIRE(r.kind == core::sched::ResultKind::Ok);
    REQUIRE(ran);
}

TEST_CASE("Scheduler serialises concurrent reads against a real server",
          "[transport][modbus][integration][concurrent]") {
    auto port = nextPort();
    core::test::ModbusTestServer srv(port);
    REQUIRE(srv.listening());
    for (int i = 0; i < 8; ++i) {
        srv.setData(core::RegisterTable::HoldingRegister, i, quint16(0x100 + i));
    }

    ModbusTcpClientTransport t(cfgFor(port));
    REQUIRE(t.connect().has_value());
    auto& sched = t.scheduler();

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> ok{0};

    constexpr int kThreads = 4;
    constexpr int kPer     = 5;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            for (int j = 0; j < kPer; ++j) {
                core::sched::RequestTag tag;
                tag.moduleId = QString::number(i);
                auto r = sched.submit(tag, [&] {
                    int cur = active.fetch_add(1) + 1;
                    int prev = peak.load();
                    while (cur > prev
                        && !peak.compare_exchange_weak(prev, cur)) {}
                    auto rr = t.read({core::RegisterTable::HoldingRegister, 0, 8});
                    if (rr.ok && rr.values.size() == 8) ok.fetch_add(1);
                    active.fetch_sub(1);
                });
                REQUIRE(r.kind == core::sched::ResultKind::Ok);
            }
        });
    }
    for (auto& th : ts) th.join();

    REQUIRE(peak.load() == 1);            // serial scheduler upheld
    REQUIRE(ok.load() == kThreads * kPer);
}
