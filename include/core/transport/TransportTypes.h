// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/base/RegisterTable.h"
#include "core/core_global.h"
#include "core/dp/State.h"

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

struct EndpointInfo {
    std::string address;
    std::uint16_t port = 0;

    friend bool operator==(EndpointInfo const&,
                           EndpointInfo const&) = default;
};

// Stable, Qt-free value snapshots. They never expose a socket, device handle,
// QObject, or any other thread-affine implementation detail.
struct TransportStatus {
    std::string      transportId;
    TransportKind    kind = TransportKind::ModbusTcpClient;
    ConnectionState  state = ConnectionState::Disconnected;
    EndpointInfo     localEndpoint;
    EndpointInfo     remoteEndpoint;
    std::string      errorMessage;
    dp::Timestamp    changedAt{};
    std::uint64_t    revision = 0;

    friend bool operator==(TransportStatus const&,
                           TransportStatus const&) = default;
};

struct PeerSession {
    std::string    transportId;
    std::string    sessionId;
    EndpointInfo   localEndpoint;
    EndpointInfo   remoteEndpoint;
    dp::Timestamp  connectedAt{};

    friend bool operator==(PeerSession const&, PeerSession const&) = default;
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
