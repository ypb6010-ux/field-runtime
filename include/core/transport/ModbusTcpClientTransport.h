// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// ModbusTcpClientTransport — concrete Transport backed by Qt's
// QModbusTcpClient. The client lives on a dedicated worker QThread; all I/O
// is initiated via QMetaObject::invokeMethod and serialised through the
// owning SerialScheduler.
//
// Phase 1 surface:
//   - synchronous `read` / `writeBatch` (block the calling thread until the
//     underlying QModbusReply emits finished)
//   - `connect()` blocks until the connection reaches Connected or the
//     timeout expires.
//   - `disconnect()` blocks until the client returns to Unconnected.
//   - `state()` reports the latest QModbusDevice::State.
//
// Half-duplex devices (485 → Ethernet gateways) are handled by the bundled
// SerialScheduler; no extra effort required at this layer.
class CORE_EXPORT ModbusTcpClientTransport : public Transport {
public:
    struct Config {
        std::string   id            = "modbus-tcp";
        std::string   host          = "127.0.0.1";
        std::uint16_t port          = 502;
        int      slaveId       = 1;
        int      connectTimeoutMs = 3000;
        int      requestTimeoutMs = 1000;
        // > 0 = auto-reconnect after this many ms once state drops to
        // Disconnected/Error. 0 disables auto-reconnect.
        int      reconnectIntervalMs = 0;
        // Scheduler configuration passed to the internal SerialScheduler.
        sched::SchedulerConfig scheduler = sched::SchedulerConfig{};
    };

    explicit ModbusTcpClientTransport(Config cfg, bus::EventBus* bus = nullptr);
    ~ModbusTcpClientTransport() override;

    CORE_DISABLE_COPY_MOVE(ModbusTcpClientTransport)

    std::string           id()    const override;
    TransportKind         kind()  const override;
    ConnectionState       state() const override;
    TransportStatus       status() const override;

    std::expected<void, std::string> connect()    override;
    void                             disconnect() override;

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
