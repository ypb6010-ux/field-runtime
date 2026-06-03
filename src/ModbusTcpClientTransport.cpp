#include "core/transport/ModbusTcpClientTransport.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include <QDateTime>
#include <QMetaObject>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtSerialBus/QModbusReply>
#include <QtSerialBus/QModbusTcpClient>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

namespace core::transport {

namespace {

QString errorStringFor(QModbusDevice::Error e) {
    switch (e) {
        case QModbusDevice::NoError:               return QStringLiteral("no error");
        case QModbusDevice::ReadError:             return QStringLiteral("read error");
        case QModbusDevice::WriteError:            return QStringLiteral("write error");
        case QModbusDevice::ConnectionError:       return QStringLiteral("connection error");
        case QModbusDevice::ConfigurationError:    return QStringLiteral("configuration error");
        case QModbusDevice::TimeoutError:          return QStringLiteral("timeout");
        case QModbusDevice::ProtocolError:         return QStringLiteral("protocol error");
        case QModbusDevice::ReplyAbortedError:     return QStringLiteral("reply aborted");
        case QModbusDevice::UnknownError:          return QStringLiteral("unknown error");
    }
    return QStringLiteral("unknown");
}

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

class ModbusTcpClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , scheduler(std::make_unique<sched::SerialScheduler>(cfg.scheduler))
        , thread(new QThread)
        , client(new QModbusTcpClient) {

        client->moveToThread(thread);
        thread->start();

        // Install the deferred-pump hook so the async scheduler can honour
        // inter_request_gap without sleeping: it asks us to re-pump after `ms`,
        // and we run that on the client's own thread via a one-shot timer
        // (context = client, so a pending timer is dropped if the client is
        // torn down). Posted to the client thread first so the timer is created
        // there regardless of which thread completed the previous request.
        {
            auto* cl = client;
            scheduler->setDelayFn([cl](int ms, std::function<void()> fn) {
                QMetaObject::invokeMethod(cl, [cl, ms, fn = std::move(fn)]() mutable {
                    QTimer::singleShot(ms, cl, [fn = std::move(fn)]() mutable { fn(); });
                });
            });
        }

        // The state and error signals are emitted from the client's thread;
        // each handler updates atomics so any thread reading state() sees a
        // current view without locking.
        QObject::connect(client, &QModbusDevice::stateChanged,
                         client, [this](QModbusDevice::State s) {
            auto const prev = state.exchange(stateFromQt(s),
                                             std::memory_order_acq_rel);
            auto const cur = stateFromQt(s);
            if (busPtr) {
                if (cur == ConnectionState::Connected
                    && prev != ConnectionState::Connected) {
                    busPtr->publish(bus::TransportEvent{
                        cfg.id, bus::TransportEventKind::Connected, {}});
                } else if (cur == ConnectionState::Disconnected
                           && prev == ConnectionState::Connected) {
                    busPtr->publish(bus::TransportEvent{
                        cfg.id, bus::TransportEventKind::Disconnected, {}});
                }
            }
        });
        QObject::connect(client, &QModbusDevice::errorOccurred,
                         client, [this](QModbusDevice::Error e) {
            if (e != QModbusDevice::NoError) {
                auto const prev = state.exchange(ConnectionState::Error,
                                                  std::memory_order_acq_rel);
                QString msg = client->errorString();
                if (msg.isEmpty()) msg = errorStringFor(e);
                lastError = msg;
                if (busPtr && prev == ConnectionState::Connected) {
                    busPtr->publish(bus::TransportEvent{
                        cfg.id, bus::TransportEventKind::Disconnected, msg});
                }
            }
        });
    }

    ~Impl() {
        autoReconnect.store(false, std::memory_order_release);
        // Halt the async scheduler first: any completion that fires while we
        // join the worker thread below must NOT pump the next queued module
        // request into this half-destroyed transport.
        if (scheduler) scheduler->stopAsync();
        if (reconnectTimer) {
            QMetaObject::invokeMethod(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        // Tear down the QObject that lives on the worker thread *while that
        // thread is still spinning its event loop*, so any in-flight Modbus
        // replies disconnect cleanly. Destroying the client from the test
        // thread after the worker has stopped is a known segfault source —
        // pending QModbusReply::finished callbacks would fire into freed
        // memory.
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

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::SerialScheduler>            scheduler;
    QThread*                                           thread = nullptr;
    QModbusTcpClient*                                  client = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
};

ModbusTcpClientTransport::ModbusTcpClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

ModbusTcpClientTransport::~ModbusTcpClientTransport() = default;

QString               ModbusTcpClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusTcpClientTransport::kind()  const { return TransportKind::ModbusTcpClient; }
ConnectionState       ModbusTcpClientTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }

sched::RequestScheduler& ModbusTcpClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
ModbusTcpClientTransport::connect() {
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }

    bool kicked = false;
    QMetaObject::invokeMethod(m_impl->client, [this, &kicked] {
        m_impl->client->setConnectionParameter(
            QModbusDevice::NetworkAddressParameter, m_impl->cfg.host);
        m_impl->client->setConnectionParameter(
            QModbusDevice::NetworkPortParameter, m_impl->cfg.port);
        m_impl->client->setTimeout(m_impl->cfg.requestTimeoutMs);
        m_impl->client->setNumberOfRetries(0);
        kicked = m_impl->client->connectDevice();
    }, Qt::BlockingQueuedConnection);

    if (!kicked) {
        armReconnectIfConfigured();
        return std::unexpected(m_impl->lastError.isEmpty()
            ? QStringLiteral("connectDevice() returned false")
            : m_impl->lastError);
    }

    // Poll for terminal state up to connectTimeoutMs. We deliberately avoid
    // running a local QEventLoop here so callers driving connect() from a
    // non-Qt thread (e.g. Catch2's test thread) still work.
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
            return std::unexpected(m_impl->lastError);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    armReconnectIfConfigured();
    return std::unexpected(QStringLiteral("connect timeout"));
}

void ModbusTcpClientTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.load(std::memory_order_acquire)) return;
    m_impl->autoReconnect.store(true, std::memory_order_release);

