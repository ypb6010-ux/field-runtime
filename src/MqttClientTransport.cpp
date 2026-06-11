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
            lastError = QStringLiteral("MQTT error %1").arg(int(e));
        });
        QObject::connect(client, &QMqttClient::messageReceived, client,
            [this](QByteArray const& payload, QMqttTopicName const& topic) {
                std::lock_guard lk(cacheMtx);
                cache[topicSuffix(cfg.topicPrefix, topic.name())] = payload;
            });
    }

    ~Impl() {
        if (scheduler) scheduler->stopAsync();
        if (client) {
            QMetaObject::invokeMethod(client, [this] {
                client->disconnectFromHost();
                delete client;
                client = nullptr;
            }, Qt::BlockingQueuedConnection);
        }
        thread->quit();
        thread->wait();
        delete thread;
    }

    QString topicForAddress(int addr) const {
        return joinTopic(cfg.topicPrefix, cfg.topicTemplate.arg(addr));
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    QThread*                                           thread = nullptr;
    QMqttClient*                                       client = nullptr;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
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
    QMetaObject::invokeMethod(m_impl->client, [this] {
        m_impl->client->connectToHost();
    }, Qt::BlockingQueuedConnection);

    auto const deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(m_impl->cfg.connectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto s = state();
        if (s == ConnectionState::Connected) {
            // Subscribe to the wildcard topic so `read()` finds cached values.
            QMetaObject::invokeMethod(m_impl->client, [this] {
                QString const wildcard = m_impl->cfg.topicPrefix.isEmpty()
                    ? QStringLiteral("#")
                    : m_impl->cfg.topicPrefix
                        + (m_impl->cfg.topicPrefix.endsWith('/')
                            ? QStringLiteral("#") : QStringLiteral("/#"));
                m_impl->client->subscribe(QMqttTopicFilter(wildcard),
                                            quint8(m_impl->cfg.qos));
            }, Qt::BlockingQueuedConnection);
            return std::expected<void, QString>{};
        }
        if (s == ConnectionState::Error)
            return std::expected<void, QString>(
                std::unexpect, m_impl->lastError);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::unexpected(QStringLiteral("MQTT connect timeout"));
}

void MqttClientTransport::disconnect() {
    QMetaObject::invokeMethod(m_impl->client, [this] {
        m_impl->client->disconnectFromHost();
    }, Qt::BlockingQueuedConnection);
}

ReadResult MqttClientTransport::read(ReadRequest const& req) {
    ReadResult result;
    result.startAddress = req.startAddress;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    core::RegisterWords out;
    out.reserve(req.count);
    std::lock_guard lk(m_impl->cacheMtx);
    for (int i = 0; i < req.count; ++i) {
        QString const suffix = m_impl->cfg.topicTemplate.arg(req.startAddress + i);
        auto it = m_impl->cache.find(suffix);
        if (it == m_impl->cache.end()) {
            // Missing cache entry — treat as zero so PollRange can still
            // run and surface the missing topic via subsequent updates.
            out.push_back(0);
            continue;
        }
        bool ok = false;
        quint16 const v = quint16(QString::fromUtf8(it->second).toUInt(&ok));
        out.push_back(ok ? v : 0);
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
    QMetaObject::invokeMethod(m_impl->client, [this, &batch, &result] {
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
    }, Qt::BlockingQueuedConnection);
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
    auto* client = m_impl->client;
    QMetaObject::invokeMethod(client, [this, batch, done = std::move(done)]() {
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
