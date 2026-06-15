// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <string>

#include "GatewayAsio.h"

#include "core/config/ConfigSchema.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/Transport.h"

namespace core::gateway {

class StubTransport final : public transport::Transport {
public:
    StubTransport(config::TransportConfig cfg, gateway_asio::io_context& io);
    ~StubTransport() override;

    std::string id() const override;
    transport::TransportKind kind() const override;
    transport::ConnectionState state() const override;

    std::expected<void, std::string> connect() override;
    void disconnect() override;

    sched::RequestScheduler& scheduler() override;

    transport::ReadResult read(transport::ReadRequest const& req) override;
    transport::WriteResult writeBatch(transport::WriteBatch const& batch) override;

private:
    config::TransportConfig m_cfg;
    gateway_asio::io_context* m_io = nullptr;
    std::unique_ptr<sched::RequestScheduler> m_scheduler;
    std::atomic<transport::ConnectionState> m_state{transport::ConnectionState::Disconnected};
    std::atomic<std::uint16_t> m_counter{0};
    mutable std::mutex m_mtx;
    core::RegisterWords m_lastWrite;
};

} // namespace core::gateway