    auto* impl = m_impl.get();
    QString const id = m_impl->cfg.id;
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;

    QMetaObject::invokeMethod(m_impl->client, [impl, id, intervalMs] {
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
                // Re-issue connectDevice on the client's own thread.
                impl->client->disconnectDevice();
                impl->client->setConnectionParameter(
                    QModbusDevice::NetworkAddressParameter, impl->cfg.host);
                impl->client->setConnectionParameter(
                    QModbusDevice::NetworkPortParameter, impl->cfg.port);
                impl->client->setTimeout(impl->cfg.requestTimeoutMs);
                impl->client->setNumberOfRetries(0);
                impl->client->connectDevice();
            });
        impl->reconnectTimer->start();
    }, Qt::BlockingQueuedConnection);
}

void ModbusTcpClientTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    QMetaObject::invokeMethod(m_impl->client, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->client->disconnectDevice();
    }, Qt::BlockingQueuedConnection);

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (state() == ConnectionState::Disconnected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

namespace {

template <class Init>
auto invokeReplyAndWait(QModbusTcpClient*  client,
                         int                timeoutMs,
                         Init               buildReply)
    -> std::pair<bool, QString> {

    QSemaphore     done(0);
    bool           ok       = false;
    QString        err;
    QModbusReply*  captured = nullptr;
    QList<quint16> values;

    QMetaObject::invokeMethod(client, [&] {
        QModbusReply* reply = buildReply(client);
        if (!reply) {
            err = client->errorString();
            if (err.isEmpty()) err = QStringLiteral("send failed");
            done.release();
            return;
        }
        captured = reply;
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                err = reply->errorString();
            } else {
                ok = true;
                values = reply->result().values();
            }
            reply->deleteLater();
            done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, client,
            [reply, &ok, &err, &values, &done] {
                if (reply->error() != QModbusDevice::NoError) {
                    err = reply->errorString();
                } else {
                    ok     = true;
                    values = reply->result().values();
                }
                reply->deleteLater();
                done.release();
            });
    }, Qt::BlockingQueuedConnection);

    if (!done.tryAcquire(1, timeoutMs)) {
        return {false, QStringLiteral("client timeout")};
    }
    Q_UNUSED(captured);
    (void)values;  // values used for read paths via lambda
    return {ok, err};
}

} // namespace

ReadResult ModbusTcpClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;

    if (state() != ConnectionState::Connected) {
        result.ok           = false;
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }

    QSemaphore done(0);
    QMetaObject::invokeMethod(m_impl->client, [this, req, &result, &done] {
        QModbusDataUnit unit(req.table, req.startAddress, req.count);
        auto* reply = m_impl->client->sendReadRequest(unit, m_impl->cfg.slaveId);
        if (!reply) {
            result.ok           = false;
            result.errorMessage = m_impl->client->errorString();
            if (result.errorMessage.isEmpty())
                result.errorMessage = QStringLiteral("sendReadRequest failed");
            done.release();
            return;
        }
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                result.ok           = false;
                result.errorMessage = reply->errorString();
            } else {
                result.ok     = true;
                result.values = reply->result().values();
            }
            reply->deleteLater();
            done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, m_impl->client,
            [reply, &result, &done] {
                if (reply->error() != QModbusDevice::NoError) {
                    result.ok           = false;
                    result.errorMessage = reply->errorString();
                } else {
                    result.ok     = true;
                    result.values = reply->result().values();
                }
                reply->deleteLater();
                done.release();
            });
    }, Qt::BlockingQueuedConnection);

    if (!done.tryAcquire(1, m_impl->cfg.requestTimeoutMs * 2 + 500)) {
        result.ok           = false;
        result.errorMessage = QStringLiteral("read timeout");
    }
    return result;
}

