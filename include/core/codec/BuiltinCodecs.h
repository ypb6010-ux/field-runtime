// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <unordered_map>
#include <QString>

#include "core/core_global.h"
#include "core/codec/Codec.h"
#include "core/dp/ScalarType.h"

namespace core::codec {

// BuiltinScalarCodec — pure-C++ codec covering every ScalarType except enum
// and string lookups. The decode pipeline (design §4.5) is:
//
//   1. WordOrder byte permutation (multi-register types only)
//   2. ScalarType byte → native value (incl. IEEE-754 reinterpretation)
//   3. shift / mask extraction (integer types only)
//   4. scale * raw + offset linear transform
//
// `encode` runs the same pipeline in reverse. Stateless; instances are safe
// to share between datapoints.
class CORE_EXPORT BuiltinScalarCodec : public Codec {
public:
    explicit BuiltinScalarCodec(dp::ScalarType type);

    QString        id() const override;
    QVariant       decode(QList<quint16> const& raw,
                          dp::PortRef const&     ref) override;
    QList<quint16> encode(QVariant const&        value,
                          dp::PortRef const&     ref) override;

    dp::ScalarType scalarType() const noexcept { return m_type; }

    // Canonical builtin id for a given scalar type: "builtin.<scalar>".
    static QString idFor(dp::ScalarType type);

private:
    dp::ScalarType m_type;
};

// EnumU16Codec — wraps a U16 scalar decode with a map<u16, QString>. Missing
// keys decode to "Unknown(<n>)" so the datapoint always carries a value.
class CORE_EXPORT EnumU16Codec : public Codec {
public:
    EnumU16Codec(QString id, std::unordered_map<quint16, QString> map);

    QString        id() const override;
    QVariant       decode(QList<quint16> const& raw,
                          dp::PortRef const&     ref) override;
    QList<quint16> encode(QVariant const&        value,
                          dp::PortRef const&     ref) override;

private:
    QString                                m_id;
    std::unordered_map<quint16, QString>   m_forward;   // raw → name
    std::unordered_map<QString, quint16>   m_reverse;   // name → raw
};

} // namespace core::codec
