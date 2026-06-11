// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Qt boundary bridge: converts the Qt-free core::RegisterTable vocabulary
// (core-base) to/from Qt SerialBus' QModbusDataUnit::RegisterType. Lives in the
// Qt transport layer (this header pulls in Qt SerialBus), keeping the Qt
// dependency at the boundary and out of the Qt-free core-base headers under
// include/core/base/. Public so Qt-side consumers (transports, test fixtures,
// product adapters) share one definition.

#include <QtSerialBus/QModbusDataUnit>

#include "core/base/RegisterTable.h"

namespace core {

inline QModbusDataUnit::RegisterType toQModbus(RegisterTable t) {
    switch(t){
        case RegisterTable::DiscreteInput:   return QModbusDataUnit::DiscreteInputs;
        case RegisterTable::Coil:            return QModbusDataUnit::Coils;
        case RegisterTable::InputRegister:   return QModbusDataUnit::InputRegisters;
        case RegisterTable::HoldingRegister: return QModbusDataUnit::HoldingRegisters;
        case RegisterTable::Invalid:         break;
    }
    return QModbusDataUnit::Invalid;
}

inline RegisterTable fromQModbus(QModbusDataUnit::RegisterType t) {
    switch(t){
        case QModbusDataUnit::DiscreteInputs:   return RegisterTable::DiscreteInput;
        case QModbusDataUnit::Coils:            return RegisterTable::Coil;
        case QModbusDataUnit::InputRegisters:   return RegisterTable::InputRegister;
        case QModbusDataUnit::HoldingRegisters: return RegisterTable::HoldingRegister;
        case QModbusDataUnit::Invalid:          break;
    }
    return RegisterTable::Invalid;
}

} // namespace core
