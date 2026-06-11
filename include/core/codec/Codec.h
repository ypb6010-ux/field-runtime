// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QList>
#include <QString>
#include <QVariant>

#include "core/core_global.h"

namespace core::dp { struct PortRef; }

namespace core::codec {

class CORE_EXPORT Codec {
public:
    virtual ~Codec() = default;
    virtual QString id() const = 0;

    // Decode raw Modbus registers into a typed value.
    virtual QVariant       decode(QList<quint16> const& raw,
                                  dp::PortRef const&     ref) = 0;

    // Encode a typed value into raw Modbus registers ready to be written.
    virtual QList<quint16> encode(QVariant const&        value,
                                  dp::PortRef const&     ref) = 0;
};

} // namespace core::codec
