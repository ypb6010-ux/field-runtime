// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// Deterministic, hardware-free self-test for the Modbus RTU framing layer
// (nmbs::crc16 / buildRtu* / parseRtu*). It walks the full wire contract the
// AsioModbusRtuClient <-> RtuMockSlave pair relies on: client request ->
// slave parse -> slave response -> client parse, for both reads and writes,
// plus a CRC known-answer and the exception path. Exit code 0 = all pass.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "nanomodbus.h"

namespace {

int g_failures = 0;

void check(bool ok, std::string const& what) {
    if (ok) {
        std::cout << "  ok   " << what << "\n";
    } else {
        std::cout << "  FAIL " << what << "\n";
        ++g_failures;
    }
}

std::uint16_t getU16(std::span<std::uint8_t const> d, std::size_t pos) {
    return std::uint16_t((std::uint16_t(d[pos]) << 8) | d[pos + 1]);
}

} // namespace

int main() {
    // 1. CRC16 known-answer against the documented Modbus frame
    //    "01 04 02 FF FF B8 80" (CRC sent low byte first: lo=0xB8, hi=0x80,
    //    i.e. crc16 value 0x80B8).
    {
        std::vector<std::uint8_t> body{0x01, 0x04, 0x02, 0xFF, 0xFF};
        auto const crc = nmbs::crc16(body);
        check(crc == 0x80B8, "crc16 known-answer 0x80B8");

        auto resp = nmbs::buildRtuReadRegistersResponse(
            1, nmbs::Function::ReadInputRegisters, std::vector<std::uint16_t>{0xFFFF});
        check(resp.size() == 7 && resp[5] == 0xB8 && resp[6] == 0x80,
              "FC04 frame trailer matches documented '... B8 80'");
    }

    // 2. Read round-trip: client builds FC03 request -> slave parses -> slave
    //    builds response -> client parses back the same registers.
    {
        auto req = nmbs::buildRtuReadRequest(1, nmbs::Function::ReadHoldingRegisters, 0, 3);
        std::uint8_t unit = 0;
        std::span<std::uint8_t const> pdu;
        std::string err;
        bool parsed = nmbs::parseRtuRequest(req, unit, pdu, err);
        check(parsed && unit == 1 && pdu.size() == 5 && pdu[0] == 0x03,
              "FC03 request parses (unit/pdu)");
        check(parsed && getU16(pdu, 1) == 0 && getU16(pdu, 3) == 3,
              "FC03 request start=0 count=3");

        std::vector<std::uint16_t> regs{230, 1450, 2};
        auto resp = nmbs::buildRtuReadRegistersResponse(
            1, nmbs::Function::ReadHoldingRegisters, regs);
        std::vector<std::uint16_t> out;
        bool ok = nmbs::parseRtuReadResponse(
            resp, 1, nmbs::Function::ReadHoldingRegisters, out, err);
        check(ok && out == regs, "FC03 response round-trips 230/1450/2");
    }

    // 3. Write round-trip: client builds FC10 request -> slave parses values ->
    //    slave echoes start/count -> client validates.
    {
        std::vector<std::uint16_t> values{4242, 7};
        auto req = nmbs::buildRtuWriteMultipleRegistersRequest(1, 10, values);
        std::uint8_t unit = 0;
        std::span<std::uint8_t const> pdu;
        std::string err;
        bool parsed = nmbs::parseRtuRequest(req, unit, pdu, err);
        check(parsed && pdu[0] == 0x10 && getU16(pdu, 1) == 10 && getU16(pdu, 3) == 2,
              "FC10 request parses start=10 count=2");
        check(parsed && pdu[5] == 4 && getU16(pdu, 6) == 4242 && getU16(pdu, 8) == 7,
              "FC10 request carries values 4242/7");

        auto resp = nmbs::buildRtuWriteMultipleRegistersResponse(1, 10, 2);
        bool ok = nmbs::parseRtuWriteMultipleRegistersResponse(resp, 1, 10, 2, err);
        check(ok, "FC10 response validates start/count echo");
    }

    // 4. Exception path: slave reports illegal data address -> client surfaces it.
    {
        auto resp = nmbs::buildRtuExceptionResponse(1, 0x03, 0x02);
        std::vector<std::uint16_t> out;
        std::string err;
        bool ok = nmbs::parseRtuReadResponse(
            resp, 1, nmbs::Function::ReadHoldingRegisters, out, err);
        check(!ok && err.find("exception 2") != std::string::npos,
              "FC03 exception 0x02 is surfaced");
    }

    // 5. CRC tamper is rejected.
    {
        auto resp = nmbs::buildRtuReadRegistersResponse(
            1, nmbs::Function::ReadHoldingRegisters, std::vector<std::uint16_t>{99});
        resp.back() ^= 0xFF;  // corrupt CRC hi byte
        std::vector<std::uint16_t> out;
        std::string err;
        bool ok = nmbs::parseRtuReadResponse(
            resp, 1, nmbs::Function::ReadHoldingRegisters, out, err);
        check(!ok && err.find("CRC") != std::string::npos, "corrupt CRC is rejected");
    }

    if (g_failures == 0) {
        std::cout << "RTU framing self-test: ALL PASS\n";
        return EXIT_SUCCESS;
    }
    std::cout << "RTU framing self-test: " << g_failures << " FAILURE(S)\n";
    return EXIT_FAILURE;
}
