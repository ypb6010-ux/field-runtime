// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "GatewayAsio.h"
#include "nanomodbus.h"

namespace {

std::uint16_t getU16(std::span<std::uint8_t const> data, std::size_t pos) {
    return std::uint16_t((std::uint16_t(data[pos]) << 8) | data[pos + 1]);
}

std::vector<std::uint8_t> readAdu(gateway_asio::ip::tcp::socket& socket) {
    gateway_error_code ec;
    std::array<std::uint8_t, 7> header{};
    gateway_asio::read(socket, gateway_asio::buffer(header), ec);
    if (ec) return {};
    auto const length = getU16(header, 4);
    if (length < 2 || length > 260) return {};
    std::vector<std::uint8_t> adu(header.begin(), header.end());
    adu.resize(6 + length);
    gateway_asio::read(socket, gateway_asio::buffer(adu.data() + 7, length - 1), ec);
    if (ec) return {};
    return adu;
}

std::vector<std::uint8_t> handleRequest(std::vector<std::uint16_t>& holding,
                                        std::span<std::uint8_t const> adu) {
    nmbs::ResponseHeader header;
    std::span<std::uint8_t const> pdu;
    std::string error;
    if (!nmbs::parseRequestHeader(adu, header, pdu, error)) return {};
    if (pdu.empty()) return {};

    auto const function = pdu[0];
    if (function == 0x03 || function == 0x04) {
        if (pdu.size() != 5) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }
        auto const start = getU16(pdu, 1);
        auto const count = getU16(pdu, 3);
        if (count == 0 || start + count > holding.size()) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x02);
        }
        auto first = holding.begin() + start;
        auto last = first + count;
        return nmbs::buildReadRegistersResponse(
            header.transactionId,
            header.unitId,
            function == 0x03 ? nmbs::Function::ReadHoldingRegisters
                             : nmbs::Function::ReadInputRegisters,
            std::span<std::uint16_t const>(&*first, std::size_t(last - first)));
    }

    if (function == 0x10) {
        if (pdu.size() < 6) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }
        auto const start = getU16(pdu, 1);
        auto const count = getU16(pdu, 3);
        auto const byteCount = pdu[5];
        if (count == 0 || byteCount != count * 2 || pdu.size() != std::size_t(6 + byteCount)
            || start + count > holding.size()) {
            return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x03);
        }
        for (std::uint16_t i = 0; i < count; i++) {
            holding[start + i] = getU16(pdu, 6 + i * 2);
        }
        return nmbs::buildWriteMultipleRegistersResponse(
            header.transactionId, header.unitId, start, count);
    }

    return nmbs::buildExceptionResponse(header.transactionId, header.unitId, function, 0x01);
}

} // namespace

int main(int argc, char** argv) {
    auto const port = static_cast<unsigned short>(
        argc > 1 ? std::stoi(argv[1]) : 15020);
    // "silent" mode accepts the TCP connection but never answers a request — a
    // deaf PLC. Used to prove the gateway's per-request timeout keeps the io
    // loop alive (control / MQTT keep responding) instead of freezing forever.
    bool const silent = (argc > 2 && std::string(argv[2]) == "silent");
    gateway_asio::io_context io;
    gateway_asio::ip::tcp::acceptor acceptor(
        io,
        gateway_asio::ip::tcp::endpoint(gateway_asio::ip::address_v4::loopback(), port));

    std::vector<std::uint16_t> holding(128, 0);
    holding[0] = 230;      // temperature with scale=0.1 -> 23.0
    holding[1] = 1450;     // speed
    holding[2] = 0x5678;   // CDAB word order for 0x12345678
    holding[3] = 0x1234;
    holding[4] = 2;        // run_state -> enum_u16 "fault"

    std::cout << "mock Modbus TCP server listening on 127.0.0.1:" << port
              << (silent ? " (silent)" : "") << std::endl;
    for (;;) {
        gateway_asio::ip::tcp::socket socket(io);
        gateway_error_code ec;
        acceptor.accept(socket, ec);
        if (ec) return EXIT_FAILURE;
        for (;;) {
            auto request = readAdu(socket);
            if (request.empty()) break;
            if (silent) continue;   // read and drop; never reply
            auto response = handleRequest(holding, request);
            if (response.empty()) break;
            gateway_asio::write(socket, gateway_asio::buffer(response), ec);
            if (ec) break;
        }
    }
}
