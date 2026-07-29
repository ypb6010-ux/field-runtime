// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/ModbusTcpServerTransport.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <QHash>
#include <QHostAddress>
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
#include "core/transport/RegisterTableQt.h"

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

QString qs(std::string const& s) {
    return QString::fromStdString(s);
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
        , statusTracker(
              cfg.id, TransportKind::ModbusTcpServer, &busRef,
              EndpointInfo{cfg.listenAddress, cfg.listenPort}, {})
        , scheduler(std::make_unique<sched::SerialScheduler>(cfg.scheduler))
        , thread(new QThread)
        , server(new QModbusTcpServer) {

        server->moveToThread(thread);
        thread->start();

        detail::invokeBlocking(server, [this] {
            server->installConnectionObserver(
                new ConnectionObserver(
                    [this](QTcpSocket* socket) {
                        return addPeer(socket);
                    }));
        });
        QObject::connect(
            server, &QModbusTcpServer::modbusClientDisconnected,
            server, [this](QTcpSocket* socket) {
                removePeer(socket, "peer disconnected");
            });
        QObject::connect(server, &QModbusDevice::stateChanged,
                         server, [this](QModbusDevice::State s) {
            auto const cur  = stateFromQt(s);
            if (cur == ConnectionState::Disconnected) {
                drainPeers("server stopped");
            }
            if (cur == ConnectionState::Connected) setLastError({});
            auto const error = getLastError();
            auto const effective =
                cur == ConnectionState::Disconnected && !error.empty()
                ? ConnectionState::Error : cur;
            auto const prev =
                state.exchange(effective, std::memory_order_acq_rel);
            statusTracker.update(
                effective,
                effective == ConnectionState::Error ? error : std::string{});
            if (!busPtr) return;
            if (effective == ConnectionState::Connected
                && prev != ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Connected, {}});
            } else if (effective != ConnectionState::Connected
                       && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, error});
            }
        });
        QObject::connect(server, &QModbusDevice::errorOccurred,
                         server, [this](QModbusDevice::Error) {
            auto const prev = state.exchange(ConnectionState::Error,
                                              std::memory_order_acq_rel);
            auto const message = server->errorString().toStdString();
            setLastError(message);
            statusTracker.update(ConnectionState::Error, message);
            if (busPtr && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, message});
            }
        });
        QObject::connect(server, &QModbusTcpServer::dataWritten,
                         server, [this](QModbusDataUnit::RegisterType table,
                                         int address, int size) {
            core::RegisterWords values;
            values.reserve(size);
            for (int i = 0; i < size; ++i) {
                quint16 v = 0;
                server->data(table, address + i, &v);
                values.push_back(v);
            }
            busPtr->publish(bus::ServerWriteEvent{
                cfg.id, core::fromQModbus(table), address, std::move(values)});
        });
    }

    ~Impl() {
        autoReconnect.store(false, std::memory_order_release);
        if (reconnectTimer) {
            detail::invokeBlocking(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            });
        }
        if (server) {
            detail::invokeBlocking(server, [this] {
                server->disconnectDevice();
                delete server;
                server = nullptr;
            });
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    void setLastError(std::string message) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    std::string getLastError() const {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    bool addPeer(QTcpSocket* socket) {
        if (!socket) return false;
        PeerSession session;
        session.transportId = cfg.id;
        session.sessionId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        session.localEndpoint = EndpointInfo{
            socket->localAddress().toString().toStdString(),
            static_cast<std::uint16_t>(socket->localPort())};
        session.remoteEndpoint = EndpointInfo{
            socket->peerAddress().toString().toStdString(),
            static_cast<std::uint16_t>(socket->peerPort())};
        session.connectedAt = std::chrono::system_clock::now();
        {
            std::lock_guard lock(peersMutex);
            if (cfg.maxClients > 0
                && static_cast<int>(peers.size()) >= cfg.maxClients) {
                return false;
            }
            peers.insert(socket, session);
        }
        if (busPtr) {
            busPtr->publish(bus::PeerSessionChanged{
                bus::PeerSessionChangeKind::Connected,
                session, {}, std::chrono::system_clock::now()});
        }
        return true;
    }

    void removePeer(QTcpSocket* socket, std::string reason) {
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
                bus::PeerSessionChangeKind::Disconnected,
                std::move(session), std::move(reason),
                std::chrono::system_clock::now()});
        }
    }

    void drainPeers(std::string const& reason) {
        std::vector<PeerSession> removed;
        {
            std::lock_guard lock(peersMutex);
            removed.reserve(peers.size());
            for (auto it = peers.cbegin(); it != peers.cend(); ++it) {
                removed.push_back(it.value());
            }
            peers.clear();
        }
        if (!busPtr) return;
        for (auto& session : removed) {
            busPtr->publish(bus::PeerSessionChanged{
                bus::PeerSessionChangeKind::Disconnected,
                std::move(session), reason,
                std::chrono::system_clock::now()});
        }
    }

    std::vector<PeerSession> peerSnapshot() const {
        std::lock_guard lock(peersMutex);
        std::vector<PeerSession> result;
        result.reserve(peers.size());
        for (auto it = peers.cbegin(); it != peers.cend(); ++it) {
            result.push_back(it.value());
        }
        return result;
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr;
    detail::TransportStatusTracker                    statusTracker;
    std::unique_ptr<sched::SerialScheduler>            scheduler;
    QThread*                                           thread = nullptr;
    QModbusTcpServer*                                  server = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    std::string                                        lastError;
    mutable std::mutex                                 peersMutex;
    QHash<QTcpSocket*, PeerSession>                    peers;
};

