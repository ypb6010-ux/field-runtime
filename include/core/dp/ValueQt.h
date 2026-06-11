// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Qt boundary bridge for the Qt-free dp::Value. Marshals Value <-> QVariant at
// the QML edge (and anywhere a Qt product still speaks QVariant). Kept out of
// the Qt-free core-base headers; only the QML bridge / Qt layer includes this.

#include <QMetaType>
#include <QString>
#include <QVariant>

#include "core/dp/Value.h"

namespace core::dp {

inline QVariant toQVariant(Value const& v) {
    return std::visit([](auto const& x) -> QVariant {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>)      return {};
        else if constexpr (std::is_same_v<T, std::string>)    return QString::fromStdString(x);
        else if constexpr (std::is_same_v<T, bool>)           return x;
        else if constexpr (std::is_same_v<T, std::int64_t>)   return QVariant::fromValue(qlonglong(x));
        else if constexpr (std::is_same_v<T, std::uint64_t>)  return QVariant::fromValue(qulonglong(x));
        else                                                  return x;   // double
    }, v);
}

inline Value fromQVariant(QVariant const& q) {
    switch (q.typeId()) {
        case QMetaType::UnknownType: return std::monostate{};
        case QMetaType::Bool:        return q.toBool();
        case QMetaType::Double:      return q.toDouble();
        case QMetaType::Float:       return double(q.toFloat());
        case QMetaType::Short:
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::LongLong:    return std::int64_t(q.toLongLong());
        case QMetaType::UShort:
        case QMetaType::UInt:
        case QMetaType::ULong:
        case QMetaType::ULongLong:   return std::uint64_t(q.toULongLong());
        case QMetaType::QString:     return q.toString().toStdString();
        default:
            if (!q.isValid())            return std::monostate{};
            if (q.canConvert<double>())  return q.toDouble();
            return q.toString().toStdString();
    }
}

} // namespace core::dp
