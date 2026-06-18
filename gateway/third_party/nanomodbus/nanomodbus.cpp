// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "nanomodbus.h"

#include <algorithm>

namespace nmbs {

namespace {

constexpr std::uint16_t kProtocolId = 0;
constexpr std::size_t kMbapSize = 7;

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(std::uint8_t(value >> 8));
    out.push_back(std::uint8_t(value & 0xFFu));
}

std::uint16_t getU16(std::span<std::uint8_t const> data, std::size_t pos) {
    return std::uint16_t((std::uint16_t(data[pos]) << 8) | data[pos + 1]);
}

std::vector<std::uint8_t> makeAdu(std::uint16_t transactionId,
                                  std::uint8_t unitId,
                                  std::span<std::uint8_t const> pdu) {
    std::vector<std::uint8_t> out;
    out.reserve(kMbapSize + pdu.size());
    putU16(out, transactionId);
    putU16(out, kProtocolId);
    putU16(out, std::uint16_t(1 + pdu.size()));
    out.push_back(unitId);
    out.insert(out.end(), pdu.begin(), pdu.end());
    return out;
}

bool parseAdu(std::span<std::uint8_t const> adu,
              ResponseHeader& header,
              std::span<std::uint8_t const>& pdu,
              std::string& error) {
    if (adu.size() < kMbapSize + 1) {
        error = "short Modbus TCP ADU";
        return false;
    }
    header.transactionId = getU16(adu, 0);
    auto const protocolId = getU16(adu, 2);
    if (protocolId != kProtocolId) {
        error = "invalid Modbus protocol id";
        return false;
    }
    auto const length = getU16(adu, 4);
    if (length < 2 || adu.size() != std::size_t(kMbapSize - 1 + length)) {
        error = "invalid Modbus TCP length";
        return false;
    }
    header.unitId = adu[6];
    pdu = adu.subspan(kMbapSize);
    header.function = pdu[0];
    return true;
}

bool checkException(ResponseHeader const& header,
                    Function expectedFunction,
                    std::span<std::uint8_t const> pdu,
                    std::string& error) {
    auto const expected = static_cast<std::uint8_t>(expectedFunction);
    if (header.function == std::uint8_t(expected | 0x80u)) {
        auto const code = pdu.size() > 1 ? pdu[1] : 0;
        error = "Modbus exception " + std::to_string(code);
        return false;
    }
    if (header.function != expected) {
        error = "unexpected Modbus function";
        return false;
    }
    return true;
}

} // namespace

std::vector<std::uint8_t> buildReadRequest(ReadRequest const& req) {
    std::vector<std::uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(static_cast<std::uint8_t>(req.function));
    putU16(pdu, req.startAddress);
    putU16(pdu, req.count);
    return makeAdu(req.transactionId, req.unitId, pdu);
}

std::vector<std::uint8_t> buildWriteMultipleRegistersRequest(
    WriteMultipleRegistersRequest const& req) {
    std::vector<std::uint8_t> pdu;
    pdu.reserve(6 + req.values.size() * 2);
    pdu.push_back(static_cast<std::uint8_t>(Function::WriteMultipleRegisters));
    putU16(pdu, req.startAddress);
    putU16(pdu, std::uint16_t(req.values.size()));
    pdu.push_back(std::uint8_t(req.values.size() * 2));
    for (auto value : req.values) putU16(pdu, value);
    return makeAdu(req.transactionId, req.unitId, pdu);
}

std::vector<std::uint8_t> buildWriteSingleRegisterRequest(std::uint16_t transactionId,
                                                          std::uint8_t unitId,
                                                          std::uint16_t address,
                                                          std::uint16_t value) {
    std::vector<std::uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(0x06);
    putU16(pdu, address);
    putU16(pdu, value);
    return makeAdu(transactionId, unitId, pdu);
}

bool parseReadResponse(std::span<std::uint8_t const> adu,
                       std::uint16_t expectedTransactionId,
                       Function expectedFunction,
                       std::vector<std::uint16_t>& out,
                       std::string& error) {
    ResponseHeader header;
    std::span<std::uint8_t const> pdu;
    if (!parseAdu(adu, header, pdu, error)) return false;
    if (header.transactionId != expectedTransactionId) {
        error = "unexpected Modbus transaction id";
        return false;
    }
    if (!checkException(header, expectedFunction, pdu, error)) return false;
    if (pdu.size() < 2 || pdu[1] % 2 != 0 || pdu.size() != std::size_t(2 + pdu[1])) {
        error = "invalid Modbus read response byte count";
        return false;
    }
    out.clear();
    out.reserve(pdu[1] / 2);
    for (std::size_t pos = 2; pos < pdu.size(); pos += 2) {
        out.push_back(getU16(pdu, pos));
    }
    return true;
}

