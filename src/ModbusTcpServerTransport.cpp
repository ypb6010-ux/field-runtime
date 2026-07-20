// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/ModbusTcpServerTransport.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <functional>

#include <QHash>
#include <QMetaObject>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtSerialBus/QModbusDataUnitMap>
#include <QtSerialBus/QModbusTcpConnectionObserver>
#include <QtSerialBus/QModbusTcpServer>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

#include "QtThreadInvoke.h"
#include "TransportStatusTracker.h"

namespace core::transport {

namespace {

ConnectionState stateFromQt(QModbusDevice::State s) {
    switch (s) {
        case QModbusDevice::UnconnectedState: return ConnectionState::Disconnected;
        case QModbusDevice::ConnectingState:  return ConnectionState::Connecting;
        case QModbusDevice::ConnectedState:   return ConnectionState::Connected;
        case QModbusDevice::ClosingState:     return ConnectionState::Disconnected;
    }
    return ConnectionState::Disconnected;
}

class ConnectionObserver final : public QModbusTcpConnectionObserver {
public:
    explicit ConnectionObserver(std::function<bool(QTcpSocket*)> callback)
        : m_callback(std::move(callback)) {}

    bool acceptNewConnection(QTcpSocket* client) override {
        return m_callback ? m_callback(client) : true;
    }

private:
    std::function<bool(QTcpSocket*)> m_callback;
};

} // namespace

class ModbusTcpServerTransport::Impl {
public:
    Impl(Config c, bus::EventBus& busRef)
        : cfg(std::move(c))
        , busPtr(&busRef)
        , statusTracker(cfg.id, TransportKind::ModbusTcpServer, &busRef,
                        EndpointInfo{cfg.listenAddress, cfg.listenPort}, {})
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread)
        , server(new QModbusTcpServer) {

        server->moveToThread(thread);
        thread->start();

        detail::invokeBlocking(server, [this] {
            server->installConnectionObserver(new ConnectionObserver(
                [this](QTcpSocket* socket) {
                    addPeer(socket);
                    return true;
                }));
        });
        QObject::connect(server, &QModbusTcpServer::modbusClientDisconnected,
                         server, [this](QTcpSocket* socket) {
            removePeer(socket, QStringLiteral("peer disconnected"));
        });

        QObject::connect(server, &QModbusDevice::stateChanged,
                         server, [this](QModbusDevice::State s) {
            auto const cur  = stateFromQt(s);
            if (cur == ConnectionState::Disconnected) {
                drainPeers(QStringLiteral("server stopped"));
            }
            if (cur == ConnectionState::Connected) setLastError({});
            auto const error = getLastError();
            auto const effective = cur == ConnectionState::Disconnected
                                && !error.isEmpty()
                ? ConnectionState::Error : cur;
            state.store(effective, std::memory_order_release);
            statusTracker.update(effective,
                effective == ConnectionState::Error ? error : QString{});
        });
        QObject::connect(server, &QModbusDevice::errorOccurred,
                         server, [this](QModbusDevice::Error) {
            auto const message = server->errorString();
            setLastError(message);
            drainPeers(message);
            state.store(ConnectionState::Error, std::memory_order_release);
            statusTracker.update(ConnectionState::Error, message);
        });
        QObject::connect(server, &QModbusTcpServer::dataWritten,
                         server, [this](QModbusDataUnit::RegisterType table,
                                         int address, int size) {
            QList<quint16> values;
            values.reserve(size);
            for (int i = 0; i < size; ++i) {
                quint16 v = 0;
                server->data(table, address + i, &v);
                values.append(v);
            }
            busPtr->publish(bus::ServerWriteEvent{
                cfg.id, table, address, std::move(values)});
        });
    }

