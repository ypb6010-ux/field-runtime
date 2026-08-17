// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "GatewayAsio.h"

#include "core/base/RegisterTable.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/config/ConfigSchema.h"
#include "core/sched/RequestScheduler.h"
#include "core/transport/Transport.h"

namespace core::gateway {

class AsioModbusTcpServer final : public transport::Transport {
public:
    using WriteAuthorizer =
        std::function<bool(bus::ServerWriteEvent const&, std::string&)>;

    AsioModbusTcpServer(config::TransportConfig cfg,
                        gateway_asio::io_context& io,
                        bus::EventBus& bus);
    ~AsioModbusTcpServer() override;

    std::string id() const override;
    transport::TransportKind kind() const override;
    transport::ConnectionState state() const override;

    std::expected<void, std::string> connect() override;
    void disconnect() override;

    sched::RequestScheduler& scheduler() override;

    transport::ReadResult read(transport::ReadRequest const& req) override;
    transport::WriteResult writeBatch(transport::WriteBatch const& batch) override;
    void setWriteAuthorizer(WriteAuthorizer authorizer);

private:
    using TcpSocket = gateway_asio::ip::tcp::socket;

    void startAccept();
    void startReadHeader(std::shared_ptr<TcpSocket> socket);
    void startReadBody(std::shared_ptr<TcpSocket> socket,
                       std::shared_ptr<std::array<std::uint8_t, 7>> header,
                       std::uint16_t length);
    std::vector<std::uint8_t> handleRequest(
        std::shared_ptr<TcpSocket> const& socket,
        std::vector<std::uint8_t> const& adu);
    bus::ServerWriteEvent makeWriteEvent(
        std::shared_ptr<TcpSocket> const& socket,
        int unitId,
        int start,
        core::RegisterWords values);
    bool authorizeAndPublish(bus::ServerWriteEvent const& event,
                             std::string& error);

    core::RegisterWords readLocal(core::RegisterTable table, int start, int count);
    bool writeLocal(core::RegisterTable table,
                    int start,
                    core::RegisterWords const& values,
                    bool publishEvent);
    bool rangeAllowed(core::RegisterTable table, int start, int count) const;
    void removeClient(std::shared_ptr<TcpSocket> const& socket);
    void ensureRangeLocked(core::RegisterTable table, int start, int count);
    void closeAllLocked();

    config::TransportConfig m_cfg;
    gateway_asio::io_context* m_io = nullptr;
    bus::EventBus* m_bus = nullptr;
    std::unique_ptr<gateway_asio::ip::tcp::acceptor> m_acceptor;
    std::set<std::shared_ptr<TcpSocket>,
             std::owner_less<std::shared_ptr<TcpSocket>>> m_clients;
    std::map<std::shared_ptr<TcpSocket>, std::string,
             std::owner_less<std::shared_ptr<TcpSocket>>> m_sessionIds;
    std::uint64_t m_nextSessionId = 1;
    WriteAuthorizer m_writeAuthorizer;
    std::unique_ptr<sched::RequestScheduler> m_scheduler;
    std::atomic<transport::ConnectionState> m_state{transport::ConnectionState::Disconnected};
    std::mutex m_mtx;
    std::map<core::RegisterTable, core::RegisterWords> m_tables;
};

} // namespace core::gateway
