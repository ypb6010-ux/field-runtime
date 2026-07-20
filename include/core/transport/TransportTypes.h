// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QList>
#include <QDateTime>
#include <QString>
#include <QtSerialBus/QModbusDataUnit>

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

struct EndpointInfo {
    QString address;
    quint16 port = 0;

    friend bool operator==(EndpointInfo const&, EndpointInfo const&) = default;
};

// Stable value snapshots exposed by Transport/ICore. They intentionally do
// not expose QTcpSocket/QModbusDevice or any other thread-affine QObject.
struct TransportStatus {
    QString          transportId;
    TransportKind    kind = TransportKind::ModbusTcpClient;
    ConnectionState  state = ConnectionState::Disconnected;
    EndpointInfo     localEndpoint;
    EndpointInfo     remoteEndpoint;
    QString          errorMessage;
    QDateTime        changedAt;
    quint64          revision = 0;

    friend bool operator==(TransportStatus const&, TransportStatus const&) = default;
};

struct PeerSession {
    QString       transportId;
    QString       sessionId;
    EndpointInfo  localEndpoint;
    EndpointInfo  remoteEndpoint;
    QDateTime     connectedAt;

    friend bool operator==(PeerSession const&, PeerSession const&) = default;
};

struct ReadRequest {
    QModbusDataUnit::RegisterType table;
    int                           startAddress = 0;
    int                           count        = 0;
};

struct WriteBatch {
    QModbusDataUnit::RegisterType table;
    int                           startAddress = 0;
    QList<quint16>                values;
};

struct ReadResult {
    bool            ok = false;
    QList<quint16>  values;
    int             startAddress = 0;
    QString         errorMessage;
};

struct WriteResult {
    bool     ok = false;
    QString  errorMessage;
};

struct WatchRange {
    QModbusDataUnit::RegisterType table;
    int  startAddress = 0;
    int  size         = 0;
};

} // namespace core::transport
