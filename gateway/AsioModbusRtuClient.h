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

#include "core/config/ConfigSchema.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/Transport.h"

namespace core::gateway {

// No-Qt Modbus RTU master over an asio serial_port. Mirrors AsioModbusTcpClient
// but frames ADUs as [unitId][PDU][CRC16] (no MBAP/transaction id) and recovers
// response length from the function code instead of a length prefix. Half-duplex
// serialisation is provided by the owning scheduler; each request is bounded by
// `requestTimeoutMs` so a silent slave never parks the single gateway io thread.
class AsioModbusRtuClient final : public transport::Transport {
public:
    AsioModbusRtuClient(config::TransportConfig cfg, gateway_asio::io_context& io);
    ~AsioModbusRtuClient() override;

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

    // Latest raw holding-register words observed by polling, indexed by absolute
    // address. Bridge mirroring copies these RAW words; unknown addresses read 0.
    core::RegisterWords snapshotHoldingRegisters(int start, int count) const;

private:
    // (response ADU, error). On error the ADU is empty and the port is closed /
    // state moved to Error.
    using TransactDone = std::function<void(std::vector<std::uint8_t>, std::string)>;

    void transactAsync(std::shared_ptr<std::vector<std::uint8_t>> request,
                       TransactDone done);
    void cacheHoldingRegisters(int startAddress, core::RegisterWords const& values);
    void closePortLocked();
    void failPortAsync(std::string const& reason);
    std::expected<void, std::string> applySerialOptions();

    config::TransportConfig m_cfg;
    gateway_asio::io_context* m_io = nullptr;
    gateway_asio::serial_port m_serial;
    std::unique_ptr<sched::RequestScheduler> m_scheduler;
    std::atomic<transport::ConnectionState> m_state{transport::ConnectionState::Disconnected};
    std::mutex m_portMtx;
    mutable std::mutex m_cacheMtx;
    std::map<int, std::uint16_t> m_hrCache;
};

} // namespace core::gateway
