// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

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

    gateway_asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](auto const&, int) {
        gateway.stop();
        io.stop();
    });

    auto snapshotTimer = std::make_shared<gateway_asio::steady_timer>(io);
    scheduleSnapshot(io, gateway, snapshotTimer);

    gateway.start();
    io.run();
    gateway.stop();
    return EXIT_SUCCESS;
}
