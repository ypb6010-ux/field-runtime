// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/ModbusTcpServerTransport.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include <QHash>
#include <QMetaObject>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtSerialBus/QModbusDataUnitMap>
#include <QtSerialBus/QModbusTcpServer>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"
#include "core/transport/RegisterTableQt.h"

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

} // namespace

class ModbusTcpServerTransport::Impl {
public:
    Impl(Config c, bus::EventBus& busRef)
        : cfg(std::move(c))
        , busPtr(&busRef)
        , scheduler(std::make_unique<sched::SerialScheduler>(cfg.scheduler))
        , thread(new QThread)
        , server(new QModbusTcpServer) {

        server->moveToThread(thread);
        thread->start();

        QObject::connect(server, &QModbusDevice::stateChanged,
                         server, [this](QModbusDevice::State s) {
            auto const cur  = stateFromQt(s);
            auto const prev = state.exchange(cur, std::memory_order_acq_rel);
            if (!busPtr) return;
            if (cur == ConnectionState::Connected
                && prev != ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Connected, {}});
            } else if (cur == ConnectionState::Disconnected
                       && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, {}});
            }
        });
        QObject::connect(server, &QModbusDevice::errorOccurred,
                         server, [this](QModbusDevice::Error) {
            auto const prev = state.exchange(ConnectionState::Error,
                                              std::memory_order_acq_rel);
            lastError = server->errorString();
            if (busPtr && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, lastError.toStdString()});
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
            QMetaObject::invokeMethod(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        if (server) {
            QMetaObject::invokeMethod(server, [this] {
                server->disconnectDevice();
                delete server;
                server = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr;
    std::unique_ptr<sched::SerialScheduler>            scheduler;
    QThread*                                           thread = nullptr;
    QModbusTcpServer*                                  server = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
};

ModbusTcpServerTransport::ModbusTcpServerTransport(Config cfg, bus::EventBus& bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusTcpServerTransport::~ModbusTcpServerTransport() = default;

std::string           ModbusTcpServerTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusTcpServerTransport::kind()  const { return TransportKind::ModbusTcpServer; }
ConnectionState       ModbusTcpServerTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }

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
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }
    bool ok = false;
    QMetaObject::invokeMethod(m_impl->server, [this, &ok] {
        ok = applyListenConfig(m_impl->server, m_impl->cfg);
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        armReconnectIfConfigured();
        return std::unexpected(m_impl->lastError.isEmpty()
            ? std::string("connectDevice() returned false")
            : m_impl->lastError.toStdString());
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
            return std::unexpected(m_impl->lastError.toStdString());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    armReconnectIfConfigured();
    return std::unexpected(std::string("listen timeout"));
}

void ModbusTcpServerTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    QMetaObject::invokeMethod(m_impl->server, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->server->disconnectDevice();
    }, Qt::BlockingQueuedConnection);
}

void ModbusTcpServerTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.load(std::memory_order_acquire)) return;
    m_impl->autoReconnect.store(true, std::memory_order_release);

    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;

    QMetaObject::invokeMethod(m_impl->server, [impl, intervalMs] {
        if (impl->reconnectTimer) return;
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
    }, Qt::BlockingQueuedConnection);
}

ReadResult ModbusTcpServerTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    QMetaObject::invokeMethod(m_impl->server, [this, req, &result] {
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
    }, Qt::BlockingQueuedConnection);
    return result;
}

WriteResult ModbusTcpServerTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    QMetaObject::invokeMethod(m_impl->server, [this, batch, &result] {
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
    }, Qt::BlockingQueuedConnection);
    return result;
}

} // namespace core::transport
