// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

#include "core/base/RegisterTable.h"
#include "core/core_global.h"

namespace core::transport {

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error,
};

enum class TransportKind {
    ModbusTcpClient,
    ModbusTcpServer,
    ModbusRtu,
    OpcUaClient,
    MqttClient,       // Qt6::Mqtt backend
    MqttPahoClient,   // paho.mqtt.cpp backend
    S7Client,
};

struct ReadRequest {
    core::RegisterTable           table = core::RegisterTable::HoldingRegister;
    int                           startAddress = 0;
    int                           count        = 0;
};

struct WriteBatch {
    core::RegisterTable           table = core::RegisterTable::HoldingRegister;
    int                           startAddress = 0;
    core::RegisterWords                values;
};

struct ReadResult {
    bool            ok = false;
    core::RegisterWords  values;
    int             startAddress = 0;
    std::string     errorMessage;
};

struct WriteResult {
    bool     ok = false;
    std::string  errorMessage;
};

struct WatchRange {
    core::RegisterTable table = core::RegisterTable::HoldingRegister;
    int  startAddress = 0;
    int  size         = 0;
};

} // namespace core::transport
