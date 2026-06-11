// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// --- core-base ---------------------------------------------------------------
// First citizen of the Qt-free core-base layer. The register-table vocabulary
// used across the core abstraction (transport seam, datapoints, modules) must
// not drag Qt SerialBus into headers, otherwise no Qt-free (gateway) transport
// can implement the abstraction. This enum mirrors the Modbus table space; the
// single Qt boundary (Modbus transports) converts via the private bridge
// src/ModbusRegisterTable.h (toQModbus / fromQModbus).
//
// Values intentionally mirror QModbusDataUnit::RegisterType semantics so the
// bridge is a 1:1 switch.

namespace core {

enum class RegisterTable {
    Invalid,
    DiscreteInput,    // read-only bit
    Coil,             // read/write bit
    InputRegister,    // read-only 16-bit
    HoldingRegister,  // read/write 16-bit
};

} // namespace core