bool parseWriteMultipleRegistersResponse(std::span<std::uint8_t const> adu,
                                         std::uint16_t expectedTransactionId,
                                         std::uint16_t expectedStart,
                                         std::uint16_t expectedCount,
                                         std::string& error) {
    ResponseHeader header;
    std::span<std::uint8_t const> pdu;
    if (!parseAdu(adu, header, pdu, error)) return false;
    if (header.transactionId != expectedTransactionId) {
        error = "unexpected Modbus transaction id";
        return false;
    }
    if (!checkException(header, Function::WriteMultipleRegisters, pdu, error)) return false;
    if (pdu.size() != 5) {
        error = "invalid Modbus write response";
        return false;
    }
    if (getU16(pdu, 1) != expectedStart || getU16(pdu, 3) != expectedCount) {
        error = "Modbus write response does not match request";
        return false;
    }
    return true;
}

bool parseRequestHeader(std::span<std::uint8_t const> adu,
                        ResponseHeader& out,
                        std::span<std::uint8_t const>& pdu,
                        std::string& error) {
    return parseAdu(adu, out, pdu, error);
}

std::vector<std::uint8_t> buildReadRegistersResponse(std::uint16_t transactionId,
                                                     std::uint8_t unitId,
                                                     Function function,
                                                     std::span<std::uint16_t const> values) {
    std::vector<std::uint8_t> pdu;
    pdu.reserve(2 + values.size() * 2);
    pdu.push_back(static_cast<std::uint8_t>(function));
    pdu.push_back(std::uint8_t(values.size() * 2));
    for (auto value : values) putU16(pdu, value);
    return makeAdu(transactionId, unitId, pdu);
}

std::vector<std::uint8_t> buildWriteMultipleRegistersResponse(
    std::uint16_t transactionId,
    std::uint8_t unitId,
    std::uint16_t startAddress,
    std::uint16_t count) {
    std::vector<std::uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(static_cast<std::uint8_t>(Function::WriteMultipleRegisters));
    putU16(pdu, startAddress);
    putU16(pdu, count);
    return makeAdu(transactionId, unitId, pdu);
}

std::vector<std::uint8_t> buildWriteSingleRegisterResponse(std::uint16_t transactionId,
                                                           std::uint8_t unitId,
                                                           std::uint16_t address,
                                                           std::uint16_t value) {
    std::vector<std::uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(0x06);
    putU16(pdu, address);
    putU16(pdu, value);
    return makeAdu(transactionId, unitId, pdu);
}

std::vector<std::uint8_t> buildExceptionResponse(std::uint16_t transactionId,
                                                 std::uint8_t unitId,
                                                 std::uint8_t function,
                                                 std::uint8_t exceptionCode) {
    std::vector<std::uint8_t> pdu{std::uint8_t(function | 0x80u), exceptionCode};
    return makeAdu(transactionId, unitId, pdu);
}

// ─── Modbus RTU framing ────────────────────────────────────────────────────

namespace {

void appendCrc(std::vector<std::uint8_t>& adu) {
    auto const c = crc16(adu);
    adu.push_back(std::uint8_t(c & 0xFFu));   // low byte first
    adu.push_back(std::uint8_t(c >> 8));
}

bool rtuCrcOk(std::span<std::uint8_t const> adu) {
    if (adu.size() < 4) return false;
    auto const body = adu.subspan(0, adu.size() - 2);
    auto const c = crc16(body);
    auto const lo = adu[adu.size() - 2];
    auto const hi = adu[adu.size() - 1];
    return lo == std::uint8_t(c & 0xFFu) && hi == std::uint8_t(c >> 8);
}

} // namespace

std::uint16_t crc16(std::span<std::uint8_t const> data) {
    std::uint16_t crc = 0xFFFFu;
    for (auto byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; i++) {
            if (crc & 1u) {
                crc = std::uint16_t((crc >> 1) ^ 0xA001u);
            } else {
                crc = std::uint16_t(crc >> 1);
            }
        }
    }
    return crc;
}

std::vector<std::uint8_t> buildRtuReadRequest(std::uint8_t unitId,
                                              Function function,
                                              std::uint16_t startAddress,
                                              std::uint16_t count) {
    std::vector<std::uint8_t> adu;
    adu.reserve(8);
    adu.push_back(unitId);
    adu.push_back(static_cast<std::uint8_t>(function));
    putU16(adu, startAddress);
    putU16(adu, count);
    appendCrc(adu);
    return adu;
}

