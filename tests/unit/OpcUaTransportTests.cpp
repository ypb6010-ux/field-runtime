#include <catch2/catch_test_macros.hpp>

#include "core/transport/OpcUaClientTransport.h"

using namespace core::transport;

TEST_CASE("OpcUaClientTransport surfaces config id/kind/state",
          "[transport][opcua]") {
    OpcUaClientTransport::Config cfg;
    cfg.id          = "opcua_test";
    cfg.endpointUrl = "opc.tcp://127.0.0.1:65535";  // nothing should listen here
    cfg.connectTimeoutMs = 300;
    cfg.requestTimeoutMs = 300;
    OpcUaClientTransport t(cfg);
    REQUIRE(t.id()   == "opcua_test");
    REQUIRE(t.kind() == TransportKind::OpcUaClient);
    REQUIRE(t.state() == ConnectionState::Disconnected);
}

TEST_CASE("OpcUaClientTransport.connect on an unreachable endpoint fails",
          "[transport][opcua]") {
    OpcUaClientTransport::Config cfg;
    cfg.id          = "opcua_unreachable";
    cfg.endpointUrl = "opc.tcp://127.0.0.1:65535";
    cfg.connectTimeoutMs = 300;
    cfg.requestTimeoutMs = 300;
    OpcUaClientTransport t(cfg);

    auto r = t.connect();
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("OpcUaClientTransport.read/writeBatch on disconnected report "
          "'not connected'", "[transport][opcua]") {
    OpcUaClientTransport::Config cfg;
    cfg.id          = "opcua_idle";
    cfg.endpointUrl = "opc.tcp://127.0.0.1:65535";
    cfg.connectTimeoutMs = 200;
    cfg.requestTimeoutMs = 200;
    OpcUaClientTransport t(cfg);

    auto rd = t.read({QModbusDataUnit::HoldingRegisters, 0, 4});
    REQUIRE_FALSE(rd.ok);

    auto wr = t.writeBatch({QModbusDataUnit::HoldingRegisters, 0, {0x1234}});
    REQUIRE_FALSE(wr.ok);
}
