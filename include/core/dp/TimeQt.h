// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Qt boundary bridge for the Qt-free dp::Timestamp (std::chrono). Marshals to/
// from QDateTime at the QML edge / Qt consumers. Kept out of core-base headers.

#include <chrono>

#include <QDateTime>

#include "core/dp/State.h"

namespace core::dp {

inline QDateTime toQDateTime(Timestamp tp) {
    if (tp.time_since_epoch().count() == 0) {
        return {};   // null timestamp -> null QDateTime
    }
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        tp.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms);
}

inline Timestamp fromQDateTime(QDateTime const& dt) {
    if (!dt.isValid()) {
        return {};
    }
    return Timestamp{} + std::chrono::milliseconds(dt.toMSecsSinceEpoch());
}

} // namespace core::dp
