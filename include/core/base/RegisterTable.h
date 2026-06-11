// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// --- core-base ---------------------------------------------------------------
// Qt-free register vocabulary shared across the core abstraction (transport
// seam, datapoints, modules, codecs). Keeping these out of Qt headers is what
// lets a Qt-free (gateway) transport implement the abstraction. The enum
// mirrors the Modbus table space; the single Qt boundary (Modbus transports)
// converts via include/core/transport/RegisterTableQt.h (toQModbus/fromQModbus).
//
// RegisterWords is the raw 16-bit register payload — std::vector<uint16_t>
// instead of QList<quint16> so no QtCore container leaks into the abstraction.

#include <cstdint>
#include <vector>

namespace core {

enum class RegisterTable {
    Invalid,
    DiscreteInput,    // read-only bit
    Coil,             // read/write bit
    InputRegister,    // read-only 16-bit
    HoldingRegister,  // read/write 16-bit
};

using RegisterWords = std::vector<std::uint16_t>;

} // namespace core
