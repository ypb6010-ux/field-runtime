// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "ControlSocket.h"
#include "GatewayAsio.h"
#include "GatewayAssembly.h"

namespace {

void scheduleSnapshot(gateway_asio::io_context& io,
                      core::gateway::GatewayAssembly& gateway,
                      std::shared_ptr<gateway_asio::steady_timer> timer) {
    timer->expires_after(std::chrono::seconds(1));
    timer->async_wait([&io, &gateway, timer](auto const& ec) {
        if (ec) return;
        gateway.printSnapshot();
        scheduleSnapshot(io, gateway, timer);
    });
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: field_gateway <gateway.toml>\n";
        return EXIT_FAILURE;
    }

    gateway_asio::io_context io;
    core::gateway::GatewayAssembly gateway(io);
    if (!gateway.load(argv[1])) {
        return EXIT_FAILURE;
    }

    std::unique_ptr<core::gateway::ControlSocket> control;
    if (auto cfg = gateway.controlConfig()) {
        control = std::make_unique<core::gateway::ControlSocket>(io, gateway, *cfg);
        try {
            control->start();
            gateway.logger().logf(core::log::LogLevel::Info,
                                  "gateway",
                                  "control",
                                  "listening "
                                      + cfg->listenAddress + ":"
                                      + std::to_string(cfg->listenPort));
        } catch (std::runtime_error const& e) {
            std::cerr << e.what() << '\n';
            return EXIT_FAILURE;
        }
    }

    gateway_asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](auto const&, int) {
        if (control) control->stop();
        gateway.stop();
        io.stop();
    });

    auto snapshotTimer = std::make_shared<gateway_asio::steady_timer>(io);
    scheduleSnapshot(io, gateway, snapshotTimer);

    auto gateTimer = std::make_shared<gateway_asio::steady_timer>(io);
    if (auto const* serverId = std::getenv("FIELD_GATEWAY_FORWARD_SERVER")) {
        if (auto const* delay = std::getenv("FIELD_GATEWAY_DISABLE_FORWARD_AFTER_MS")) {
            gateTimer->expires_after(std::chrono::milliseconds(std::stoi(delay)));
            gateTimer->async_wait([&gateway, server = std::string(serverId)](auto const& ec) {
                if (ec) return;
                gateway.setServerForwardEnabled(server, false);
                gateway.logger().logf(core::log::LogLevel::Warn,
                                      "gateway",
                                      server,
                                      "forward disabled");
            });
        }
    }

    gateway.start();
    io.run();
    if (control) control->stop();
    gateway.stop();
    return EXIT_SUCCESS;
}
