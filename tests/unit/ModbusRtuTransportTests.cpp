// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/transport/ModbusRtuTransport.h"

using namespace core::transport;

TEST_CASE("ModbusRtuTransport surfaces config-level id/kind/state",
          "[transport][rtu]") {
    ModbusRtuTransport::Config cfg;
    cfg.id        = "rtu1";
    cfg.portName  = "COM_BOGUS_FOR_TEST";   // no real port — connect will fail
    cfg.baudRate  = 9600;
    cfg.slaveId   = 1;
    cfg.connectTimeoutMs = 200;
    cfg.requestTimeoutMs = 200;

    ModbusRtuTransport t(cfg);
    REQUIRE(t.id()   == "rtu1");
    REQUIRE(t.kind() == TransportKind::ModbusRtu);
    REQUIRE(t.state() == ConnectionState::Disconnected);
}

TEST_CASE("ModbusRtuTransport.connect on a non-existent port returns an error",
          "[transport][rtu]") {
    ModbusRtuTransport::Config cfg;
    cfg.id        = "rtu_missing";
    cfg.portName  = "COM_DOES_NOT_EXIST_99";
    cfg.baudRate  = 9600;
    cfg.connectTimeoutMs = 200;
    cfg.requestTimeoutMs = 200;
    ModbusRtuTransport t(cfg);

    auto r = t.connect();
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("ModbusRtuTransport read/write on a disconnected port report "
          "'not connected'", "[transport][rtu]") {
    ModbusRtuTransport::Config cfg;
    cfg.id       = "rtu_idle";
    cfg.portName = "COM_DOES_NOT_EXIST_99";
    cfg.baudRate = 9600;
    ModbusRtuTransport t(cfg);

    auto rd = t.read({QModbusDataUnit::HoldingRegisters, 0, 4});
    REQUIRE_FALSE(rd.ok);
    REQUIRE(rd.errorMessage.contains("not connected"));

    auto wr = t.writeBatch({QModbusDataUnit::HoldingRegisters, 0, {0x1234}});
    REQUIRE_FALSE(wr.ok);
    REQUIRE(wr.errorMessage.contains("not connected"));
}
