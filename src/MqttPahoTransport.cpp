// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/MqttPahoTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef CORE_HAS_MQTT_PAHO
  #include <mqtt/async_client.h>
  #include <mqtt/callback.h>
  #include <mqtt/iaction_listener.h>
#endif

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/sched/SerialScheduler.h"

namespace core::transport {

#ifdef CORE_HAS_MQTT_PAHO

namespace {

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

class MqttPahoTransport::Impl : public mqtt::callback {
public:
    struct WriteJob {
        WriteBatch batch;
        Transport::WriteDone done;
    };

    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , client(cfg.brokerUri.toStdString(), cfg.clientId.toStdString()) {
        client.set_callback(*this);
        writeWorker = std::thread([this] { runWriteWorker(); });
    }

    ~Impl() override {
        std::deque<WriteJob> cancelled;
        {
            std::lock_guard lock(writeMutex);
            stopping.store(true, std::memory_order_release);
            cancelled.swap(writeQueue);
        }
        writeCv.notify_all();
        for (auto& job : cancelled) {
            try {
                job.done(WriteResult{false,
                    QStringLiteral("Paho MQTT transport is stopping")});
            } catch (...) {
            }
        }
        // Wake/cancel the transport worker before waiting for the scheduler:
        // queued scheduler jobs complete immediately, and the one active
        // publish observes `stopping` between registers. This bounds teardown
        // to at most one Paho request timeout instead of a whole large batch.
        if (scheduler) scheduler->stopAsync();
        if (writeWorker.joinable()) writeWorker.join();
        try {
            if (client.is_connected()) {
                auto token = client.disconnect(cfg.requestTimeoutMs);
                (void)token->wait_for(cfg.requestTimeoutMs);
            }
        } catch (...) { /* RAII tear-down — swallow */ }
    }

    QString topicForAddress(int addr) const {
        return joinTopic(cfg.topicPrefix, cfg.topicTemplate.arg(addr));
    }

    // mqtt::callback overrides — invoked from paho's dispatcher thread.
    void connected(std::string const&) override {
        auto const prev = state.exchange(ConnectionState::Connected,
                                          std::memory_order_acq_rel);
        // Automatic reconnect does not restore subscriptions for a clean
        // session, so subscribe again after every successful connection.
        try {
            QString const wildcard = cfg.topicPrefix.isEmpty()
                ? QStringLiteral("#")
                : cfg.topicPrefix + (cfg.topicPrefix.endsWith('/')
                    ? QStringLiteral("#") : QStringLiteral("/#"));
            client.subscribe(wildcard.toStdString(), cfg.qos);
        } catch (...) {
        }
        if (busPtr && prev != ConnectionState::Connected) {
            busPtr->publish(bus::TransportEvent{
                cfg.id, bus::TransportEventKind::Connected, {}});
        }
    }

    void connection_lost(std::string const& cause) override {
        auto const message = QString::fromStdString(cause);
        setLastError(message);
        auto const prev = state.exchange(ConnectionState::Disconnected,
                                          std::memory_order_acq_rel);
        clearCache();
        if (busPtr && prev == ConnectionState::Connected) {
            busPtr->publish(bus::TransportEvent{
                cfg.id, bus::TransportEventKind::Disconnected, message});
        }
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        if (!msg) return;
        QString const fullTopic = QString::fromStdString(msg->get_topic());
        QByteArray const payload(msg->get_payload().data(),
                                  int(msg->get_payload().size()));
        std::lock_guard lk(cacheMtx);
        cache[topicSuffix(cfg.topicPrefix, fullTopic)] = payload;
    }

    void setLastError(QString message) {
        std::lock_guard lock(errorMutex);
        lastError = std::move(message);
    }

    void clearCache() {
        std::lock_guard lk(cacheMtx);
        cache.clear();
    }

    QString getLastError() const {
        std::lock_guard lock(errorMutex);
        return lastError;
    }

