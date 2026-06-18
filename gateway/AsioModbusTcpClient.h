// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "GatewayAsio.h"
#include "RegisterSnapshotSource.h"

#include "core/config/ConfigSchema.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/Transport.h"

namespace core::gateway {

class AsioModbusTcpClient final : public transport::Transport,
                                  public RegisterSnapshotSource {
public:
    AsioModbusTcpClient(config::TransportConfig cfg, gateway_asio::io_context& io);
    ~AsioModbusTcpClient() override;

    std::string id() const override;
    transport::TransportKind kind() const override;
    transport::ConnectionState state() const override;

    std::expected<void, std::string> connect() override;
    void disconnect() override;

    sched::RequestScheduler& scheduler() override;

    transport::ReadResult read(transport::ReadRequest const& req) override;
    transport::WriteResult writeBatch(transport::WriteBatch const& batch) override;

    // Non-blocking I/O — this is the path the gateway poll/bridge modules use
    // (PollRange::driveTick / SinkWindow::driveTick go through readAsync /
    // writeAsync). Each request is bounded by `requestTimeoutMs`: a deaf PLC
    // fails the request without ever parking the single io thread, so the
    // control socket / MQTT / other transports keep running.
    void readAsync(transport::ReadRequest const& req, ReadDone done) override;
    void writeAsync(transport::WriteBatch const& batch, WriteDone done) override;

    // Latest raw holding-register words observed by polling, indexed by
    // absolute PLC address. Bridge mirroring copies these RAW register words to
    // the operator-box server table (it must not re-encode decoded engineering
    // values). Unknown addresses read back as 0.
    core::RegisterWords snapshotHoldingRegisters(int start, int count) const override;

private:
    // Completion form: (response ADU, error). On error the ADU is empty and the
    // socket has been closed / state moved to Error.
    using TransactDone = std::function<void(std::vector<std::uint8_t>, std::string)>;

    std::vector<std::uint8_t> transact(std::vector<std::uint8_t> const& request,
                                       std::string& error);
    void transactAsync(std::shared_ptr<std::vector<std::uint8_t>> request,
                       TransactDone done);
    void cacheHoldingRegisters(int startAddress, core::RegisterWords const& values);
    std::uint16_t nextTransactionId();
    void closeSocketLocked();
    void failSocketAsync(std::string const& reason);

    config::TransportConfig m_cfg;
    gateway_asio::io_context* m_io = nullptr;
    gateway_asio::ip::tcp::socket m_socket;
    std::unique_ptr<sched::RequestScheduler> m_scheduler;
    std::atomic<transport::ConnectionState> m_state{transport::ConnectionState::Disconnected};
    std::atomic<std::uint16_t> m_transactionId{1};
    std::mutex m_socketMtx;
    mutable std::mutex m_cacheMtx;
    std::map<int, std::uint16_t> m_hrCache;
};

} // namespace core::gateway