WriteResult ModbusTcpClientTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (state() != ConnectionState::Connected) {
        result.ok           = false;
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    if (batch.values.isEmpty()) {
        result.ok = true;
        return result;
    }

    QSemaphore done(0);
    QMetaObject::invokeMethod(m_impl->client, [this, batch, &result, &done] {
        QModbusDataUnit unit(batch.table, batch.startAddress, batch.values.size());
        for (int i = 0; i < batch.values.size(); ++i) {
            unit.setValue(i, batch.values.at(i));
        }
        auto* reply = m_impl->client->sendWriteRequest(unit, m_impl->cfg.slaveId);
        if (!reply) {
            result.ok           = false;
            result.errorMessage = m_impl->client->errorString();
            if (result.errorMessage.isEmpty())
                result.errorMessage = QStringLiteral("sendWriteRequest failed");
            done.release();
            return;
        }
        if (reply->isFinished()) {
            if (reply->error() != QModbusDevice::NoError) {
                result.ok           = false;
                result.errorMessage = reply->errorString();
            } else {
                result.ok = true;
            }
            reply->deleteLater();
            done.release();
            return;
        }
        QObject::connect(reply, &QModbusReply::finished, m_impl->client,
            [reply, &result, &done] {
                if (reply->error() != QModbusDevice::NoError) {
                    result.ok           = false;
                    result.errorMessage = reply->errorString();
                } else {
                    result.ok = true;
                }
                reply->deleteLater();
                done.release();
            });
    }, Qt::BlockingQueuedConnection);

    if (!done.tryAcquire(1, m_impl->cfg.requestTimeoutMs * 2 + 500)) {
        result.ok           = false;
        result.errorMessage = QStringLiteral("write timeout");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Async I/O — non-blocking. The request is posted to the client's worker
// thread (QueuedConnection, so the caller returns at once) and `done` is
// invoked on that thread when the reply finishes. The client's own event loop
// services the socket and the per-request timeout (setTimeout + retries 0), so
// no caller thread is ever parked and no extra wait machinery is needed.
// ---------------------------------------------------------------------------
void ModbusTcpClientTransport::readAsync(ReadRequest const& req, ReadDone done) {
    if (state() != ConnectionState::Connected) {
        ReadResult r;
        r.startAddress = req.startAddress;
        r.ok = false;
        r.errorMessage = QStringLiteral("not connected");
        done(std::move(r));
        return;
    }
    auto*     client  = m_impl->client;
    int const slaveId = m_impl->cfg.slaveId;
    QMetaObject::invokeMethod(client, [client, slaveId, req, done = std::move(done)]() mutable {
        auto* reply = client->sendReadRequest(
            QModbusDataUnit(req.table, req.startAddress, req.count), slaveId);
        if (!reply) {
            ReadResult r;
            r.startAddress = req.startAddress;
            r.ok = false;
            r.errorMessage = client->errorString();
            if (r.errorMessage.isEmpty()) r.errorMessage = QStringLiteral("sendReadRequest failed");
            done(std::move(r));
            return;
        }
        auto finish = [reply, req, done = std::move(done)]() {
            ReadResult r;
            r.startAddress = req.startAddress;
            if (reply->error() != QModbusDevice::NoError) {
                r.ok = false;
                r.errorMessage = reply->errorString();
            } else {
                r.ok = true;
                r.values = reply->result().values();
            }
            reply->deleteLater();
            done(std::move(r));
        };
        if (reply->isFinished()) finish();
        else QObject::connect(reply, &QModbusReply::finished, client, std::move(finish));
    }, Qt::QueuedConnection);
}

void ModbusTcpClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, QStringLiteral("not connected")});
        return;
    }
    if (batch.values.isEmpty()) {
        done(WriteResult{true, {}});
        return;
    }
    auto*     client  = m_impl->client;
    int const slaveId = m_impl->cfg.slaveId;
    QMetaObject::invokeMethod(client, [client, slaveId, batch, done = std::move(done)]() mutable {
        QModbusDataUnit unit(batch.table, batch.startAddress, batch.values.size());
        for (int i = 0; i < batch.values.size(); ++i) unit.setValue(i, batch.values.at(i));
        auto* reply = client->sendWriteRequest(unit, slaveId);
        if (!reply) {
            QString err = client->errorString();
            if (err.isEmpty()) err = QStringLiteral("sendWriteRequest failed");
            done(WriteResult{false, std::move(err)});
            return;
        }
        auto finish = [reply, done = std::move(done)]() {
            WriteResult r;
            if (reply->error() != QModbusDevice::NoError) {
                r.ok = false;
                r.errorMessage = reply->errorString();
            } else {
                r.ok = true;
            }
            reply->deleteLater();
            done(std::move(r));
        };
        if (reply->isFinished()) finish();
        else QObject::connect(reply, &QModbusReply::finished, client, std::move(finish));
    }, Qt::QueuedConnection);
}

} // namespace core::transport