    WriteResult performWrite(WriteBatch const& batch) {
        WriteResult result;
        if (state.load(std::memory_order_acquire) != ConnectionState::Connected) {
            result.errorMessage = QStringLiteral("not connected");
            return result;
        }
        try {
            for (int i = 0; i < batch.values.size(); ++i) {
                if (stopping.load(std::memory_order_acquire)) {
                    result.errorMessage =
                        QStringLiteral("Paho MQTT transport is stopping");
                    return result;
                }
                QString const topic = topicForAddress(batch.startAddress + i);
                std::string const payload = std::to_string(batch.values.at(i));
                auto token = client.publish(topic.toStdString(), payload.data(),
                                            payload.size(), cfg.qos,
                                            /*retained=*/false);
                if (!token->wait_for(cfg.requestTimeoutMs)) {
                    result.errorMessage =
                        QStringLiteral("paho publish timeout @ %1").arg(topic);
                    return result;
                }
            }
            result.ok = true;
        } catch (mqtt::exception const& e) {
            result.errorMessage = QString::fromUtf8(e.what());
        } catch (std::exception const& e) {
            result.errorMessage = QString::fromUtf8(e.what());
        }
        return result;
    }

    void enqueueWrite(WriteBatch batch, Transport::WriteDone done) {
        QString rejection;
        {
            std::lock_guard lock(writeMutex);
            if (stopping.load(std::memory_order_acquire)) {
                rejection = QStringLiteral("Paho MQTT transport is stopping");
            } else if (writeQueue.size() >= maxQueuedWrites) {
                rejection = QStringLiteral("Paho MQTT write queue is full");
            } else {
                writeQueue.push_back(WriteJob{std::move(batch), std::move(done)});
                writeCv.notify_one();
                return;
            }
        }
        try {
            done(WriteResult{false, std::move(rejection)});
        } catch (...) {
        }
    }

