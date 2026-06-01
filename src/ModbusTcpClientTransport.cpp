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
#include <QtSerialBus/QModbusReply>
#include <QtSerialBus/QModbusTcpClient>

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
    explicit Impl(Config c)
        : cfg(std::move(c))
        , scheduler(std::make_unique<sched::SerialScheduler>(cfg.scheduler))
        , thread(new QThread)
        , client(new QModbusTcpClient) {

        client->moveToThread(thread);
        thread->start();

        // The state and error signals are emitted from the client's thread;
        // each handler updates atomics so any thread reading state() sees a
        // current view without locking.
        QObject::connect(client, &QModbusDevice::stateChanged,
                         client, [this](QModbusDevice::State s) {
            state.store(stateFromQt(s), std::memory_order_release);
        });
        QObject::connect(client, &QModbusDevice::errorOccurred,
                         client, [this](QModbusDevice::Error e) {
            if (e != QModbusDevice::NoError) {
                state.store(ConnectionState::Error, std::memory_order_release);
                QString msg = client->errorString();
                if (msg.isEmpty()) msg = errorStringFor(e);
                lastError = msg;
            }
        });
    }

    ~Impl() {
        // Tear down on the client's own thread.
        QMetaObject::invokeMethod(client, [this] {
            client->disconnectDevice();
            client->deleteLater();
            client = nullptr;
        }, Qt::BlockingQueuedConnection);

        thread->quit();
        thread->wait();
        delete thread;
    }

    Config                                            cfg;
    std::unique_ptr<sched::SerialScheduler>            scheduler;
    QThread*                                           thread = nullptr;
    QModbusTcpClient*                                  client = nullptr;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
};

ModbusTcpClientTransport::ModbusTcpClientTransport(Config cfg)
    : m_impl(std::make_unique<Impl>(std::move(cfg))) {}

ModbusTcpClientTransport::~ModbusTcpClientTransport() = default;

QString               ModbusTcpClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         ModbusTcpClientTransport::kind()  const { return TransportKind::ModbusTcpClient; }
ConnectionState       ModbusTcpClientTransport::state() const { return m_impl->state.load(std::memory_order_acquire); }

sched::RequestScheduler& ModbusTcpClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
ModbusTcpClientTransport::connect() {
    if (state() == ConnectionState::Connected) return {};

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
        if (s == ConnectionState::Connected) return {};
        if (s == ConnectionState::Error)
            return std::unexpected(m_impl->lastError);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::unexpected(QStringLiteral("connect timeout"));
}

void ModbusTcpClientTransport::disconnect() {
    QMetaObject::invokeMethod(m_impl->client, [this] {
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

} // namespace core::transport
