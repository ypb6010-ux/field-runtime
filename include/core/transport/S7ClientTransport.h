// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// S7ClientTransport — Siemens S7 (S7-200 / 300 / 400 / 1200 / 1500) over
// ISO-on-TCP. Reuses Modbus's ReadRequest / WriteBatch shape: table is
// remapped to S7 area (HoldingRegisters → DB, Coils → M, InputRegisters → I).
// Address is interpreted as DB number for DB area, byte offset otherwise.
//
// Placeholder until upstream `snap7` (v1.4.2, LGPL) is vendored.
class CORE_EXPORT S7ClientTransport : public Transport {
public:
    struct Config {
        std::string  id          = "s7-1";
        std::string  host        = "192.168.0.1";
        int      port        = 102;        // ISO-on-TCP default
        int      rack        = 0;
        int      slot        = 1;
        int      connectTimeoutMs    = 5000;
        int      requestTimeoutMs    = 2000;
        int      reconnectIntervalMs = 5000;
        sched::SchedulerConfig scheduler = sched::SchedulerConfig{};  // serial
    };

    explicit S7ClientTransport(Config cfg, bus::EventBus* bus = nullptr);
    ~S7ClientTransport() override;

    CORE_DISABLE_COPY_MOVE(S7ClientTransport)

    std::string           id()    const override;
    TransportKind         kind()  const override;
    ConnectionState       state() const override;
    TransportStatus       status() const override;

    std::expected<void, std::string> connect()    override;
    void                             disconnect() override;

    sched::RequestScheduler& scheduler() override;

    ReadResult  read      (ReadRequest const& req)         override;
    WriteResult writeBatch(WriteBatch  const& batch)       override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
