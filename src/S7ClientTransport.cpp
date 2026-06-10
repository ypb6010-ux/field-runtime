// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/transport/S7ClientTransport.h"

#include <atomic>
#include <utility>

#include "core/sched/SerialScheduler.h"

namespace core::transport {

class S7ClientTransport::Impl {
public:
    Impl(Config c, bus::EventBus* b)
        : cfg(std::move(c)), busPtr(b)
        , scheduler(sched::makeScheduler(cfg.scheduler)) {}
    Config                              cfg;
    bus::EventBus*                      busPtr;
    std::unique_ptr<sched::RequestScheduler> scheduler;
    std::atomic<ConnectionState>        state{ConnectionState::Disconnected};
};

S7ClientTransport::S7ClientTransport(Config cfg, bus::EventBus* bus)
    : m_impl(std::make_unique<Impl>(std::move(cfg), bus)) {}

S7ClientTransport::~S7ClientTransport() = default;

QString               S7ClientTransport::id()    const { return m_impl->cfg.id; }
TransportKind         S7ClientTransport::kind()  const { return TransportKind::S7Client; }
ConnectionState       S7ClientTransport::state() const { return m_impl->state.load(); }

sched::RequestScheduler& S7ClientTransport::scheduler() { return *m_impl->scheduler; }

// Stub: real implementation requires `snap7` (http://snap7.sourceforge.net/).
std::expected<void, QString> S7ClientTransport::connect() {
    return std::unexpected(QStringLiteral(
        "S7ClientTransport: snap7 library not yet vendored"));
}
void S7ClientTransport::disconnect() {}

ReadResult S7ClientTransport::read(ReadRequest const&) {
    ReadResult r;
    r.errorMessage = QStringLiteral("S7ClientTransport not implemented yet");
    return r;
}

WriteResult S7ClientTransport::writeBatch(WriteBatch const&) {
    WriteResult r;
    r.errorMessage = QStringLiteral("S7ClientTransport not implemented yet");
    return r;
}

} // namespace core::transport
