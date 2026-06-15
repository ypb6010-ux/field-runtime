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
std::vector<std::uint8_t> buildExceptionResponse(std::uint16_t transactionId,
                                                 std::uint8_t unitId,
                                                 std::uint8_t function,
                                                 std::uint8_t exceptionCode);

} // namespace nmbs
