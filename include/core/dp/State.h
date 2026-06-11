// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// --- core-base ---------------------------------------------------------------
// Qt-free reactive runtime state of a datapoint: the decoded value, its
// validity/quality, and the timestamp of the last update. This is the part the
// core logic (poll/route) reads and writes; the QObject Datapoint (Qt layer)
// composes a State and marshals it to QML (value -> QVariant, timestamp ->
// QDateTime). Keeping State Qt-free is what lets a Qt-free build carry datapoint
// state without QtCore.

#include <chrono>

#include "core/dp/Value.h"

namespace core::dp {

enum class DpState {
    Ok,
    Stale,
    Error,
    Missing,
};

using Timestamp = std::chrono::system_clock::time_point;

struct State {
    Value     value;
    DpState   state = DpState::Missing;
    Timestamp timestamp{};
};

} // namespace core::dp
