// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>

#include "GatewayAsio.h"
#include "GatewayAssembly.h"

namespace core::gateway {

class ControlSocket {
public:
    ControlSocket(gateway_asio::io_context& io,
                  GatewayAssembly& gateway,
                  ControlConfig config);
    ~ControlSocket();

    void start();
    void stop();

private:
    using TcpSocket = gateway_asio::ip::tcp::socket;

    void startAccept();
    void handleClient(std::shared_ptr<TcpSocket> socket);
    std::string handleCommand(std::string const& command);
    std::string statusJson();
    std::string liveJson() const;

    gateway_asio::io_context* m_io = nullptr;
    GatewayAssembly* m_gateway = nullptr;
    ControlConfig m_config;
    std::unique_ptr<gateway_asio::ip::tcp::acceptor> m_acceptor;
    bool m_started = false;
};

} // namespace core::gateway