    ~Impl() {
        autoReconnect.store(false, std::memory_order_release);
        if (scheduler) scheduler->stopAsync();
        if (reconnectTimer) {
            detail::invokeBlocking(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            });
        }
        if (server) {
            detail::invokeBlocking(server, [this] {
                drainPeers(QStringLiteral("server destroyed"));
                server->disconnectDevice();
                delete server;
                server = nullptr;
            });
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    void setLastError(QString message) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    QString getLastError() const {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    void addPeer(QTcpSocket* socket) {
        if (!socket) return;
        PeerSession session;
        session.transportId = cfg.id;
        session.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.localEndpoint = EndpointInfo{
            socket->localAddress().toString(), socket->localPort()};
        session.remoteEndpoint = EndpointInfo{
            socket->peerAddress().toString(), socket->peerPort()};
        session.connectedAt = QDateTime::currentDateTimeUtc();
        {
            std::lock_guard lock(peersMutex);
            peers.insert(socket, session);
        }
        if (busPtr) {
            busPtr->publish(bus::PeerSessionChanged{
                bus::PeerSessionChangeKind::Connected, session, {},
                QDateTime::currentDateTimeUtc()});
        }
    }

    void removePeer(QTcpSocket* socket, QString reason) {
        PeerSession session;
        bool found = false;
        {
            std::lock_guard lock(peersMutex);
            auto const it = peers.find(socket);
            if (it != peers.end()) {
                session = it.value();
                peers.erase(it);
                found = true;
            }
        }
        if (found && busPtr) {
            busPtr->publish(bus::PeerSessionChanged{
                bus::PeerSessionChangeKind::Disconnected, session,
                std::move(reason), QDateTime::currentDateTimeUtc()});
        }
    }

    void drainPeers(QString const& reason) {
        QList<PeerSession> removed;
        {
            std::lock_guard lock(peersMutex);
            removed.reserve(peers.size());
            for (auto it = peers.cbegin(); it != peers.cend(); ++it) {
                removed.append(it.value());
            }
            peers.clear();
        }
        if (!busPtr) return;
        for (auto const& session : removed) {
            busPtr->publish(bus::PeerSessionChanged{
                bus::PeerSessionChangeKind::Disconnected, session, reason,
                QDateTime::currentDateTimeUtc()});
        }
    }

    QList<PeerSession> peerSnapshot() const {
        std::lock_guard lock(peersMutex);
        QList<PeerSession> out;
        out.reserve(peers.size());
        for (auto it = peers.cbegin(); it != peers.cend(); ++it) {
            out.append(it.value());
        }
        return out;
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr;
    detail::TransportStatusTracker                    statusTracker;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QModbusTcpServer*                                  server = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    QString                                            lastError;
    mutable std::mutex                                 peersMutex;
    QHash<QTcpSocket*, PeerSession>                    peers;
};

ModbusTcpServerTransport::ModbusTcpServerTransport(Config cfg, bus::EventBus& bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusTcpServerTransport::~ModbusTcpServerTransport() = default;

QString               ModbusTcpServerTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusTcpServerTransport::kind()  const { return TransportKind::ModbusTcpServer; }
ConnectionState       ModbusTcpServerTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }
TransportStatus       ModbusTcpServerTransport::status() const { return m_impl->statusTracker.snapshot(); }
QList<PeerSession>    ModbusTcpServerTransport::peerSessions() const { return m_impl->peerSnapshot(); }

sched::RequestScheduler& ModbusTcpServerTransport::scheduler() { return *m_impl->scheduler; }

namespace {

// Apply the configured listen ranges and connection parameters to `server`.
// Called on the server's own thread inside an invokeMethod hop.
bool applyListenConfig(QModbusTcpServer*                       server,
                       ModbusTcpServerTransport::Config const&  cfg) {
    QHash<QModbusDataUnit::RegisterType, QPair<int, int>> bounds;
    for (auto const& r : cfg.listenRanges) {
        int const start = r.startAddress;
        int const end   = r.startAddress + r.size - 1;
        if (bounds.contains(r.table)) {
            auto cur = bounds.value(r.table);
            cur.first  = std::min(cur.first,  start);
            cur.second = std::max(cur.second, end);
            bounds[r.table] = cur;
        } else {
            bounds.insert(r.table, qMakePair(start, end));
        }
    }
    QModbusDataUnitMap map;
    for (auto it = bounds.constBegin(); it != bounds.constEnd(); ++it) {
        int const count = it.value().second - it.value().first + 1;
        map.insert(it.key(),
            QModbusDataUnit(it.key(), it.value().first, count));
    }
    if (!server->setMap(map)) return false;
    server->setServerAddress(cfg.slaveId);
    server->setConnectionParameter(
        QModbusDevice::NetworkAddressParameter, cfg.listenAddress);
    server->setConnectionParameter(
        QModbusDevice::NetworkPortParameter, cfg.listenPort);
    return server->connectDevice();
}

} // namespace

std::expected<void, QString>
ModbusTcpServerTransport::connect() {
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }
    m_impl->setLastError({});
    m_impl->state.store(ConnectionState::Connecting,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Connecting);
    bool ok = false;
    detail::invokeBlocking(m_impl->server, [this, &ok] {
        ok = applyListenConfig(m_impl->server, m_impl->cfg);
        if (!ok) m_impl->setLastError(m_impl->server->errorString());
    });
    if (!ok) {
        armReconnectIfConfigured();
        auto const error = m_impl->getLastError();
        auto const message = error.isEmpty()
            ? QStringLiteral("connectDevice() returned false")
            : error;
        m_impl->state.store(ConnectionState::Error, std::memory_order_release);
        m_impl->statusTracker.update(ConnectionState::Error, message);
        return std::unexpected(message);
    }

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) {
            armReconnectIfConfigured();
            return {};
        }
        if (s == ConnectionState::Error) {
            armReconnectIfConfigured();
            return std::unexpected(m_impl->getLastError());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    armReconnectIfConfigured();
    auto const error = m_impl->getLastError();
    auto const message = error.isEmpty()
        ? QStringLiteral("listen timeout") : error;
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}

void ModbusTcpServerTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    m_impl->setLastError({});
    detail::invokeBlocking(m_impl->server, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->drainPeers(QStringLiteral("server stopped"));
        m_impl->server->disconnectDevice();
    });
    m_impl->state.store(ConnectionState::Disconnected, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}

void ModbusTcpServerTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.load(std::memory_order_acquire)) return;
    m_impl->autoReconnect.store(true, std::memory_order_release);

    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;

    detail::invokeBlocking(m_impl->server, [impl, intervalMs] {
        if (impl->reconnectTimer) {
            if (!impl->reconnectTimer->isActive()) impl->reconnectTimer->start();
            return;
        }
        impl->reconnectTimer = new QTimer(impl->server);
        impl->reconnectTimer->setInterval(intervalMs);
        impl->reconnectTimer->setSingleShot(false);
        QObject::connect(impl->reconnectTimer, &QTimer::timeout, impl->server,
            [impl] {
                if (!impl->autoReconnect.load(std::memory_order_acquire)) return;
                auto const s = impl->state.load(std::memory_order_acquire);
                if (s == ConnectionState::Connected) return;
                if (s == ConnectionState::Connecting) return;
                impl->server->disconnectDevice();
                applyListenConfig(impl->server, impl->cfg);
            });
        impl->reconnectTimer->start();
    });
}

