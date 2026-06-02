#include "core/transport/MqttPahoTransport.h"

#include <atomic>
#include <chrono>
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
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c))
        , busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler))
        , client(cfg.brokerUri.toStdString(), cfg.clientId.toStdString()) {
        client.set_callback(*this);
    }

    ~Impl() override {
        try {
            if (client.is_connected()) client.disconnect()->wait();
        } catch (...) { /* RAII tear-down — swallow */ }
    }

    QString topicForAddress(int addr) const {
        return joinTopic(cfg.topicPrefix, cfg.topicTemplate.arg(addr));
    }

    // mqtt::callback overrides — invoked from paho's dispatcher thread.
    void connected(std::string const&) override {
        auto const prev = state.exchange(ConnectionState::Connected,
                                          std::memory_order_acq_rel);
        if (busPtr && prev != ConnectionState::Connected) {
            busPtr->publish(bus::TransportEvent{
                cfg.id, bus::TransportEventKind::Connected, {}});
        }
    }

    void connection_lost(std::string const& cause) override {
        auto const prev = state.exchange(ConnectionState::Disconnected,
                                          std::memory_order_acq_rel);
        lastError = QString::fromStdString(cause);
        if (busPtr && prev == ConnectionState::Connected) {
            busPtr->publish(bus::TransportEvent{
                cfg.id, bus::TransportEventKind::Disconnected, lastError});
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

    Config                                            cfg;
    bus::EventBus*                                    busPtr = nullptr;
    std::unique_ptr<sched::RequestScheduler>           scheduler;
    mqtt::async_client                                 client;
    std::atomic<ConnectionState>                       state{ConnectionState::Disconnected};
    QString                                            lastError;
    mutable std::mutex                                  cacheMtx;
    std::unordered_map<QString, QByteArray>             cache;
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

        m_impl->state.store(ConnectionState::Connecting,
                             std::memory_order_release);
        auto tok = m_impl->client.connect(opts);
        if (!tok->wait_for(m_impl->cfg.connectTimeoutMs)) {
            m_impl->state.store(ConnectionState::Disconnected,
                                 std::memory_order_release);
            return std::unexpected(QStringLiteral("paho MQTT connect timeout"));
        }
        // Subscribe to wildcard so `read()` finds cached payloads.
        QString const wildcard = m_impl->cfg.topicPrefix.isEmpty()
            ? QStringLiteral("#")
            : m_impl->cfg.topicPrefix
                + (m_impl->cfg.topicPrefix.endsWith('/')
                    ? QStringLiteral("#") : QStringLiteral("/#"));
        m_impl->client.subscribe(wildcard.toStdString(),
                                  m_impl->cfg.qos)->wait();
        return {};
    } catch (mqtt::exception const& e) {
        m_impl->state.store(ConnectionState::Error,
                             std::memory_order_release);
        m_impl->lastError = QString::fromUtf8(e.what());
        return std::unexpected(m_impl->lastError);
    } catch (std::exception const& e) {
        return std::unexpected(QString::fromUtf8(e.what()));
    }
}

void MqttPahoTransport::disconnect() {
    try {
        if (m_impl->client.is_connected()) m_impl->client.disconnect()->wait();
    } catch (...) { /* swallow */ }
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
        if (it == m_impl->cache.end()) { out.append(0); continue; }
        bool ok = false;
        quint16 const v = quint16(QString::fromUtf8(it->second).toUInt(&ok));
        out.append(ok ? v : 0);
    }
    result.ok     = true;
    result.values = std::move(out);
    return result;
}

WriteResult MqttPahoTransport::writeBatch(WriteBatch const& batch) {
    WriteResult result;
    if (state() != ConnectionState::Connected) {
        result.errorMessage = QStringLiteral("not connected");
        return result;
    }
    try {
        for (int i = 0; i < batch.values.size(); ++i) {
            QString const topic = m_impl->topicForAddress(batch.startAddress + i);
            std::string  const payload =
                std::to_string(batch.values.at(i));
            auto pubTok = m_impl->client.publish(topic.toStdString(),
                                                  payload.data(),
                                                  payload.size(),
                                                  m_impl->cfg.qos,
                                                  /*retained=*/false);
            if (!pubTok->wait_for(m_impl->cfg.requestTimeoutMs)) {
                result.errorMessage =
                    QStringLiteral("paho publish timeout @ %1").arg(topic);
                return result;
            }
        }
        result.ok = true;
    } catch (mqtt::exception const& e) {
        result.errorMessage = QString::fromUtf8(e.what());
    }
    return result;
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

#endif

} // namespace core::transport
