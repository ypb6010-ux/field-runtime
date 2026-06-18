// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nmbs {

enum class Function : std::uint8_t {
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteMultipleRegisters = 0x10,
};

struct ReadRequest {
    std::uint16_t transactionId = 0;
    std::uint8_t unitId = 1;
    Function function = Function::ReadHoldingRegisters;
    std::uint16_t startAddress = 0;
    std::uint16_t count = 0;
};

struct WriteMultipleRegistersRequest {
    std::uint16_t transactionId = 0;
    std::uint8_t unitId = 1;
    std::uint16_t startAddress = 0;
    std::vector<std::uint16_t> values;
};

struct ResponseHeader {
    std::uint16_t transactionId = 0;
    std::uint8_t unitId = 1;
    std::uint8_t function = 0;
};

std::vector<std::uint8_t> buildReadRequest(ReadRequest const& req);
std::vector<std::uint8_t> buildWriteMultipleRegistersRequest(
    WriteMultipleRegistersRequest const& req);
std::vector<std::uint8_t> buildWriteSingleRegisterRequest(std::uint16_t transactionId,
                                                          std::uint8_t unitId,
                                                          std::uint16_t address,
                                                          std::uint16_t value);

bool parseReadResponse(std::span<std::uint8_t const> adu,
                       std::uint16_t expectedTransactionId,
                       Function expectedFunction,
                       std::vector<std::uint16_t>& out,
                       std::string& error);

bool parseWriteMultipleRegistersResponse(std::span<std::uint8_t const> adu,
                                         std::uint16_t expectedTransactionId,
                                         std::uint16_t expectedStart,
                                         std::uint16_t expectedCount,
                                         std::string& error);

bool parseRequestHeader(std::span<std::uint8_t const> adu,
                        ResponseHeader& out,
                        std::span<std::uint8_t const>& pdu,
                        std::string& error);

std::vector<std::uint8_t> buildReadRegistersResponse(std::uint16_t transactionId,
                                                     std::uint8_t unitId,
                                                     Function function,
                                                     std::span<std::uint16_t const> values);
std::vector<std::uint8_t> buildWriteMultipleRegistersResponse(
    std::uint16_t transactionId,
    std::uint8_t unitId,
    std::uint16_t startAddress,
    std::uint16_t count);
std::vector<std::uint8_t> buildWriteSingleRegisterResponse(std::uint16_t transactionId,
                                                           std::uint8_t unitId,
                                                           std::uint16_t address,
                                                           std::uint16_t value);
std::vector<std::uint8_t> buildExceptionResponse(std::uint16_t transactionId,
                                                 std::uint8_t unitId,
                                                 std::uint8_t function,
                                                 std::uint8_t exceptionCode);

// ─── Modbus RTU framing ────────────────────────────────────────────────────
// RTU ADU = [unitId][PDU...][CRC16-lo][CRC16-hi]. No transaction id, no length
// prefix — framing is recovered from the function code plus (for reads) the
// byte-count field. CRC16 uses the Modbus polynomial 0xA001 (init 0xFFFF) and
// is appended low byte first.

std::uint16_t crc16(std::span<std::uint8_t const> data);

std::vector<std::uint8_t> buildRtuReadRequest(std::uint8_t unitId,
                                              Function function,
                                              std::uint16_t startAddress,
                                              std::uint16_t count);
std::vector<std::uint8_t> buildRtuWriteMultipleRegistersRequest(
    std::uint8_t unitId,
    std::uint16_t startAddress,
    std::span<std::uint16_t const> values);

// Server-side helpers (for the mock RTU slave).
std::vector<std::uint8_t> buildRtuReadRegistersResponse(std::uint8_t unitId,
                                                        Function function,
                                                        std::span<std::uint16_t const> values);
std::vector<std::uint8_t> buildRtuWriteMultipleRegistersResponse(std::uint8_t unitId,
                                                                 std::uint16_t startAddress,
                                                                 std::uint16_t count);
std::vector<std::uint8_t> buildRtuExceptionResponse(std::uint8_t unitId,
                                                    std::uint8_t function,
                                                    std::uint8_t exceptionCode);

// Validate CRC + unit/function then extract registers from a complete read
// response ADU.
bool parseRtuReadResponse(std::span<std::uint8_t const> adu,
                          std::uint8_t expectedUnit,
                          Function expectedFunction,
                          std::vector<std::uint16_t>& out,
                          std::string& error);
bool parseRtuWriteMultipleRegistersResponse(std::span<std::uint8_t const> adu,
                                            std::uint8_t expectedUnit,
                                            std::uint16_t expectedStart,
                                            std::uint16_t expectedCount,
                                            std::string& error);

// Parse a complete request ADU received by the slave. On success `unitId` /
// `pdu` are filled (pdu excludes unit id and CRC). Verifies CRC.
bool parseRtuRequest(std::span<std::uint8_t const> adu,
                     std::uint8_t& unitId,
                     std::span<std::uint8_t const>& pdu,
                     std::string& error);

} // namespace nmbs