    void runWriteWorker() {
        for (;;) {
            WriteJob job;
            {
                std::unique_lock lock(writeMutex);
                writeCv.wait(lock, [this] {
                    return stopping.load(std::memory_order_acquire)
                        || !writeQueue.empty();
                });
                if (stopping.load(std::memory_order_acquire)
                    && writeQueue.empty()) return;
                job = std::move(writeQueue.front());
                writeQueue.pop_front();
            }
            auto result = performWrite(job.batch);
            try {
                job.done(std::move(result));
            } catch (...) {
            }
        }
    }

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    mqtt::async_client                                 client;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    mutable std::mutex                                 errorMutex;
    QString                                            lastError;
    mutable std::mutex                                  cacheMtx;
    std::unordered_map<QString, QByteArray>             cache;
    std::mutex                                         writeMutex;
    std::condition_variable                            writeCv;
    std::deque<WriteJob>                               writeQueue;
    std::thread                                        writeWorker;
    std::atomic_bool                                   stopping{false};
    static constexpr std::size_t                       maxQueuedWrites = 1024;
};

MqttPahoTransport::MqttPahoTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

MqttPahoTransport::~MqttPahoTransport() = default;

QString               MqttPahoTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttPahoTransport::kind()  const { return TransportKind::MqttPahoClient; }
ConnectionState       MqttPahoTransport::state() const { return m_impl->state.load(); }

sched::RequestScheduler& MqttPahoTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString>
MqttPahoTransport::connect() {
    try {
        m_impl->setLastError({});
        m_impl->clearCache();
        mqtt::connect_options opts;
        opts.set_clean_session(m_impl->cfg.cleanSession);
        opts.set_keep_alive_interval(60);
        if (!m_impl->cfg.username.isEmpty()) {
            opts.set_user_name(m_impl->cfg.username.toStdString());
        }
        if (!m_impl->cfg.password.isEmpty()) {
            opts.set_password(m_impl->cfg.password.toStdString());
        }
        opts.set_connect_timeout(std::chrono::milliseconds(
            m_impl->cfg.connectTimeoutMs));
        if (m_impl->cfg.reconnectIntervalMs > 0) {
            int const retrySeconds = std::max(
                1, (m_impl->cfg.reconnectIntervalMs + 999) / 1000);
            opts.set_automatic_reconnect(retrySeconds, retrySeconds);
        }

        m_impl->state.store(ConnectionState::Connecting,
                             std::memory_order_release);
        auto tok = m_impl->client.connect(opts);
        if (!tok->wait_for(m_impl->cfg.connectTimeoutMs)) {
            auto const message = QStringLiteral("paho MQTT connect timeout");
            m_impl->setLastError(message);
            m_impl->state.store(ConnectionState::Error,
                                std::memory_order_release);
            return std::unexpected(message);
        }
        // Subscribe to wildcard so `read()` finds cached payloads.
        QString const wildcard = m_impl->cfg.topicPrefix.isEmpty()
            ? QStringLiteral("#")
            : m_impl->cfg.topicPrefix
                + (m_impl->cfg.topicPrefix.endsWith('/')
                    ? QStringLiteral("#") : QStringLiteral("/#"));
        auto subscribeToken = m_impl->client.subscribe(wildcard.toStdString(),
                                                       m_impl->cfg.qos);
        if (!subscribeToken->wait_for(m_impl->cfg.requestTimeoutMs)) {
            auto const message = QStringLiteral("paho MQTT subscribe timeout");
            m_impl->setLastError(message);
            m_impl->state.store(ConnectionState::Error,
                                std::memory_order_release);
            try {
                auto token = m_impl->client.disconnect(m_impl->cfg.requestTimeoutMs);
                (void)token->wait_for(m_impl->cfg.requestTimeoutMs);
            } catch (...) {
            }
            return std::unexpected(message);
        }
        return {};
    } catch (mqtt::exception const& e) {
        auto const message = QString::fromUtf8(e.what());
        m_impl->setLastError(message);
        m_impl->state.store(ConnectionState::Error,
                             std::memory_order_release);
        return std::unexpected(message);
    } catch (std::exception const& e) {
        auto const message = QString::fromUtf8(e.what());
        m_impl->setLastError(message);
        m_impl->state.store(ConnectionState::Error,
                            std::memory_order_release);
        return std::unexpected(message);
    }
}

void MqttPahoTransport::disconnect() {
    try {
        if (m_impl->client.is_connected()) {
            auto token = m_impl->client.disconnect(m_impl->cfg.requestTimeoutMs);
            (void)token->wait_for(m_impl->cfg.requestTimeoutMs);
        }
    } catch (...) { /* swallow */ }
    m_impl->clearCache();
    m_impl->state.store(ConnectionState::Disconnected,
                         std::memory_order_release);
}

ReadResult MqttPahoTransport::read(ReadRequest const& req) {
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

WriteResult MqttPahoTransport::writeBatch(WriteBatch const& batch) {
    return m_impl->performWrite(batch);
}

void MqttPahoTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    if (state() != ConnectionState::Connected) {
        done(WriteResult{false, QStringLiteral("not connected")});
        return;
    }
    m_impl->enqueueWrite(batch, std::move(done));
}

#else  // !CORE_HAS_MQTT_PAHO

class MqttPahoTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c)), busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                                  cfg;
    bus::EventBus*                          busPtr;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>            state{ConnectionState::Disconnected};
};

MqttPahoTransport::MqttPahoTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}
MqttPahoTransport::~MqttPahoTransport() = default;

QString               MqttPahoTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttPahoTransport::kind()  const { return TransportKind::MqttPahoClient; }
ConnectionState       MqttPahoTransport::state() const { return m_impl->state.load(); }
sched::RequestScheduler& MqttPahoTransport::scheduler() { return *m_impl->scheduler; }

std::expected<void, QString> MqttPahoTransport::connect() {
    return std::unexpected(QStringLiteral(
        "MqttPahoTransport disabled (CORE_BUILD_MQTT_PAHO=OFF)"));
}
void        MqttPahoTransport::disconnect()                          {}
ReadResult  MqttPahoTransport::read      (ReadRequest const&)       {
    ReadResult r; r.errorMessage = QStringLiteral("paho MQTT disabled at build time");
    return r;
}
WriteResult MqttPahoTransport::writeBatch(WriteBatch  const&)       {
    WriteResult r; r.errorMessage = QStringLiteral("paho MQTT disabled at build time");
    return r;
}
void MqttPahoTransport::writeAsync(WriteBatch const& batch, WriteDone done) {
    done(writeBatch(batch));
}

#endif

} // namespace core::transport