ModbusTcpServerTransport::ModbusTcpServerTransport(Config cfg, bus::EventBus& bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusTcpServerTransport::~ModbusTcpServerTransport() = default;

std::string           ModbusTcpServerTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusTcpServerTransport::kind()  const { return TransportKind::ModbusTcpServer; }
ConnectionState       ModbusTcpServerTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }
TransportStatus       ModbusTcpServerTransport::status() const {
    return m_impl->statusTracker.snapshot();
}
std::vector<PeerSession>
ModbusTcpServerTransport::peerSessions() const {
    return m_impl->peerSnapshot();
}

sched::RequestScheduler& ModbusTcpServerTransport::scheduler() { return *m_impl->scheduler; }

namespace {

// Apply the configured listen ranges and connection parameters to `server`.
// Called on the server's own thread inside an invokeMethod hop.
bool applyListenConfig(QModbusTcpServer*                       server,
                       ModbusTcpServerTransport::Config const&  cfg) {
    QHash<QModbusDataUnit::RegisterType, QPair<int, int>> bounds;
    for (auto const& r : cfg.listenRanges) {
        auto const qt   = core::toQModbus(r.table);
        int const start = r.startAddress;
        int const end   = r.startAddress + r.size - 1;
        if (bounds.contains(qt)) {
            auto cur = bounds.value(qt);
            cur.first  = std::min(cur.first,  start);
            cur.second = std::max(cur.second, end);
            bounds[qt] = cur;
        } else {
            bounds.insert(qt, qMakePair(start, end));
        }
    }
    QModbusDataUnitMap map;
    for (auto it = bounds.constBegin(); it != bounds.constEnd(); ++it) {
        int const count = it.value().second - it.value().first + 1;
        map.insert(it.key(),
            QModbusDataUnit(it.key(), it.value().first, count));
    }
    server->setMap(map);
    server->setServerAddress(cfg.slaveId);
    server->setConnectionParameter(
        QModbusDevice::NetworkAddressParameter, qs(cfg.listenAddress));
    server->setConnectionParameter(
        QModbusDevice::NetworkPortParameter, cfg.listenPort);
    return server->connectDevice();
}

} // namespace

std::expected<void, std::string>
ModbusTcpServerTransport::connect() {
    if (QThread::currentThread() == m_impl->server->thread()) {
        return std::unexpected(
            std::string("connect() cannot block the Modbus TCP server thread"));
    }
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
        if (!ok) {
            m_impl->setLastError(
                m_impl->server->errorString().toStdString());
        }
    });
    if (!ok) {
        armReconnectIfConfigured();
        auto const error = m_impl->getLastError();
        auto const message = error.empty()
            ? std::string("connectDevice() returned false")
            : error;
        m_impl->state.store(ConnectionState::Error,
                            std::memory_order_release);
        m_impl->statusTracker.update(ConnectionState::Error, message);
        return std::unexpected(message);
    }

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
    auto const message =
        error.empty() ? std::string("listen timeout") : error;
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}

void ModbusTcpServerTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    m_impl->setLastError({});
    detail::invokeBlocking(m_impl->server, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->server->disconnectDevice();
    });
    m_impl->state.store(ConnectionState::Disconnected,
                        std::memory_order_release);
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
            if (!impl->reconnectTimer->isActive()) {
                impl->reconnectTimer->start();
            }
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
        core::RegisterWords out;
        out.reserve(req.count);
        for (int i = 0; i < req.count; ++i) {
            quint16 v = 0;
            if (!m_impl->server->data(core::toQModbus(req.table), req.startAddress + i, &v)) {
                result.ok           = false;
                result.errorMessage = "address out of range";
                return;
            }
            out.push_back(v);
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
            if (!m_impl->server->setData(core::toQModbus(batch.table),
                                          batch.startAddress + i,
                                          batch.values.at(i))) {
                result.ok           = false;
                result.errorMessage = "setData refused";
                return;
            }
        }
        result.ok = true;
    });
    return result;
}

} // namespace core::transport
