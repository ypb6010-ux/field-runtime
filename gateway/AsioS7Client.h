// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "GatewayAsio.h"
#include "RegisterSnapshotSource.h"

#include "core/config/ConfigSchema.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/Transport.h"

namespace core::gateway {

// No-Qt Siemens S7 client (ISO-on-TCP) over snap7. snap7's client API is
// synchronous and blocking, so — exactly like AsioOpcUaClient wraps open62541 —
// it is confined to a dedicated worker io_context/thread; completions are posted
// back to the main gateway io thread. Scheduler timing runs on the main thread.
//
// Addressing (v1): table HoldingRegister -> S7 area DB (DB number from config
// `db`), InputRegister -> area PE (I). `startAddress` = BYTE offset in the area;
// `count` = number of 16-bit words; reads/writes `count*2` bytes big-endian
// (S7 is big-endian), matching DBW<byte>/DBD<byte> conventions.
class AsioS7Client final : public transport::Transport,
                           public RegisterSnapshotSource {
public:
    AsioS7Client(config::TransportConfig cfg, gateway_asio::io_context& mainIo);
    ~AsioS7Client() override;

    std::string id() const override;
    transport::TransportKind kind() const override;
    transport::ConnectionState state() const override;

    std::expected<void, std::string> connect() override;
    void disconnect() override;

    sched::RequestScheduler& scheduler() override;

    transport::ReadResult read(transport::ReadRequest const& req) override;
    transport::WriteResult writeBatch(transport::WriteBatch const& batch) override;

    void readAsync(transport::ReadRequest const& req, ReadDone done) override;
    void writeAsync(transport::WriteBatch const& batch, WriteDone done) override;

    core::RegisterWords snapshotHoldingRegisters(int start, int count) const override;

private:
    transport::ReadResult readOnWorker(transport::ReadRequest const& req);
    transport::WriteResult writeOnWorker(transport::WriteBatch const& batch);
    void cacheHoldingRegisters(int startAddress, core::RegisterWords const& values);

    config::TransportConfig m_cfg;
    gateway_asio::io_context* m_mainIo = nullptr;
    std::unique_ptr<sched::RequestScheduler> m_scheduler;
    std::atomic<transport::ConnectionState> m_state{transport::ConnectionState::Disconnected};

    gateway_asio::io_context m_workerIo;
    gateway_asio::executor_work_guard<gateway_asio::io_context::executor_type> m_workGuard;
    std::thread m_workerThread;
    std::uintptr_t m_client = 0;   // snap7 S7Object (uintptr_t), touched only on the worker thread

    mutable std::mutex m_cacheMtx;
    std::map<int, std::uint16_t> m_hrCache;
};

} // namespace core::gateway
