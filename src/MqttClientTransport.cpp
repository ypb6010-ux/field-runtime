#include "core/transport/MqttClientTransport.h"

#include <atomic>
#include <utility>

#include "core/sched/SerialScheduler.h"

namespace core::transport {

class MqttClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c)), busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                              cfg;
    bus::EventBus*                      busPtr;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>        state{ConnectionState::Disconnected};
};

MqttClientTransport::MqttClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

MqttClientTransport::~MqttClientTransport() = default;

QString               MqttClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         MqttClientTransport::kind()  const { return TransportKind::MqttClient; }
ConnectionState       MqttClientTransport::state() const { return m_impl->state.load(); }

sched::RequestScheduler& MqttClientTransport::scheduler() { return *m_impl->scheduler; }

// Stub: real implementation requires `paho.mqtt.cpp`
// (https://github.com/eclipse/paho.mqtt.cpp).
std::expected<void, QString> MqttClientTransport::connect() {
    return std::unexpected(QStringLiteral(
        "MqttClientTransport: paho.mqtt.cpp library not yet vendored"));
}
void MqttClientTransport::disconnect() {}

ReadResult MqttClientTransport::read(ReadRequest const&) {
    ReadResult r;
    r.errorMessage = QStringLiteral("MqttClientTransport not implemented yet");
    return r;
}

WriteResult MqttClientTransport::writeBatch(WriteBatch const&) {
    WriteResult r;
    r.errorMessage = QStringLiteral("MqttClientTransport not implemented yet");
    return r;
}

} // namespace core::transport
