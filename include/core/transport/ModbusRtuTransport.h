// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// ModbusRtuTransport — Modbus RTU client over a serial port (RS-232 / RS-485).
// Wraps Qt's `QModbusRtuSerialMaster` on a dedicated worker QThread; all I/O
// is serialised through the internal SerialScheduler. The default scheduler
// inter-request gap of 5–10 ms is recommended for RS-485 bus turnaround.
//
// Auto-reconnect mirrors ModbusTcpClientTransport: when configured with
// `reconnectIntervalMs > 0`, the transport republishes connect attempts on
// the worker thread until the serial port reopens successfully.
class CORE_EXPORT ModbusRtuTransport : public Transport {
public:
    enum class Parity { None, Even, Odd };

    struct Config {
        QString  id            = QStringLiteral("modbus-rtu");
        QString  portName      = QStringLiteral("COM1");
        int      baudRate      = 9600;
        int      dataBits      = 8;
        int      stopBits      = 1;
        Parity   parity        = Parity::None;
        int      slaveId       = 1;
        int      connectTimeoutMs    = 3000;
        int      requestTimeoutMs    = 1000;
        int      reconnectIntervalMs = 0;
        sched::SchedulerConfig scheduler = sched::SchedulerConfig{};
    };

    explicit ModbusRtuTransport(Config cfg, bus::EventBus* bus = nullptr);
    ~ModbusRtuTransport() override;

    CORE_DISABLE_COPY_MOVE(ModbusRtuTransport)

    QString               id()    const override;
    TransportKind         kind()  const override;
    ConnectionState       state() const override;

    std::expected<void, QString> connect()    override;
    void                          disconnect() override;

    sched::RequestScheduler& scheduler() override;

    ReadResult  read      (ReadRequest const& req)         override;
    WriteResult writeBatch(WriteBatch  const& batch)       override;

    void readAsync (ReadRequest const& req,   ReadDone  done) override;
    void writeAsync(WriteBatch  const& batch, WriteDone done) override;

private:
    void armReconnectIfConfigured();

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
