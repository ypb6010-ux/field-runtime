#include <catch2/catch_test_macros.hpp>

#include <chrono>

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
    auto r = t.read({QModbusDataUnit::HoldingRegisters, 0, 4});
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

    srv.setData(QModbusDataUnit::HoldingRegisters,  0, 0x1234);
    srv.setData(QModbusDataUnit::HoldingRegisters,  1, 0x5678);
    srv.setData(QModbusDataUnit::HoldingRegisters,  2, 42);
    srv.setData(QModbusDataUnit::HoldingRegisters,  3, 9999);

    ModbusTcpClientTransport t(cfgFor(port));
    auto connected = t.connect();
    REQUIRE(connected.has_value());
    REQUIRE(t.state() == ConnectionState::Connected);

    auto r = t.read({QModbusDataUnit::HoldingRegisters, 0, 4});
    REQUIRE(r.ok);
    REQUIRE(r.values == QList<quint16>{0x1234, 0x5678, 42, 9999});

    auto w = t.writeBatch({QModbusDataUnit::HoldingRegisters, 10, {0xAAAA, 0xBBBB}});
    REQUIRE(w.ok);
    REQUIRE(srv.getData(QModbusDataUnit::HoldingRegisters, 10) == 0xAAAA);
    REQUIRE(srv.getData(QModbusDataUnit::HoldingRegisters, 11) == 0xBBBB);

    t.disconnect();
    REQUIRE(t.state() == ConnectionState::Disconnected);
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
        srv.setData(QModbusDataUnit::HoldingRegisters, i, quint16(0x100 + i));
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
                    auto rr = t.read({QModbusDataUnit::HoldingRegisters, 0, 8});
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
