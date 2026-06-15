// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "GatewayAsio.h"
#include "nanomodbus.h"

namespace {

std::uint16_t nextTx() {
    static std::uint16_t tx = 1;
    return tx++;
}

std::vector<std::uint8_t> transact(gateway_asio::ip::tcp::socket& socket,
                                   std::vector<std::uint8_t> const& request) {
    gateway_error_code ec;
    gateway_asio::write(socket, gateway_asio::buffer(request), ec);
    if (ec) return {};

    std::array<std::uint8_t, 7> header{};
    gateway_asio::read(socket, gateway_asio::buffer(header), ec);
    if (ec) return {};
    auto const length = std::uint16_t((std::uint16_t(header[4]) << 8) | header[5]);
    if (length < 2 || length > 260) return {};
    std::vector<std::uint8_t> response(header.begin(), header.end());
    response.resize(6 + length);
    gateway_asio::read(socket, gateway_asio::buffer(response.data() + 7, length - 1), ec);
    if (ec) return {};
    return response;
}

} // namespace

int main(int argc, char** argv) {
    std::string const host = argc > 1 ? argv[1] : "127.0.0.1";
    auto const port = argc > 2 ? std::stoi(argv[2]) : 15021;
    auto const writeValue = std::uint16_t(argc > 3 ? std::stoi(argv[3]) : 4321);

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

    std::string error;
    auto const readMirrorTx = nextTx();
    auto mirrorResponse = transact(socket, nmbs::buildReadRequest({
        readMirrorTx, 1, nmbs::Function::ReadHoldingRegisters, 0, 4
    }));
    std::vector<std::uint16_t> mirror;
    if (!nmbs::parseReadResponse(
            mirrorResponse,
            readMirrorTx,
            nmbs::Function::ReadHoldingRegisters,
            mirror,
            error)) {
        std::cerr << "read mirror failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "mirror";
    for (auto value : mirror) std::cout << ' ' << value;
    std::cout << '\n';

    auto const writeTx = nextTx();
    auto writeResponse = transact(socket, nmbs::buildWriteMultipleRegistersRequest({
        writeTx, 1, 10, std::vector<std::uint16_t>{writeValue}
    }));
    if (!nmbs::parseWriteMultipleRegistersResponse(writeResponse, writeTx, 10, 1, error)) {
        std::cerr << "write failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "write hr10=" << writeValue << '\n';

    auto const readWriteTx = nextTx();
    auto writeReadResponse = transact(socket, nmbs::buildReadRequest({
        readWriteTx, 1, nmbs::Function::ReadHoldingRegisters, 10, 1
    }));
    std::vector<std::uint16_t> written;
    if (!nmbs::parseReadResponse(
            writeReadResponse,
            readWriteTx,
            nmbs::Function::ReadHoldingRegisters,
            written,
            error)) {
        std::cerr << "read write-area failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "server.hr10=" << (written.empty() ? 0 : written.front()) << '\n';
    return EXIT_SUCCESS;
}