std::vector<std::uint8_t> buildRtuWriteMultipleRegistersRequest(
    std::uint8_t unitId,
    std::uint16_t startAddress,
    std::span<std::uint16_t const> values) {
    std::vector<std::uint8_t> adu;
    adu.reserve(9 + values.size() * 2);
    adu.push_back(unitId);
    adu.push_back(static_cast<std::uint8_t>(Function::WriteMultipleRegisters));
    putU16(adu, startAddress);
    putU16(adu, std::uint16_t(values.size()));
    adu.push_back(std::uint8_t(values.size() * 2));
    for (auto value : values) putU16(adu, value);
    appendCrc(adu);
    return adu;
}

std::vector<std::uint8_t> buildRtuReadRegistersResponse(std::uint8_t unitId,
                                                        Function function,
                                                        std::span<std::uint16_t const> values) {
    std::vector<std::uint8_t> adu;
    adu.reserve(5 + values.size() * 2);
    adu.push_back(unitId);
    adu.push_back(static_cast<std::uint8_t>(function));
    adu.push_back(std::uint8_t(values.size() * 2));
    for (auto value : values) putU16(adu, value);
    appendCrc(adu);
    return adu;
}

std::vector<std::uint8_t> buildRtuWriteMultipleRegistersResponse(std::uint8_t unitId,
                                                                 std::uint16_t startAddress,
                                                                 std::uint16_t count) {
    std::vector<std::uint8_t> adu;
    adu.reserve(8);
    adu.push_back(unitId);
    adu.push_back(static_cast<std::uint8_t>(Function::WriteMultipleRegisters));
    putU16(adu, startAddress);
    putU16(adu, count);
    appendCrc(adu);
    return adu;
}

std::vector<std::uint8_t> buildRtuExceptionResponse(std::uint8_t unitId,
                                                    std::uint8_t function,
                                                    std::uint8_t exceptionCode) {
    std::vector<std::uint8_t> adu{unitId, std::uint8_t(function | 0x80u), exceptionCode};
    appendCrc(adu);
    return adu;
}

bool parseRtuReadResponse(std::span<std::uint8_t const> adu,
                          std::uint8_t expectedUnit,
                          Function expectedFunction,
                          std::vector<std::uint16_t>& out,
                          std::string& error) {
    if (adu.size() < 5) {
        error = "short Modbus RTU read response";
        return false;
    }
    if (!rtuCrcOk(adu)) {
        error = "Modbus RTU CRC mismatch";
        return false;
    }
    if (adu[0] != expectedUnit) {
        error = "unexpected Modbus RTU unit id";
        return false;
    }
    auto const expected = static_cast<std::uint8_t>(expectedFunction);
    if (adu[1] == std::uint8_t(expected | 0x80u)) {
        error = "Modbus exception " + std::to_string(adu[2]);
        return false;
    }
    if (adu[1] != expected) {
        error = "unexpected Modbus function";
        return false;
    }
    auto const byteCount = adu[2];
    if (byteCount % 2 != 0 || adu.size() != std::size_t(3 + byteCount + 2)) {
        error = "invalid Modbus RTU read byte count";
        return false;
    }
    out.clear();
    out.reserve(byteCount / 2);
    for (std::size_t pos = 3; pos < std::size_t(3 + byteCount); pos += 2) {
        out.push_back(getU16(adu, pos));
    }
    return true;
}

bool parseRtuWriteMultipleRegistersResponse(std::span<std::uint8_t const> adu,
                                            std::uint8_t expectedUnit,
                                            std::uint16_t expectedStart,
                                            std::uint16_t expectedCount,
                                            std::string& error) {
    if (adu.size() != 8) {
        error = "invalid Modbus RTU write response";
        return false;
    }
    if (!rtuCrcOk(adu)) {
        error = "Modbus RTU CRC mismatch";
        return false;
    }
    if (adu[0] != expectedUnit) {
        error = "unexpected Modbus RTU unit id";
        return false;
    }
    auto const expected = static_cast<std::uint8_t>(Function::WriteMultipleRegisters);
    if (adu[1] == std::uint8_t(expected | 0x80u)) {
        error = "Modbus exception " + std::to_string(adu[2]);
        return false;
    }
    if (adu[1] != expected) {
        error = "unexpected Modbus function";
        return false;
    }
    if (getU16(adu, 2) != expectedStart || getU16(adu, 4) != expectedCount) {
        error = "Modbus RTU write response does not match request";
        return false;
    }
    return true;
}

bool parseRtuRequest(std::span<std::uint8_t const> adu,
                     std::uint8_t& unitId,
                     std::span<std::uint8_t const>& pdu,
                     std::string& error) {
    if (adu.size() < 4) {
        error = "short Modbus RTU request";
        return false;
    }
    if (!rtuCrcOk(adu)) {
        error = "Modbus RTU CRC mismatch";
        return false;
    }
    unitId = adu[0];
    pdu = adu.subspan(1, adu.size() - 3);  // drop unit id + CRC
    return true;
}

} // namespace nmbs
