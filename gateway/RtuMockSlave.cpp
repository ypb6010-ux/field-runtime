// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// Minimal in-process Modbus RTU slave for the gateway RTU self-test. Opens a
// serial port (one end of a virtual pair, e.g. created by `socat -d -d
// pty,raw,echo=0 pty,raw,echo=0`) and answers FC03/04 reads and FC10 writes so
// the gateway's AsioModbusRtuClient can poll it.
//
// Usage: field_gateway_rtu_mock_slave <serial-port> [unit-id] [baud]
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

std::uint16_t getU16(std::span<std::uint8_t const> d, std::size_t pos) {
    return std::uint16_t((std::uint16_t(d[pos]) << 8) | d[pos + 1]);
}

// Read exactly n bytes, appending to buf. Returns false on error/EOF.
bool readExactly(gateway_asio::serial_port& port, std::vector<std::uint8_t>& buf,
                 std::size_t n) {
    auto const from = buf.size();
    buf.resize(from + n);
    gateway_error_code ec;
    gateway_asio::read(port, gateway_asio::buffer(buf.data() + from, n), ec);
    return !ec;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <serial-port> [unit-id] [baud]\n";
        return EXIT_FAILURE;
    }
    std::string const portName = argv[1];
    auto const unitId = std::uint8_t(argc > 2 ? std::stoi(argv[2]) : 1);
    auto const baud = unsigned(argc > 3 ? std::stoi(argv[3]) : 9600);

    // Holding registers: 0 -> 230 (temp 23.0), 1 -> 1450 (speed), 2 -> 2 (fault).
    std::array<std::uint16_t, 256> regs{};
    regs[0] = 230;
    regs[1] = 1450;
    regs[2] = 2;

    gateway_asio::io_context io;
    gateway_asio::serial_port port(io);
    gateway_error_code ec;
    port.open(portName, ec);
    if (ec) {
        std::cerr << "open serial '" << portName << "' failed: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }
    using sp = gateway_asio::serial_port_base;
    port.set_option(sp::baud_rate(baud));
    port.set_option(sp::character_size(8));
    port.set_option(sp::stop_bits(sp::stop_bits::one));
    port.set_option(sp::parity(sp::parity::none));
    port.set_option(sp::flow_control(sp::flow_control::none));

    std::cout << "mock RTU slave on " << portName << " unit " << int(unitId)
              << " @ " << baud << std::endl;

    for (;;) {
        std::vector<std::uint8_t> adu;
        if (!readExactly(port, adu, 2)) break;     // [unit][func]
        auto const base = std::uint8_t(adu[1] & 0x7Fu);

        std::size_t tail = 0;
        if (base == 0x03 || base == 0x04 || base == 0x06) {
            tail = 6;                              // addr2 + (count|value)2 + crc2
        } else if (base == 0x10) {
            if (!readExactly(port, adu, 5)) break; // start2 + count2 + byteCount1
            tail = std::size_t(adu[6]) + 2;        // values + crc2
        } else {
            // Unknown function: best-effort drain of 2 CRC bytes and ignore.
            readExactly(port, adu, 2);
            continue;
        }
        if (!readExactly(port, adu, tail)) break;

        std::uint8_t reqUnit = 0;
        std::span<std::uint8_t const> pdu;
        std::string error;
        if (!nmbs::parseRtuRequest(adu, reqUnit, pdu, error)) {
            std::cerr << "bad request: " << error << "\n";
            continue;
        }
        if (reqUnit != unitId) continue;           // not addressed to us

        auto const function = pdu[0];
        std::vector<std::uint8_t> response;
        if (function == 0x03 || function == 0x04) {
            auto const start = getU16(pdu, 1);
            auto const count = getU16(pdu, 3);
            if (count == 0 || count > 125 || std::size_t(start) + count > regs.size()) {
                response = nmbs::buildRtuExceptionResponse(unitId, function, 0x02);
            } else {
                std::vector<std::uint16_t> words(regs.begin() + start,
                                                 regs.begin() + start + count);
                response = nmbs::buildRtuReadRegistersResponse(
                    unitId, static_cast<nmbs::Function>(function), words);
            }
        } else if (function == 0x10) {
            auto const start = getU16(pdu, 1);
            auto const count = getU16(pdu, 3);
            if (count == 0 || std::size_t(start) + count > regs.size()
                || pdu.size() < std::size_t(6 + count * 2)) {
                response = nmbs::buildRtuExceptionResponse(unitId, function, 0x02);
            } else {
                for (std::uint16_t i = 0; i < count; i++) {
                    regs[start + i] = getU16(pdu, 6 + std::size_t(i) * 2);
                }
                response = nmbs::buildRtuWriteMultipleRegistersResponse(unitId, start, count);
            }
        } else {
            response = nmbs::buildRtuExceptionResponse(unitId, function, 0x01);
        }

        gateway_asio::write(port, gateway_asio::buffer(response), ec);
        if (ec) break;
    }

    return EXIT_SUCCESS;
}
