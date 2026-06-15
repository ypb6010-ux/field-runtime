// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "GatewayAsio.h"

#include "core/bus/EventBus.h"
#include "core/codec/CodecRegistry.h"
#include "core/config/ConfigSchema.h"
#include "core/dp/DatapointRegistry.h"
#include "core/log/Logger.h"
#include "core/module/PollRange.h"
#include "core/transport/Transport.h"

namespace core::gateway {

class GatewayAssembly {
public:
    explicit GatewayAssembly(gateway_asio::io_context& io);
    ~GatewayAssembly();

    bool load(std::string const& tomlPath);
    void start();
    void stop();
    void printSnapshot(std::size_t limit = 8) const;

    log::Logger& logger();

private:
    struct PollTimer {
        module::PollRange* poll = nullptr;
        std::unique_ptr<gateway_asio::steady_timer> timer;
    };

    using DpById = std::map<std::string, std::shared_ptr<dp::Datapoint>>;

    void wireFromSchema(config::ConfigSchema const& schema);
    void buildTransports(config::ConfigSchema const& schema);
    DpById buildDatapoints(config::ConfigSchema const& schema);
    void buildPollRanges(config::ConfigSchema const& schema, DpById const& byId);
    void wireBindings(module::PollRange& poll,
                      config::ConfigSchema const& schema,
                      DpById const& byId,
                      std::string const& transportId,
                      transport::ReadRequest const& req);
    void schedulePoll(PollTimer& pollTimer);

    gateway_asio::io_context* m_io = nullptr;
    log::Logger m_logger;
    bus::EventBus m_bus;
    codec::CodecRegistry m_codecs;
    dp::DatapointRegistry m_datapoints;
    std::map<std::string, std::unique_ptr<transport::Transport>> m_transports;
    std::vector<std::unique_ptr<module::PollRange>> m_pollRanges;
    std::vector<PollTimer> m_pollTimers;
    bool m_started = false;
};

} // namespace core::gateway
