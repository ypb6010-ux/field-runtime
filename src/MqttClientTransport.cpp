// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/MqttClientTransport.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>

#ifdef CORE_HAS_MQTT_QT
  #include <QtMqtt/QMqttClient>
  #include <QtMqtt/QMqttSubscription>
  #include <QtMqtt/QMqttTopicFilter>
  #include <QtMqtt/QMqttTopicName>
#endif

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

#include "QtThreadInvoke.h"

namespace core::transport {

#ifdef CORE_HAS_MQTT_QT

namespace {

ConnectionState stateFromQt(QMqttClient::ClientState s) {
    switch (s) {
        case QMqttClient::Disconnected: return ConnectionState::Disconnected;
        case QMqttClient::Connecting:   return ConnectionState::Connecting;
        case QMqttClient::Connected:    return ConnectionState::Connected;
    }
    return ConnectionState::Disconnected;
}

QString joinTopic(QString const& prefix, QString const& tail) {
    if (prefix.isEmpty()) return tail;
    if (prefix.endsWith('/') || tail.startsWith('/')) return prefix + tail;
    return prefix + QStringLiteral("/") + tail;
}

QString topicSuffix(QString const& prefix, QString const& full) {
    if (prefix.isEmpty()) return full;
    QString p = prefix;
    if (!p.endsWith('/')) p += '/';
    if (full.startsWith(p)) return full.mid(p.size());
    return full;
}

} // namespace

class MqttClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread)
        , client(new QMqttClient) {

        // Parse brokerUri into host + port (Qt MQTT expects them separated).
        QUrl const url(cfg.brokerUri);
        if (url.isValid()) {
            client->setHostname(url.host().isEmpty()
                ? QStringLiteral("127.0.0.1") : url.host());
            client->setPort(quint16(url.port(1883)));
        } else {
            client->setHostname(QStringLiteral("127.0.0.1"));
            client->setPort(1883);
        }
        if (!cfg.clientId.isEmpty())  client->setClientId(cfg.clientId);
        if (!cfg.username.isEmpty())  client->setUsername(cfg.username);
        if (!cfg.password.isEmpty())  client->setPassword(cfg.password);
        client->setCleanSession(cfg.cleanSession);
        client->setKeepAlive(60);

        client->moveToThread(thread);
        thread->start();

        QObject::connect(client, &QMqttClient::stateChanged,
                         client, [this](QMqttClient::ClientState s) {
            auto const cur  = stateFromQt(s);
            auto const prev = state.exchange(cur, std::memory_order_acq_rel);
            if (cur == ConnectionState::Disconnected) clearCache();
            if (cur == ConnectionState::Connected && !subscribeWildcard()) {
                state.store(ConnectionState::Error, std::memory_order_release);
                client->disconnectFromHost();
                return;
            }
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
        QObject::connect(client, &QMqttClient::errorChanged,
                         client, [this](QMqttClient::ClientError e) {
            if (e == QMqttClient::NoError) return;
            clearCache();
            auto const message = QStringLiteral("MQTT error %1").arg(int(e));
            setLastError(message);
            auto const prev = state.exchange(ConnectionState::Error,
                                              std::memory_order_acq_rel);
            if (busPtr && prev == ConnectionState::Connected) {
                busPtr->publish(bus::TransportEvent{
                    cfg.id, bus::TransportEventKind::Disconnected, message});
            }
        });
        QObject::connect(client, &QMqttClient::messageReceived, client,
            [this](QByteArray const& payload, QMqttTopicName const& topic) {
                std::lock_guard lk(cacheMtx);
                cache[topicSuffix(cfg.topicPrefix, topic.name())] = payload;
            });
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        autoReconnect.store(false, std::memory_order_release);
        if (scheduler) scheduler->stopAsync();
        if (reconnectTimer) {
            detail::invokeBlocking(reconnectTimer, [this] {
                reconnectTimer->stop();
                delete reconnectTimer;
                reconnectTimer = nullptr;
            });
        }
        if (client) {
            detail::invokeBlocking(client, [this] {
                client->disconnectFromHost();
                delete client;
                client = nullptr;
            });
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    QString topicForAddress(int addr) const {
        return joinTopic(cfg.topicPrefix, cfg.topicTemplate.arg(addr));
    }

    void clearCache() {
        std::lock_guard lk(cacheMtx);
        cache.clear();
    }

    bool subscribeWildcard() {
        QString const wildcard = cfg.topicPrefix.isEmpty()
            ? QStringLiteral("#")
            : cfg.topicPrefix + (cfg.topicPrefix.endsWith('/')
                ? QStringLiteral("#") : QStringLiteral("/#"));
        if (client->subscribe(QMqttTopicFilter(wildcard), quint8(cfg.qos))) {
            return true;
        }
        setLastError(QStringLiteral("MQTT subscribe failed: %1").arg(wildcard));
        return false;
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
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QMqttClient*                                       client = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic<bool>                                  autoReconnect{false};
    std::atomic<bool>                                  stopping{false};
    std::atomic<int>                                   queuedWrites{0};
    static constexpr int                               maxQueuedWrites = 1024;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    QString                                            lastError;
    mutable std::mutex                                  cacheMtx;
    std::unordered_map<QString, QByteArray>             cache;
};

MqttClientTransport::MqttClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

MqttClientTransport::~MqttClientTransport() = default;

QString               MqttClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttClientTransport::kind()  const { return TransportKind::MqttClient; }
ConnectionState       MqttClientTransport::state() const { return m_impl->state.load(); }

sched::RequestScheduler& MqttClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
MqttClientTransport::connect() {
    if (QThread::currentThread() == m_impl->client->thread()) {
        return std::unexpected(QStringLiteral(
            "connect() cannot block the MQTT worker thread"));
    }
    if (state() == ConnectionState::Connected) {
        armReconnectIfConfigured();
        return {};
    }
    m_impl->setLastError({});
    m_impl->clearCache();
    detail::invokeBlocking(m_impl->client, [this] {
        m_impl->client->connectToHost();
    });

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) {
            armReconnectIfConfigured();
            return std::expected<void, QString>{};
        }
        if (s == ConnectionState::Error) {
            armReconnectIfConfigured();
            return std::expected<void, QString>(
                std::unexpect, m_impl->getLastError());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto const error = m_impl->getLastError();
    detail::invokeBlocking(m_impl->client, [this] {
        m_impl->client->disconnectFromHost();
    });
    armReconnectIfConfigured();
    return std::unexpected(error.isEmpty()
        ? QStringLiteral("MQTT connect timeout") : error);
}

void MqttClientTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    detail::invokeBlocking(m_impl->client, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->client->disconnectFromHost();
    });
}

void MqttClientTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.exchange(true, std::memory_order_acq_rel)) return;
    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;
    detail::invokeBlocking(impl->client, [impl, intervalMs] {
        if (impl->reconnectTimer) {
            if (!impl->reconnectTimer->isActive()) impl->reconnectTimer->start();
            return;
        }
        impl->reconnectTimer = new QTimer(impl->client);
        impl->reconnectTimer->setInterval(intervalMs);
        QObject::connect(impl->reconnectTimer, &QTimer::timeout, impl->client,
            [impl] {
                if (!impl->autoReconnect.load(std::memory_order_acquire)
                    || impl->stopping.load(std::memory_order_acquire)) return;
                auto const s = impl->state.load(std::memory_order_acquire);
                if (s == ConnectionState::Connected
                    || s == ConnectionState::Connecting) return;
                auto const actual = impl->client->state();
                if (actual == QMqttClient::Disconnected) {
                    impl->client->connectToHost();
                } else if (actual == QMqttClient::Connected) {
                    // Let disconnect finish; the next tick reconnects cleanly.
                    impl->client->disconnectFromHost();
                }
            });
        impl->reconnectTimer->start();
    });
}

