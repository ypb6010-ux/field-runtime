// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "GatewayAsio.h"

namespace {

std::string request(std::string const& host,
                    int port,
                    std::string const& command) {
    gateway_asio::io_context io;
    gateway_asio::ip::tcp::resolver resolver(io);
    gateway_error_code ec;
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) return "resolve failed: " + ec.message();

    gateway_asio::ip::tcp::socket socket(io);
    gateway_asio::connect(socket, endpoints, ec);
    if (ec) return "connect failed: " + ec.message();

    auto line = command + "\n";
    gateway_asio::write(socket, gateway_asio::buffer(line), ec);
    if (ec) return "write failed: " + ec.message();

    std::string response;
    std::array<char, 1024> buf{};
    for (;;) {
        auto n = socket.read_some(gateway_asio::buffer(buf), ec);
        if (n > 0) response.append(buf.data(), n);
        if (ec) break;
    }
    while (!response.empty()
           && (response.back() == '\n' || response.back() == '\r')) {
        response.pop_back();
    }
    return response;
}

} // namespace

int main(int argc, char** argv) {
    std::string const host = argc > 1 ? argv[1] : "127.0.0.1";
    auto const port = argc > 2 ? std::stoi(argv[2]) : 15022;
    std::vector<std::string> commands;
    if (argc > 3) {
        for (int i = 3; i < argc; i++) commands.emplace_back(argv[i]);
    } else {
        commands = {"status", "live", "help"};
    }

    for (auto const& command : commands) {
        auto response = request(host, port, command);
        std::cout << command << " " << response << '\n';
    }
    return EXIT_SUCCESS;
}
