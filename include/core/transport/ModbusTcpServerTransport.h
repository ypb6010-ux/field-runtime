// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QList>
#include <QString>

#include "core/core_global.h"
#include "core/transport/Transport.h"

namespace core::bus { class EventBus; }

namespace core::transport {

// ModbusTcpServerTransport — Core acts as the Modbus slave to which operator
// boxes connect. Owns a QModbusTcpServer on a dedicated worker thread; on
// every client write the server publishes a `bus::ServerWriteEvent` to the
// shared EventBus so the router / SinkWindow layer can translate operator
// intent into PLC-side writes.
//
// `read` / `writeBatch` operate on the *server's own* data table — used by
// the router to mirror PLC state back into the registers operator boxes
// poll, and by tests to verify state.
class CORE_EXPORT ModbusTcpServerTransport : public Transport {
public:
    struct Config {
        QString  id              = QStringLiteral("modbus-tcp-server");
        QString  listenAddress   = QStringLiteral("0.0.0.0");
        quint16  listenPort      = 502;
        int      slaveId         = 1;
        int      connectTimeoutMs = 3000;
        // > 0 = re-listen after this many ms once state drops to
        // Disconnected/Error.
        int      reconnectIntervalMs = 0;
        QList<WatchRange> listenRanges;
        sched::SchedulerConfig scheduler = sched::SchedulerConfig{};
    };

    ModbusTcpServerTransport(Config cfg, bus::EventBus& bus);
    ~ModbusTcpServerTransport() override;

    CORE_DISABLE_COPY_MOVE(ModbusTcpServerTransport)

    QString               id()    const override;
    TransportKind         kind()  const override;
    ConnectionState       state() const override;
    TransportStatus       status() const override;
    QList<PeerSession>    peerSessions() const override;

    std::expected<void, QString> connect()    override;
    void                          disconnect() override;

    sched::RequestScheduler& scheduler() override;

    ReadResult  read      (ReadRequest const& req)         override;
    WriteResult writeBatch(WriteBatch  const& batch)       override;
    void        readAsync (ReadRequest const& req, ReadDone done) override;
    void        writeAsync(WriteBatch const& batch, WriteDone done) override;

private:
    void armReconnectIfConfigured();

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::transport
