// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// OpcUaClientTransport — OPC UA client transport (full-duplex). Pairs with
// `CreditScheduler` since OPC UA supports concurrent in-flight requests.
//
// Placeholder until upstream `open62541` (v1.4.x, MIT) is vendored. The
// header / Config / lifecycle are stable; calling `connect()` on the stub
// returns "library not vendored" so apps can compile-test their wiring
// without the library installed.
class CORE_EXPORT OpcUaClientTransport : public Transport {
public:
    struct Config {
        QString  id              = QStringLiteral("opcua-1");
        QString  endpointUrl     = QStringLiteral("opc.tcp://127.0.0.1:4840");
        QString  securityPolicy  = QStringLiteral("None");   // None / Basic256Sha256
        QString  username;
        QString  password;
        // Open62541 backend name; `open62541` is the open-source default
        // bundled with Qt OPC UA.
        QString  backend         = QStringLiteral("open62541");
        // Substituted with the register address when mapping Modbus-style
        // ReadRequest / WriteBatch into OPC UA nodes. Use Qt's QString::arg
        // placeholder, e.g. "ns=2;s=Var_%1".
        QString  nodeIdTemplate  = QStringLiteral("ns=2;s=Var_%1");
        int      connectTimeoutMs    = 5000;
        int      requestTimeoutMs    = 2000;
        int      reconnectIntervalMs = 0;
        // OPC UA supports parallel inflight; default to Credit scheduler.
        sched::SchedulerConfig scheduler = []{
            sched::SchedulerConfig s;
            s.kind        = sched::SchedulerKind::Credit;
            s.maxInflight = 8;
            return s;
        }();
    };

    explicit OpcUaClientTransport(Config cfg, bus::EventBus* bus = nullptr);
    ~OpcUaClientTransport() override;

    CORE_DISABLE_COPY_MOVE(OpcUaClientTransport)

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
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
