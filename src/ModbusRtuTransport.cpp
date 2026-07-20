// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/ModbusRtuTransport.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <QMetaObject>
#include <QSemaphore>
#include <QSerialPort>
#include <QThread>
#include <QTimer>
#include <QtSerialBus/QModbusReply>
#include <QtSerialBus/QModbusRtuSerialClient>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

#include "ModbusReplyAsync.h"
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

QSerialPort::Parity parityToQt(ModbusRtuTransport::Parity p) {
    switch (p) {
        case ModbusRtuTransport::Parity::None: return QSerialPort::NoParity;
        case ModbusRtuTransport::Parity::Even: return QSerialPort::EvenParity;
        case ModbusRtuTransport::Parity::Odd:  return QSerialPort::OddParity;
    }
    return QSerialPort::NoParity;
}

struct PendingRead {
    QSemaphore done{0};
    ReadResult result;
};

struct PendingWrite {
    QSemaphore  done{0};
    WriteResult result;
};

} // namespace

class ModbusRtuTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , statusTracker(cfg.id, TransportKind::ModbusRtu, b,
                        EndpointInfo{cfg.portName, 0}, {})
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread)
        , client(new QModbusRtuSerialClient) {

        client->moveToThread(thread);
        thread->start();

        // Honour inter_request_gap in the async path via a one-shot timer on
        // the client thread (critical for RS-485 bus turnaround). See the TCP
        // transport for the rationale.
        {
            auto* cl = client;
            scheduler->setDelayFn([cl](int ms, std::function<void()> fn) {
                QMetaObject::invokeMethod(cl, [cl, ms, fn = std::move(fn)]() mutable {
                    QTimer::singleShot(ms, cl, [fn = std::move(fn)]() mutable { fn(); });
                });
            });
        }

        QObject::connect(client, &QModbusDevice::stateChanged,
                         client, [this](QModbusDevice::State s) {
            auto const cur  = stateFromQt(s);
            if (cur == ConnectionState::Connected) setLastError({});
            auto const error = getLastError();
            auto const effective = cur == ConnectionState::Disconnected
                                && !error.isEmpty()
                ? ConnectionState::Error : cur;
            state.store(effective, std::memory_order_release);
            statusTracker.update(effective,
                effective == ConnectionState::Error ? error : QString{});
        });
        QObject::connect(client, &QModbusDevice::errorOccurred,
                         client, [this](QModbusDevice::Error e) {
            if (e == QModbusDevice::NoError) return;
            auto const message = client->errorString();
            setLastError(message);
            state.store(ConnectionState::Error, std::memory_order_release);
            statusTracker.update(ConnectionState::Error, message);
        });
    }

    ~Impl() {
        autoReconnect.store(false, std::memory_order_release);
        if (scheduler) scheduler->stopAsync();   // no async pump into a dying transport
        if (reconnectTimer) {
            detail::invokeBlocking(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            });
        }
        if (client) {
            detail::invokeBlocking(client, [this] {
                client->disconnectDevice();
                delete client;
                client = nullptr;
            });
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    void applyConnectionParams() {
        client->setConnectionParameter(QModbusDevice::SerialPortNameParameter, cfg.portName);
        client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, cfg.baudRate);
        client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, cfg.dataBits);
        client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, cfg.stopBits);
        client->setConnectionParameter(QModbusDevice::SerialParityParameter,
                                        int(parityToQt(cfg.parity)));
        client->setTimeout(cfg.requestTimeoutMs);
        client->setNumberOfRetries(0);
    }

    void setLastError(QString message) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    QString getLastError() const {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    Config                                            cfg;
    detail::TransportStatusTracker                    statusTracker;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QModbusRtuSerialClient*                            client = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    QString                                            lastError;
};

ModbusRtuTransport::ModbusRtuTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusRtuTransport::~ModbusRtuTransport() = default;

QString               ModbusRtuTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusRtuTransport::kind()  const { return TransportKind::ModbusRtu; }
ConnectionState       ModbusRtuTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }
TransportStatus       ModbusRtuTransport::status() const { return m_impl->statusTracker.snapshot(); }

sched::RequestScheduler& ModbusRtuTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
ModbusRtuTransport::connect() {
    if (QThread::currentThread() == m_impl->client->thread()) {
        return std::unexpected(QStringLiteral(
            "connect() cannot block the Modbus RTU worker thread"));
    }
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }
    m_impl->setLastError({});
    m_impl->state.store(ConnectionState::Connecting,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Connecting);
    bool kicked = false;
    detail::invokeBlocking(m_impl->client, [this, &kicked] {
        m_impl->applyConnectionParams();
        kicked = m_impl->client->connectDevice();
        if (!kicked) m_impl->setLastError(m_impl->client->errorString());
    });
    if (!kicked) {
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
        if (s == ConnectionState::Connected) { armReconnectIfConfigured(); return {}; }
        if (s == ConnectionState::Error) {
            armReconnectIfConfigured();
            return std::unexpected(m_impl->getLastError());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    armReconnectIfConfigured();
    auto const error = m_impl->getLastError();
    auto const message = error.isEmpty()
        ? QStringLiteral("connect timeout") : error;
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}

void ModbusRtuTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    m_impl->setLastError({});
    detail::invokeBlocking(m_impl->client, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->client->disconnectDevice();
    });
    m_impl->state.store(ConnectionState::Disconnected, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}

void ModbusRtuTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.load(std::memory_order_acquire)) return;
    m_impl->autoReconnect.store(true, std::memory_order_release);

    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;
    detail::invokeBlocking(m_impl->client, [impl, intervalMs] {
        if (impl->reconnectTimer) {
            if (!impl->reconnectTimer->isActive()) impl->reconnectTimer->start();
            return;
        }
        impl->reconnectTimer = new QTimer(impl->client);
        impl->reconnectTimer->setInterval(intervalMs);
        impl->reconnectTimer->setSingleShot(false);
        QObject::connect(impl->reconnectTimer, &QTimer::timeout, impl->client,
            [impl] {
                if (!impl->autoReconnect.load(std::memory_order_acquire)) return;
                auto const s = impl->state.load(std::memory_order_acquire);
                if (s == ConnectionState::Connected) return;
                if (s == ConnectionState::Connecting) return;
                impl->client->disconnectDevice();
                impl->applyConnectionParams();
                impl->client->connectDevice();
            });
        impl->reconnectTimer->start();
    });
}

ReadResult ModbusRtuTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage = QStringLiteral(
            "synchronous read cannot run on the Modbus RTU worker thread");
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    auto pending = std::make_shared<PendingRead>();
    pending->result.startAddress = req.startAddress;
    detail::invokeBlocking(m_impl->client, [this, req, pending] {
        QModbusDataUnit unit(req.table, req.startAddress, req.count);
        auto* reply = m_impl->client->sendReadRequest(unit, m_impl->cfg.slaveId);
        if (!reply) {
            pending->result.errorMessage = m_impl->client->errorString();
            if (pending->result.errorMessage.isEmpty())
                pending->result.errorMessage = QStringLiteral("sendReadRequest failed");
            pending->done.release();
            return;
        }
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                pending->result.errorMessage = reply->errorString();
            } else {
                pending->result.ok = true;
                pending->result.values = reply->result().values();
            }
            reply->deleteLater();
            pending->done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, m_impl->client,
            [reply, pending] {
                if (reply->error() != QModbusDevice::NoError) {
                    pending->result.errorMessage = reply->errorString();
                } else {
                    pending->result.ok = true;
                    pending->result.values = reply->result().values();
                }
                reply->deleteLater();
                pending->done.release();
            });
    });
    if (!pending->done.tryAcquire(1, m_impl->cfg.requestTimeoutMs * 2 + 500)) {
        result.ok = false;
        result.errorMessage = QStringLiteral("read timeout");
        return result;
    }
    return std::move(pending->result);
}

WriteResult ModbusRtuTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage = QStringLiteral(
            "synchronous write cannot run on the Modbus RTU worker thread");
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    if (batch.values.isEmpty()) { result.ok = true; return result; }

    auto pending = std::make_shared<PendingWrite>();
    detail::invokeBlocking(m_impl->client, [this, batch, pending] {
        QModbusDataUnit unit(batch.table, batch.startAddress, batch.values.size());
        for (int i = 0; i < batch.values.size(); ++i) unit.setValue(i, batch.values.at(i));
        auto* reply = m_impl->client->sendWriteRequest(unit, m_impl->cfg.slaveId);
        if (!reply) {
            pending->result.errorMessage = m_impl->client->errorString();
            if (pending->result.errorMessage.isEmpty())
                pending->result.errorMessage = QStringLiteral("sendWriteRequest failed");
            pending->done.release();
            return;
        }
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                pending->result.errorMessage = reply->errorString();
            } else { pending->result.ok = true; }
            reply->deleteLater();
            pending->done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, m_impl->client,
            [reply, pending] {
                if (reply->error() != QModbusDevice::NoError) {
                    pending->result.errorMessage = reply->errorString();
                } else { pending->result.ok = true; }
                reply->deleteLater();
                pending->done.release();
            });
    });
    if (!pending->done.tryAcquire(1, m_impl->cfg.requestTimeoutMs * 2 + 500)) {
        result.ok = false;
        result.errorMessage = QStringLiteral("write timeout");
        return result;
    }
    return std::move(pending->result);
}

// Async I/O — non-blocking; shares the QModbusReply helper with the TCP client.
void ModbusRtuTransport::readAsync(ReadRequest const& req, ReadDone done) {
    if (state() != ConnectionState::Connected) {
        ReadResult r;
        r.startAddress = req.startAddress;
        r.ok = false;
        r.errorMessage = QStringLiteral("not connected");
        done(std::move(r));
        return;
    }
    detail::modbusReadAsync(m_impl->client, m_impl->cfg.slaveId, req, std::move(done));
}

void ModbusRtuTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, QStringLiteral("not connected")});
        return;
    }
    detail::modbusWriteAsync(m_impl->client, m_impl->cfg.slaveId, batch, std::move(done));
}

} // namespace core::transport
