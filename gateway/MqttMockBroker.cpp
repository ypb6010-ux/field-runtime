// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "GatewayAsio.h"

namespace {

bool readExact(gateway_asio::ip::tcp::socket& socket,
               void* data,
               std::size_t size) {
    gateway_error_code ec;
    gateway_asio::read(socket, gateway_asio::buffer(data, size), ec);
    return !ec;
}

bool readRemainingLength(gateway_asio::ip::tcp::socket& socket,
                         std::uint32_t& out) {
    out = 0;
    int multiplier = 1;
    for (int i = 0; i < 4; i++) {
        std::uint8_t encoded = 0;
        if (!readExact(socket, &encoded, 1)) return false;
        out += std::uint32_t(encoded & 127) * std::uint32_t(multiplier);
        if ((encoded & 128) == 0) return true;
        multiplier *= 128;
    }
    return false;
}

std::uint16_t getU16(std::vector<std::uint8_t> const& data, std::size_t pos) {
    return std::uint16_t((std::uint16_t(data[pos]) << 8) | data[pos + 1]);
}

bool writePacket(gateway_asio::ip::tcp::socket& socket,
                 std::initializer_list<std::uint8_t> bytes) {
    gateway_error_code ec;
    gateway_asio::write(socket, gateway_asio::buffer(bytes.begin(), bytes.size()), ec);
    return !ec;
}

void handlePublish(std::uint8_t packetType,
                   std::vector<std::uint8_t> const& body,
                   gateway_asio::ip::tcp::socket& socket) {
    if (body.size() < 2) return;
    auto const topicLen = getU16(body, 0);
    if (body.size() < std::size_t(2 + topicLen)) return;
    std::string topic(body.begin() + 2, body.begin() + 2 + topicLen);

    std::size_t payloadOffset = 2 + topicLen;
    auto const qos = (packetType >> 1) & 0x03;
    std::uint16_t packetId = 0;
    if (qos > 0) {
        if (body.size() < payloadOffset + 2) return;
        packetId = getU16(body, payloadOffset);
        payloadOffset += 2;
    }

    std::string payload(body.begin() + payloadOffset, body.end());
    std::cout << topic << ' ' << payload << std::endl;
    if (qos == 1) {
        writePacket(socket, {0x40, 0x02,
                             std::uint8_t(packetId >> 8),
                             std::uint8_t(packetId & 0xFF)});
    }
}

} // namespace

int main(int argc, char** argv) {
    auto const port = static_cast<unsigned short>(
        argc > 1 ? std::stoi(argv[1]) : 1883);
    gateway_asio::io_context io;
    gateway_asio::ip::tcp::acceptor acceptor(
        io,
        gateway_asio::ip::tcp::endpoint(gateway_asio::ip::address_v4::loopback(), port));

    std::cout << "mock MQTT broker listening on 127.0.0.1:" << port << std::endl;
    for (;;) {
        gateway_asio::ip::tcp::socket socket(io);
        gateway_error_code ec;
        acceptor.accept(socket, ec);
        if (ec) return EXIT_FAILURE;

        for (;;) {
            std::uint8_t packetType = 0;
            if (!readExact(socket, &packetType, 1)) break;
            std::uint32_t remainingLength = 0;
            if (!readRemainingLength(socket, remainingLength)) break;
            std::vector<std::uint8_t> body(remainingLength);
            if (remainingLength > 0 && !readExact(socket, body.data(), body.size())) break;

            auto const type = packetType >> 4;
            if (type == 1) {
                if (!writePacket(socket, {0x20, 0x02, 0x00, 0x00})) break;
            } else if (type == 3) {
                handlePublish(packetType, body, socket);
            } else if (type == 12) {
                if (!writePacket(socket, {0xD0, 0x00})) break;
            } else if (type == 14) {
                break;
            }
        }
    }
}
