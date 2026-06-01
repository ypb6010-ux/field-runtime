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
#include <QtSerialBus/QModbusDataUnitMap>
#include <QtSerialBus/QModbusTcpServer>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

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

} // namespace

class ModbusTcpServerTransport::Impl {
public:
    Impl(Config c, bus::EventBus& bus)
        : cfg(std::move(c))
        , bus(&bus)
        , scheduler(std::make_unique<sched::SerialScheduler>(cfg.scheduler))
        , thread(new QThread)
        , server(new QModbusTcpServer) {

        server->moveToThread(thread);
        thread->start();

        QObject::connect(server, &QModbusDevice::stateChanged,
                         server, [this](QModbusDevice::State s) {
            state.store(stateFromQt(s), std::memory_order_release);
        });
        QObject::connect(server, &QModbusDevice::errorOccurred,
                         server, [this](QModbusDevice::Error) {
            state.store(ConnectionState::Error, std::memory_order_release);
            lastError = server->errorString();
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
            this->bus->publish(bus::ServerWriteEvent{
                cfg.id, table, address, std::move(values)});
        });
    }

    ~Impl() {
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
    bus::EventBus*                                    bus;
    std::unique_ptr<sched::SerialScheduler>            scheduler;
    QThread*                                           thread = nullptr;
    QModbusTcpServer*                                  server = nullptr;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
};

ModbusTcpServerTransport::ModbusTcpServerTransport(Config cfg, bus::EventBus& bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusTcpServerTransport::~ModbusTcpServerTransport() = default;

QString               ModbusTcpServerTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusTcpServerTransport::kind()  const { return TransportKind::ModbusTcpServer; }
ConnectionState       ModbusTcpServerTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }

sched::RequestScheduler& ModbusTcpServerTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
ModbusTcpServerTransport::connect() {
    if (state() == ConnectionState::Connected) return {};
    bool ok = false;
    QMetaObject::invokeMethod(m_impl->server, [this, &ok] {
        // Build the register map covering each configured listen range.
        // Adjacent ranges of the same table are coalesced into a single
        // QModbusDataUnit covering the full extent.
        QHash<QModbusDataUnit::RegisterType, QPair<int, int>> bounds;
        for (auto const& r : m_impl->cfg.listenRanges) {
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
        m_impl->server->setMap(map);
        m_impl->server->setServerAddress(m_impl->cfg.slaveId);
        m_impl->server->setConnectionParameter(
            QModbusDevice::NetworkAddressParameter, m_impl->cfg.listenAddress);
        m_impl->server->setConnectionParameter(
            QModbusDevice::NetworkPortParameter, m_impl->cfg.listenPort);
        ok = m_impl->server->connectDevice();
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        return std::unexpected(m_impl->lastError.isEmpty()
            ? QStringLiteral("connectDevice() returned false")
            : m_impl->lastError);
    }

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) return {};
        if (s == ConnectionState::Error)     return std::unexpected(m_impl->lastError);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::unexpected(QStringLiteral("listen timeout"));
}

void ModbusTcpServerTransport::disconnect() {
    QMetaObject::invokeMethod(m_impl->server, [this] {
        m_impl->server->disconnectDevice();
    }, Qt::BlockingQueuedConnection);
}

ReadResult ModbusTcpServerTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    QMetaObject::invokeMethod(m_impl->server, [this, req, &result] {
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
    }, Qt::BlockingQueuedConnection);
    return result;
}

WriteResult ModbusTcpServerTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    QMetaObject::invokeMethod(m_impl->server, [this, batch, &result] {
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
    }, Qt::BlockingQueuedConnection);
    return result;
}

} // namespace core::transport
