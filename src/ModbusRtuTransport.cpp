// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/ModbusRtuTransport.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include <QMetaObject>
#include <QSemaphore>
#include <QSerialPort>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QtSerialBus/QModbusReply>
#include <QtSerialBus/QModbusRtuSerialClient>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

#include "ModbusReplyAsync.h"

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

QString qs(std::string const& s) {
    return QString::fromStdString(s);
}

} // namespace

class ModbusRtuTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , scheduler(std::make_unique<sched::SerialScheduler>(cfg.scheduler))
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
        QObject::connect(client, &QModbusDevice::errorOccurred,
                         client, [this](QModbusDevice::Error e) {
            if (e == QModbusDevice::NoError) return;
            auto const prev = state.exchange(ConnectionState::Error,
                                              std::memory_order_acq_rel);
            lastError = client->errorString();
            if (busPtr && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, lastError.toStdString()});
            }
        });
    }

    ~Impl() {
        autoReconnect.store(false, std::memory_order_release);
        if (scheduler) scheduler->stopAsync();   // no async pump into a dying transport
        if (reconnectTimer) {
            QMetaObject::invokeMethod(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        if (client) {
            QMetaObject::invokeMethod(client, [this] {
                client->disconnectDevice();
                delete client;
                client = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    void applyConnectionParams() {
        client->setConnectionParameter(QModbusDevice::SerialPortNameParameter, qs(cfg.portName));
        client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, cfg.baudRate);
        client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, cfg.dataBits);
        client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, cfg.stopBits);
        client->setConnectionParameter(QModbusDevice::SerialParityParameter,
                                        int(parityToQt(cfg.parity)));
        client->setTimeout(cfg.requestTimeoutMs);
        client->setNumberOfRetries(0);
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::SerialScheduler>            scheduler;
    QThread*                                           thread = nullptr;
    QModbusRtuSerialClient*                            client = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
};

ModbusRtuTransport::ModbusRtuTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusRtuTransport::~ModbusRtuTransport() = default;

std::string           ModbusRtuTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusRtuTransport::kind()  const { return TransportKind::ModbusRtu; }
ConnectionState       ModbusRtuTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }

sched::RequestScheduler& ModbusRtuTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, std::string>
ModbusRtuTransport::connect() {
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }
    bool kicked = false;
    QMetaObject::invokeMethod(m_impl->client, [this, &kicked] {
        m_impl->applyConnectionParams();
        kicked = m_impl->client->connectDevice();
    }, Qt::BlockingQueuedConnection);
    if (!kicked) {
        armReconnectIfConfigured();
        return std::unexpected(m_impl->lastError.isEmpty()
            ? std::string("connectDevice() returned false")
            : m_impl->lastError.toStdString());
    }
    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) { armReconnectIfConfigured(); return {}; }
        if (s == ConnectionState::Error) {
            armReconnectIfConfigured();
            return std::unexpected(m_impl->lastError.toStdString());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    armReconnectIfConfigured();
    return std::unexpected(std::string("connect timeout"));
}

void ModbusRtuTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    QMetaObject::invokeMethod(m_impl->client, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->client->disconnectDevice();
    }, Qt::BlockingQueuedConnection);
}

void ModbusRtuTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.load(std::memory_order_acquire)) return;
    m_impl->autoReconnect.store(true, std::memory_order_release);

    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;
    QMetaObject::invokeMethod(m_impl->client, [impl, intervalMs] {
        if (impl->reconnectTimer) return;
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
    }, Qt::BlockingQueuedConnection);
}

ReadResult ModbusRtuTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = "not connected";
        return result;
    }
    QSemaphore done(0);
    QMetaObject::invokeMethod(m_impl->client, [this, req, &result, &done] {
        QModbusDataUnit unit(core::toQModbus(req.table), req.startAddress, req.count);
        auto* reply = m_impl->client->sendReadRequest(unit, m_impl->cfg.slaveId);
        if (!reply) {
            result.errorMessage = m_impl->client->errorString().toStdString();
            done.release();
            return;
        }
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                result.errorMessage = reply->errorString().toStdString();
            } else {
                result.ok = true;
                result.values = core::fromQtWords(reply->result().values());
            }
            reply->deleteLater();
            done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, m_impl->client,
            [reply, &result, &done] {
                if (reply->error() != QModbusDevice::NoError) {
                    result.errorMessage = reply->errorString().toStdString();
                } else {
                    result.ok = true;
                    result.values = core::fromQtWords(reply->result().values());
                }
                reply->deleteLater();
                done.release();
            });
    }, Qt::BlockingQueuedConnection);
    if (!done.tryAcquire(1, m_impl->cfg.requestTimeoutMs * 2 + 500)) {
        result.ok = false;
        result.errorMessage = "read timeout";
    }
    return result;
}

WriteResult ModbusRtuTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = "not connected";
        return result;
    }
    if (batch.values.empty()) { result.ok = true; return result; }

    QSemaphore done(0);
    QMetaObject::invokeMethod(m_impl->client, [this, batch, &result, &done] {
        int const valueCount = int(batch.values.size());
        QModbusDataUnit unit(core::toQModbus(batch.table), batch.startAddress, quint16(valueCount));
        for (int i = 0; i < valueCount; ++i) unit.setValue(i, batch.values.at(i));
        auto* reply = m_impl->client->sendWriteRequest(unit, m_impl->cfg.slaveId);
        if (!reply) {
            result.errorMessage = m_impl->client->errorString().toStdString();
            done.release();
            return;
        }
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                result.errorMessage = reply->errorString().toStdString();
            } else { result.ok = true; }
            reply->deleteLater();
            done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, m_impl->client,
            [reply, &result, &done] {
                if (reply->error() != QModbusDevice::NoError) {
                    result.errorMessage = reply->errorString().toStdString();
                } else { result.ok = true; }
                reply->deleteLater();
                done.release();
            });
    }, Qt::BlockingQueuedConnection);
    if (!done.tryAcquire(1, m_impl->cfg.requestTimeoutMs * 2 + 500)) {
        result.ok = false;
        result.errorMessage = "write timeout";
    }
    return result;
}

// Async I/O — non-blocking; shares the QModbusReply helper with the TCP client.
void ModbusRtuTransport::readAsync(ReadRequest const& req, ReadDone done) {
    if (state() != ConnectionState::Connected) {
        ReadResult r;
        r.startAddress = req.startAddress;
        r.ok = false;
        r.errorMessage = "not connected";
        done(std::move(r));
        return;
    }
    detail::modbusReadAsync(m_impl->client, m_impl->cfg.slaveId, req, std::move(done));
}

void ModbusRtuTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, "not connected"});
        return;
    }
    detail::modbusWriteAsync(m_impl->client, m_impl->cfg.slaveId, batch, std::move(done));
}

} // namespace core::transport