ReadResult MqttClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    QList<quint16> out;
    out.reserve(req.count);
    std::lock_guard lk(m_impl->cacheMtx);
    for (int i = 0; i < req.count; ++i) {
        QString const suffix = m_impl->cfg.topicTemplate.arg(req.startAddress + i);
        auto it = m_impl->cache.find(suffix);
        if (it == m_impl->cache.end()) {
            result.errorMessage =
                QStringLiteral("MQTT topic has no cached value: %1").arg(suffix);
            return result;
        }
        bool ok = false;
        uint const value = QString::fromUtf8(it->second).toUInt(&ok);
        if (!ok || value > 65535U) {
            result.errorMessage =
                QStringLiteral("MQTT topic is not a uint16: %1").arg(suffix);
            return result;
        }
        out.append(quint16(value));
    }
    result.ok     = true;
    result.values = std::move(out);
    return result;
}

WriteResult MqttClientTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    detail::invokeBlocking(m_impl->client, [this, &batch, &result] {
        for (int i = 0; i < batch.values.size(); ++i) {
            QString const topic = m_impl->topicForAddress(batch.startAddress + i);
            QByteArray const payload = QByteArray::number(batch.values.at(i));
            auto id = m_impl->client->publish(QMqttTopicName(topic),
                                                payload,
                                                quint8(m_impl->cfg.qos));
            if (id == -1) {
                result.errorMessage =
                    QStringLiteral("MQTT publish failed @ %1").arg(topic);
                return;
            }
        }
        result.ok = true;
    });
    return result;
}

