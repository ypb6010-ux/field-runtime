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
#include "TransportStatusTracker.h"

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

QString qs(std::string const& s) {
    return QString::fromStdString(s);
}

} // namespace

class MqttClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , statusTracker(
              cfg.id, TransportKind::MqttClient, b, {},
              EndpointInfo{
                  QUrl(qs(cfg.brokerUri)).host().toStdString(),
                  static_cast<std::uint16_t>(
                      QUrl(qs(cfg.brokerUri)).port(1883))})
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , thread(new QThread)
        , client(new QMqttClient) {

        // Parse brokerUri into host + port (Qt MQTT expects them separated).
        QUrl const url(qs(cfg.brokerUri));
        if (url.isValid()) {
            client->setHostname(url.host().isEmpty()
                ? QStringLiteral("127.0.0.1") : url.host());
            client->setPort(quint16(url.port(1883)));
        } else {
            client->setHostname(QStringLiteral("127.0.0.1"));
            client->setPort(1883);
        }
        if (!cfg.clientId.empty())  client->setClientId(qs(cfg.clientId));
        if (!cfg.username.empty())  client->setUsername(qs(cfg.username));
        if (!cfg.password.empty())  client->setPassword(qs(cfg.password));
        client->setCleanSession(cfg.cleanSession);
        client->setKeepAlive(60);

        client->moveToThread(thread);
        thread->start();

        QObject::connect(client, &QMqttClient::stateChanged,
                         client, [this](QMqttClient::ClientState s) {
            auto const cur  = stateFromQt(s);
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
            if (effective == ConnectionState::Connected
                && prev != ConnectionState::Connected) {
                // A clean-session reconnect discards broker-side
                // subscriptions. Subscribe on every Connected transition so
                // the read cache keeps working after automatic reconnects.
                QString const prefix = qs(cfg.topicPrefix);
                QString const wildcard = prefix.isEmpty()
                    ? QStringLiteral("#")
                    : prefix
                        + (prefix.endsWith('/')
                            ? QStringLiteral("#") : QStringLiteral("/#"));
                client->subscribe(QMqttTopicFilter(wildcard),
                                  quint8(cfg.qos));
            }
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
        QObject::connect(client, &QMqttClient::errorChanged,
                         client, [this](QMqttClient::ClientError e) {
            if (e == QMqttClient::NoError) return;
            auto const message =
                QStringLiteral("MQTT error %1").arg(int(e)).toStdString();
            setLastError(message);
            state.store(ConnectionState::Error, std::memory_order_release);
            statusTracker.update(ConnectionState::Error, message);
        });
        QObject::connect(client, &QMqttClient::messageReceived, client,
            [this](QByteArray const& payload, QMqttTopicName const& topic) {
                std::lock_guard lk(cacheMtx);
                cache[topicSuffix(qs(cfg.topicPrefix), topic.name())] = payload;
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
        return joinTopic(qs(cfg.topicPrefix), qs(cfg.topicTemplate).arg(addr));
    }

    void setLastError(std::string message) {
        std::lock_guard lock(errorMtx);
        lastError = std::move(message);
    }

    std::string getLastError() const {
        std::lock_guard lock(errorMtx);
        return lastError;
    }

    void clearCache() {
        std::lock_guard lock(cacheMtx);
        cache.clear();
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    detail::TransportStatusTracker                    statusTracker;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QMqttClient*                                       client = nullptr;
    QTimer*                                            reconnectTimer = nullptr;
    std::atomic_bool                                   autoReconnect{false};
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMtx;
    std::string                                        lastError;
    mutable std::mutex                                  cacheMtx;
    std::unordered_map<QString, QByteArray>             cache;
};

MqttClientTransport::MqttClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

MqttClientTransport::~MqttClientTransport() = default;

std::string           MqttClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttClientTransport::kind()  const { return TransportKind::MqttClient; }
ConnectionState       MqttClientTransport::state() const { return m_impl->state.load(); }
TransportStatus       MqttClientTransport::status() const {
    return m_impl->statusTracker.snapshot();
}

sched::RequestScheduler& MqttClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, std::string>
MqttClientTransport::connect() {
    if (QThread::currentThread() == m_impl->client->thread()) {
        return std::unexpected(
            std::string("connect() cannot block the MQTT worker thread"));
    }
    m_impl->setLastError({});
    m_impl->clearCache();
    m_impl->state.store(ConnectionState::Connecting,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Connecting);
    detail::invokeBlocking(m_impl->client, [this] {
        m_impl->client->connectToHost();
    });

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) {
            armReconnectIfConfigured();
            return std::expected<void, std::string>{};
        }
        if (s == ConnectionState::Error) {
            armReconnectIfConfigured();
            return std::expected<void, std::string>(
                std::unexpect, m_impl->getLastError());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto const error = m_impl->getLastError();
    auto const message =
        error.empty() ? std::string("MQTT connect timeout") : error;
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    armReconnectIfConfigured();
    return std::unexpected(message);
}

void MqttClientTransport::disconnect() {
    m_impl->autoReconnect.store(false, std::memory_order_release);
    m_impl->setLastError({});
    m_impl->clearCache();
    detail::invokeBlocking(m_impl->client, [this] {
        if (m_impl->reconnectTimer) m_impl->reconnectTimer->stop();
        m_impl->client->disconnectFromHost();
    });
    m_impl->state.store(ConnectionState::Disconnected,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}

void MqttClientTransport::armReconnectIfConfigured() {
    if (m_impl->cfg.reconnectIntervalMs <= 0) return;
    if (m_impl->autoReconnect.exchange(
            true, std::memory_order_acq_rel)) {
        return;
    }
    auto* impl = m_impl.get();
    int const intervalMs = m_impl->cfg.reconnectIntervalMs;
    detail::invokeBlocking(impl->client, [impl, intervalMs] {
        if (impl->reconnectTimer) {
            if (!impl->reconnectTimer->isActive()) {
                impl->reconnectTimer->start();
            }
            return;
        }
        impl->reconnectTimer = new QTimer(impl->client);
        impl->reconnectTimer->setInterval(intervalMs);
        impl->reconnectTimer->setSingleShot(false);
        QObject::connect(
            impl->reconnectTimer, &QTimer::timeout, impl->client,
            [impl] {
                if (!impl->autoReconnect.load(
                        std::memory_order_acquire)) {
                    return;
                }
                auto const current =
                    impl->state.load(std::memory_order_acquire);
                if (current == ConnectionState::Connected
                    || current == ConnectionState::Connecting) {
                    return;
                }
                impl->client->connectToHost();
            });
        impl->reconnectTimer->start();
    });
}

ReadResult MqttClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = "not connected";
        return result;
    }
    core::RegisterWords out;
    out.reserve(req.count);
    std::lock_guard lk(m_impl->cacheMtx);
    for (int i = 0; i < req.count; ++i) {
        QString const suffix = qs(m_impl->cfg.topicTemplate).arg(req.startAddress + i);
        auto it = m_impl->cache.find(suffix);
        if (it == m_impl->cache.end()) {
            result.errorMessage =
                QStringLiteral("MQTT topic has no cached value: %1")
                    .arg(suffix).toStdString();
            return result;
        }
        bool ok = false;
        auto const value = QString::fromUtf8(it->second).toUInt(&ok);
        if (!ok || value > 65535U) {
            result.errorMessage =
                QStringLiteral("MQTT topic is not a uint16: %1")
                    .arg(suffix).toStdString();
            return result;
        }
        out.push_back(static_cast<std::uint16_t>(value));
    }
    result.ok     = true;
    result.values = std::move(out);
    return result;
}

WriteResult MqttClientTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (QThread::currentThread() == m_impl->client->thread()) {
        result.errorMessage =
            "synchronous write cannot run on the MQTT worker thread";
        return result;
    }
    if (state() != ConnectionState::Connected) {
        result.errorMessage = "not connected";
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
                    QStringLiteral("MQTT publish failed @ %1").arg(topic).toStdString();
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
        done(WriteResult{false, "not connected"});
        return;
    }
    auto* client = m_impl->client;
    QMetaObject::invokeMethod(client, [this, batch, done = std::move(done)]() {
        for (int i = 0; i < batch.values.size(); ++i) {
            QString const topic = m_impl->topicForAddress(batch.startAddress + i);
            QByteArray const payload = QByteArray::number(batch.values.at(i));
            auto id = m_impl->client->publish(QMqttTopicName(topic), payload,
                                              quint8(m_impl->cfg.qos));
            if (id == -1) {
                done(WriteResult{false,
                    QStringLiteral("MQTT publish failed @ %1").arg(topic).toStdString()});
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
        : cfg(std::move(c))
        , statusTracker(
              cfg.id, TransportKind::MqttClient, b, {},
              EndpointInfo{
                  QUrl(QString::fromStdString(cfg.brokerUri))
                      .host().toStdString(),
                  static_cast<std::uint16_t>(
                      QUrl(QString::fromStdString(cfg.brokerUri))
                          .port(1883))})
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                                  cfg;
    detail::TransportStatusTracker          statusTracker;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>            state{ConnectionState::Disconnected};
};

MqttClientTransport::MqttClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}
MqttClientTransport::~MqttClientTransport() = default;

std::string           MqttClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttClientTransport::kind()  const { return TransportKind::MqttClient; }
ConnectionState       MqttClientTransport::state() const { return m_impl->state.load(); }
TransportStatus       MqttClientTransport::status() const {
    return m_impl->statusTracker.snapshot();
}
sched::RequestScheduler& MqttClientTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, std::string> MqttClientTransport::connect() {
    auto const message =
        std::string("MqttClientTransport disabled (CORE_BUILD_MQTT_QT=OFF)");
    m_impl->state.store(ConnectionState::Error, std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Error, message);
    return std::unexpected(message);
}
void MqttClientTransport::disconnect() {
    m_impl->state.store(ConnectionState::Disconnected,
                        std::memory_order_release);
    m_impl->statusTracker.update(ConnectionState::Disconnected);
}

void MqttClientTransport::armReconnectIfConfigured() {
    // Qt MQTT support is disabled in this build.
}
ReadResult  MqttClientTransport::read      (ReadRequest const&)       {
    ReadResult r; r.errorMessage = "Qt MQTT disabled at build time";
    return r;
}
WriteResult MqttClientTransport::writeBatch(WriteBatch  const&)       {
    WriteResult r; r.errorMessage = "Qt MQTT disabled at build time";
    return r;
}
void MqttClientTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    done(writeBatch(batch));
}

#endif

} // namespace core::transport
