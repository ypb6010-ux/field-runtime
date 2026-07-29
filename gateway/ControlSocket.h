// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
    void readCommand(std::shared_ptr<TcpSocket> socket,
                     std::shared_ptr<gateway_asio::streambuf> buffer,
                     std::shared_ptr<bool> authenticated);
    void writeResponse(std::shared_ptr<TcpSocket> socket,
                       std::shared_ptr<gateway_asio::streambuf> buffer,
                       std::shared_ptr<bool> authenticated,
                       std::string response);
    void handleCommand(std::string const& command,
                       bool& authenticated,
                       std::function<void(std::string)> done);
    std::string handleAuth(std::vector<std::string> const& parts, bool& authenticated) const;
    std::string handleForward(std::vector<std::string> const& parts, bool authenticated);
    std::string handleReconnect(std::vector<std::string> const& parts, bool authenticated);
    void handleWrite(std::vector<std::string> const& parts,
                     bool authenticated,
                     std::function<void(std::string)> done);
    std::string statusJson();
    std::string liveJson() const;

    gateway_asio::io_context* m_io = nullptr;
    GatewayAssembly* m_gateway = nullptr;
    ControlConfig m_config;
    std::unique_ptr<gateway_asio::ip::tcp::acceptor> m_acceptor;
    std::set<std::shared_ptr<TcpSocket>,
             std::owner_less<std::shared_ptr<TcpSocket>>> m_clients;
    bool m_started = false;
};

} // namespace core::gateway
