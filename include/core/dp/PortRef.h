// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <QString>
#include <QtSerialBus/QModbusDataUnit>

#include "core/core_global.h"
#include "core/dp/WordOrder.h"

namespace core::codec { class Codec; }

namespace core::dp {

// A concrete physical-layer address for a datapoint's source or sink.
// Composed of (transport, table, address, optional bit) plus the codec
// pipeline parameters described in design 4.5 (word order, shift, mask,
// scale, offset, optional custom codec).
struct PortRef {
    QString                            transport;
    QModbusDataUnit::RegisterType      table = QModbusDataUnit::HoldingRegisters;
    int                                address = 0;
    std::optional<int>                 bit;
    WordOrder                          wordOrder = WordOrder::ABCD;
    int                                shift = 0;
    quint64                            mask = 0xFFFFFFFFFFFFFFFFull;
    double                             scale = 1.0;
    double                             offset = 0.0;
    std::shared_ptr<core::codec::Codec> codec;
    // Sink-only — names the SinkWindow module-id that owns this register.
    // When set, router code stages into that SinkWindow rather than the
    // raw transport at `address`.
    QString                             window;
};

} // namespace core::dp
