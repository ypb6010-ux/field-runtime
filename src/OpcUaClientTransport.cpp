#include "core/transport/OpcUaClientTransport.h"

#include <atomic>
#include <utility>

#include "core/sched/SerialScheduler.h"

namespace core::transport {

class OpcUaClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c)), busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                              cfg;
    bus::EventBus*                      busPtr;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>        state{ConnectionState::Disconnected};
};

OpcUaClientTransport::OpcUaClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

OpcUaClientTransport::~OpcUaClientTransport() = default;

QString               OpcUaClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         OpcUaClientTransport::kind()  const { return TransportKind::OpcUaClient; }
ConnectionState       OpcUaClientTransport::state() const { return m_impl->state.load(); }

sched::RequestScheduler& OpcUaClientTransport::scheduler() { return *m_impl->scheduler; }

// Stub: real implementation requires `open62541` (https://open62541.org/) —
// pin the major version in Core/CMakeLists.txt and add an `open62541::open62541`
// linkage once the dependency is provisioned.
std::expected<void, QString> OpcUaClientTransport::connect() {
    return std::unexpected(QStringLiteral(
        "OpcUaClientTransport: open62541 library not yet vendored"));
}
void OpcUaClientTransport::disconnect() {}

ReadResult OpcUaClientTransport::read(ReadRequest const&) {
    ReadResult r;
    r.errorMessage = QStringLiteral("OpcUaClientTransport not implemented yet");
    return r;
}

WriteResult OpcUaClientTransport::writeBatch(WriteBatch const&) {
    WriteResult r;
    r.errorMessage = QStringLiteral("OpcUaClientTransport not implemented yet");
    return r;
}

} // namespace core::transport