// Async publish — post to the client thread with QueuedConnection so the caller
// (the GUI tick) never parks. publish() is fire-and-forget at this layer (QoS
// acks land later via the broker); a publish-id of -1 is the only failure we can
// surface synchronously, so we report ok once all topics are enqueued.
void MqttClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, QStringLiteral("not connected")});
        return;
    }
    if (m_impl->stopping.load(std::memory_order_acquire)) {
        done(WriteResult{false, QStringLiteral("MQTT transport is stopping")});
        return;
    }
    int const queued = m_impl->queuedWrites.fetch_add(1, std::memory_order_acq_rel);
    if (queued >= m_impl->maxQueuedWrites) {
        m_impl->queuedWrites.fetch_sub(1, std::memory_order_acq_rel);
        done(WriteResult{false, QStringLiteral("MQTT write queue is full")});
        return;
    }
    auto* client = m_impl->client;
    QMetaObject::invokeMethod(client, [this, batch, done = std::move(done)]() {
        m_impl->queuedWrites.fetch_sub(1, std::memory_order_acq_rel);
        if (m_impl->stopping.load(std::memory_order_acquire)) {
            done(WriteResult{false, QStringLiteral("MQTT transport is stopping")});
            return;
        }
        for (int i = 0; i < batch.values.size(); ++i) {
            QString const topic = m_impl->topicForAddress(batch.startAddress + i);
            QByteArray const payload = QByteArray::number(batch.values.at(i));
            auto id = m_impl->client->publish(QMqttTopicName(topic), payload,
                                              quint8(m_impl->cfg.qos));
            if (id == -1) {
                done(WriteResult{false,
                    QStringLiteral("MQTT publish failed @ %1").arg(topic)});
                return;
            }
        }
        done(WriteResult{true, {}});
    }, Qt::QueuedConnection);
}

#else  // !CORE_HAS_MQTT_QT

class MqttClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c)), busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                                  cfg;
    bus::EventBus*                          busPtr;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>            state{ConnectionState::Disconnected};
};

MqttClientTransport::MqttClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}
MqttClientTransport::~MqttClientTransport() = default;

QString               MqttClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttClientTransport::kind()  const { return TransportKind::MqttClient; }
ConnectionState       MqttClientTransport::state() const { return m_impl->state.load(); }
sched::RequestScheduler& MqttClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString> MqttClientTransport::connect() {
    return std::unexpected(QStringLiteral(
        "MqttClientTransport disabled (CORE_BUILD_MQTT_QT=OFF)"));
}
void        MqttClientTransport::disconnect()                          {}
ReadResult  MqttClientTransport::read      (ReadRequest const&)       {
    ReadResult r; r.errorMessage = QStringLiteral("Qt MQTT disabled at build time");
    return r;
}
WriteResult MqttClientTransport::writeBatch(WriteBatch  const&)       {
    WriteResult r; r.errorMessage = QStringLiteral("Qt MQTT disabled at build time");
    return r;
}
void MqttClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    done(writeBatch(batch));
}

#endif

} // namespace core::transport
