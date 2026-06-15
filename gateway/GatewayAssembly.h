// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "GatewayAsio.h"

#include "core/bus/EventBus.h"
#include "core/bus/BusEvents.h"
#include "core/bus/Subscription.h"
#include "core/codec/CodecRegistry.h"
#include "core/config/ConfigSchema.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/State.h"
#include "core/log/Logger.h"
#include "core/module/PollRange.h"
#include "core/module/SinkWindow.h"
#include "core/transport/Transport.h"

namespace core::gateway {

struct ControlConfig {
    std::string listenAddress = "127.0.0.1";
    int listenPort = 0;
};

struct GatewayTransportSnapshot {
    std::string id;
    transport::TransportKind kind = transport::TransportKind::ModbusTcpClient;
    transport::ConnectionState state = transport::ConnectionState::Disconnected;
    sched::SchedulerStats stats;
};

struct GatewayDatapointSnapshot {
    std::string id;
    dp::Value value;
    dp::DpState state = dp::DpState::Missing;
    dp::Timestamp timestamp{};
};

class GatewayAssembly {
public:
    explicit GatewayAssembly(gateway_asio::io_context& io);
    ~GatewayAssembly();

    bool load(std::string const& tomlPath);
    void start();
    void stop();
    void printSnapshot(std::size_t limit = 8) const;
    void setServerForwardEnabled(std::string const& serverTransportId, bool enabled);
    std::optional<ControlConfig> controlConfig() const;
    std::vector<GatewayTransportSnapshot> transportSnapshots();
    std::vector<GatewayDatapointSnapshot> datapointSnapshots() const;

    log::Logger& logger();

private:
    struct PollTimer {
        module::PollRange* poll = nullptr;
        std::unique_ptr<gateway_asio::steady_timer> timer;
    };
    struct SinkTimer {
        module::SinkWindow* sink = nullptr;
        std::unique_ptr<gateway_asio::steady_timer> timer;
    };

    using DpById = std::map<std::string, std::shared_ptr<dp::Datapoint>>;

    void wireFromSchema(config::ConfigSchema const& schema);
    void buildTransports(config::ConfigSchema const& schema);
    DpById buildDatapoints(config::ConfigSchema const& schema);
    void buildPollRanges(config::ConfigSchema const& schema, DpById const& byId);
    void buildSinkWindows(config::ConfigSchema const& schema);
    void buildBridges(config::ConfigSchema const& schema);
    void installEventWiring();
    void forwardBridges(bus::ServerWriteEvent const& e);
    void zeroBridgeForward(std::string const& serverTransportId);
    bool serverForwardEnabled(std::string const& serverTransportId) const;
    void mirrorBridgesOnce();
    void startMirrorPump();
    void wireBindings(module::PollRange& poll,
                      config::ConfigSchema const& schema,
                      DpById const& byId,
                      std::string const& transportId,
                      transport::ReadRequest const& req);
    void schedulePoll(PollTimer& pollTimer);
    void scheduleSink(SinkTimer& sinkTimer);

    gateway_asio::io_context* m_io = nullptr;
    log::Logger m_logger;
    bus::EventBus m_bus;
    codec::CodecRegistry m_codecs;
    dp::DatapointRegistry m_datapoints;
    std::map<std::string, std::unique_ptr<transport::Transport>> m_transports;
    std::vector<std::unique_ptr<module::PollRange>> m_pollRanges;
    std::vector<std::unique_ptr<module::SinkWindow>> m_sinkWindows;
    std::vector<PollTimer> m_pollTimers;
    std::vector<SinkTimer> m_sinkTimers;
    std::vector<config::BridgeConfig> m_bridges;
    std::vector<std::vector<std::pair<int, std::shared_ptr<dp::Datapoint>>>> m_bridgeMirrors;
    std::vector<module::SinkWindow*> m_bridgeFwdSinks;
    std::unique_ptr<bus::Subscription> m_serverWriteSub;
    std::unique_ptr<gateway_asio::steady_timer> m_mirrorTimer;
    mutable std::mutex m_forwardMtx;
    std::unordered_map<std::string, bool> m_forwardEnabled;
    std::optional<ControlConfig> m_control;
    bool m_started = false;
};

} // namespace core::gateway