ReadResult ModbusTcpServerTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    detail::invokeBlocking(m_impl->server, [this, req, &result] {
        QList<quint16> out;
        out.reserve(req.count);
        for (int i = 0; i < req.count; ++i) {
            quint16 v = 0;
            if (!m_impl->server->data(req.table, req.startAddress + i, &v)) {
                result.ok           = false;
                result.errorMessage = QStringLiteral("address out of range");
                return;
            }
            out.append(v);
        }
        result.ok     = true;
        result.values = std::move(out);
    });
    return result;
}

WriteResult ModbusTcpServerTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    detail::invokeBlocking(m_impl->server, [this, batch, &result] {
        for (int i = 0; i < batch.values.size(); ++i) {
            if (!m_impl->server->setData(batch.table,
                                          batch.startAddress + i,
                                          batch.values.at(i))) {
                result.ok           = false;
                result.errorMessage = QStringLiteral("setData refused");
                return;
            }
        }
        result.ok = true;
    });
    return result;
}

void ModbusTcpServerTransport::readAsync(ReadRequest const& req, ReadDone done) {
    if (state() != ConnectionState::Connected) {
        ReadResult result;
        result.startAddress = req.startAddress;
        result.errorMessage = QStringLiteral("not connected");
        done(std::move(result));
        return;
    }
    auto* server = m_impl->server;
    QMetaObject::invokeMethod(server, [server, req, done = std::move(done)]() mutable {
        ReadResult result;
        result.startAddress = req.startAddress;
        result.values.reserve(req.count);
        for (int i = 0; i < req.count; ++i) {
            quint16 value = 0;
            if (!server->data(req.table, req.startAddress + i, &value)) {
                result.errorMessage = QStringLiteral("address out of range");
                done(std::move(result));
                return;
            }
            result.values.append(value);
        }
        result.ok = true;
        done(std::move(result));
    }, Qt::QueuedConnection);
}

void ModbusTcpServerTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, QStringLiteral("not connected")});
        return;
    }
    auto* server = m_impl->server;
    QMetaObject::invokeMethod(server,
        [server, batch, done = std::move(done)]() mutable {
            for (int i = 0; i < batch.values.size(); ++i) {
                if (!server->setData(batch.table, batch.startAddress + i,
                                     batch.values.at(i))) {
                    done(WriteResult{false, QStringLiteral("setData refused")});
                    return;
                }
            }
            done(WriteResult{true, {}});
        }, Qt::QueuedConnection);
}

} // namespace core::transport
