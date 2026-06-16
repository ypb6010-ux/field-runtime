// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <cstdlib>
#include <iostream>
#include <istream>
#include <string>
#include <vector>

#include "GatewayAsio.h"

namespace {

bool sendCommand(gateway_asio::ip::tcp::socket& socket,
                 gateway_asio::streambuf& responseBuffer,
                 std::string const& command,
                 std::string& response) {
    auto line = command + "\n";
    gateway_error_code ec;
    gateway_asio::write(socket, gateway_asio::buffer(line), ec);
    if (ec) {
        response = "write failed: " + ec.message();
        return false;
    }

    gateway_asio::read_until(socket, responseBuffer, '\n', ec);
    if (ec) {
        response = "read failed: " + ec.message();
        return false;
    }

    std::istream is(&responseBuffer);
    std::getline(is, response);
    if (!response.empty() && response.back() == '\r') response.pop_back();
    return true;
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

    gateway_asio::io_context io;
    gateway_asio::ip::tcp::resolver resolver(io);
    gateway_error_code ec;
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) {
        std::cerr << "resolve failed: " << ec.message() << '\n';
        return EXIT_FAILURE;
    }

    gateway_asio::ip::tcp::socket socket(io);
    gateway_asio::connect(socket, endpoints, ec);
    if (ec) {
        std::cerr << "connect failed: " << ec.message() << '\n';
        return EXIT_FAILURE;
    }

    gateway_asio::streambuf responseBuffer;
    for (auto const& command : commands) {
        std::string response;
        if (!sendCommand(socket, responseBuffer, command, response)) {
            std::cout << command << ' ' << response << '\n';
            return EXIT_FAILURE;
        }
        std::cout << command << " " << response << '\n';
    }
    return EXIT_SUCCESS;
}
