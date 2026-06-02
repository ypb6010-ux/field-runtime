#pragma once

#include <QList>
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
    MqttClient,
    S7Client,
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
