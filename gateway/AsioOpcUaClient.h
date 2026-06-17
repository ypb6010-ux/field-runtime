// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <thread>

#include "GatewayAsio.h"

#include "core/config/ConfigSchema.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/Transport.h"

// Forward declaration to keep the open62541 C headers out of this header.
struct UA_Client;

namespace core::gateway {

// AsioOpcUaClient — Qt-free OPC UA southbound transport (open62541).
//
// open62541's client is a synchronous C library, so it is confined to a
// dedicated worker io_context/thread; it never runs on the gateway's main io
// thread. A Modbus-style ReadRequest{startAddress, count} maps to N node reads
// (nodeId = node_id_template with "%1" replaced by the address); each node
// Value is reduced to a u16 register word so the existing datapoint/codec
// pipeline (scale / enum / word-order) is reused unchanged. Read completions
// are posted back to the main io thread, so datapoint/bus/MQTT work stays
// single-threaded exactly like the Modbus path.
class AsioOpcUaClient final : public transport::Transport {
public:
    AsioOpcUaClient(config::TransportConfig cfg, gateway_asio::io_context& mainIo);
    ~AsioOpcUaClient() override;

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

private:
    // Runs on the worker thread only: performs the per-node OPC UA reads.
    transport::ReadResult readOnWorker(transport::ReadRequest const& req);
    std::string nodeIdFor(int address) const;

    config::TransportConfig m_cfg;
    gateway_asio::io_context* m_mainIo = nullptr;
    std::unique_ptr<sched::RequestScheduler> m_scheduler;
    std::atomic<transport::ConnectionState> m_state{transport::ConnectionState::Disconnected};

    gateway_asio::io_context m_workerIo;
    gateway_asio::executor_work_guard<gateway_asio::io_context::executor_type> m_workGuard;
    std::thread m_workerThread;
    UA_Client* m_client = nullptr;   // created/used/destroyed on the worker thread
};

} // namespace core::gateway
