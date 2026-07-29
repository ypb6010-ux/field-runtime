// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "StubTransport.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace core::gateway {

StubTransport::StubTransport(config::TransportConfig cfg,
                             gateway_asio::io_context& io)
    : m_cfg(std::move(cfg))
    , m_io(&io)
    , m_scheduler(sched::makeScheduler(m_cfg.scheduler)) {
    m_scheduler->setDelayFn([this](int ms, std::function<void()> fn) {
        auto timer = std::make_shared<gateway_asio::steady_timer>(*m_io);
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([timer, fn = std::move(fn)](auto const& ec) mutable {
            if (!ec) fn();
        });
    });
}

StubTransport::~StubTransport() {
    disconnect();
}

std::string StubTransport::id() const {
    return m_cfg.id;
}

transport::TransportKind StubTransport::kind() const {
    return m_cfg.kind;
}

transport::ConnectionState StubTransport::state() const {
    return m_state.load(std::memory_order_acquire);
}

std::expected<void, std::string> StubTransport::connect() {
    if (m_scheduler) m_scheduler->startAsync();
    m_state.store(transport::ConnectionState::Connected, std::memory_order_release);
    return {};
}

void StubTransport::disconnect() {
    m_state.store(transport::ConnectionState::Disconnected, std::memory_order_release);
    if (m_scheduler) m_scheduler->stopAsync();
}

sched::RequestScheduler& StubTransport::scheduler() {
    return *m_scheduler;
}

transport::ReadResult StubTransport::read(transport::ReadRequest const& req) {
    transport::ReadResult out;
    out.startAddress = req.startAddress;
    if (state() != transport::ConnectionState::Connected) {
        out.errorMessage = "stub transport is not connected";
        return out;
    }

    out.ok = true;
    out.values.reserve(req.count);
    auto const base = m_counter.fetch_add(1, std::memory_order_acq_rel);
    for (int i = 0; i < req.count; i++) {
        out.values.push_back(std::uint16_t(base + req.startAddress + i));
    }
    return out;
}

transport::WriteResult StubTransport::writeBatch(transport::WriteBatch const& batch) {
    if (state() != transport::ConnectionState::Connected) {
        return {false, "stub transport is not connected"};
    }

    std::lock_guard lk(m_mtx);
    m_lastWrite = batch.values;
    return {true, {}};
}

} // namespace core::gateway
