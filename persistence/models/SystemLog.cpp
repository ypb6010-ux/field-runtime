// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "SystemLog.h"

QX_REGISTER_CPP_USER(SystemLog)

namespace qx {
template <> void register_class(QxClass<SystemLog>& t) {
    t.setName("system_log");
    t.id(&SystemLog::id, "id");
    t.data(&SystemLog::ts,       "ts")->setNotNull(true);
    t.data(&SystemLog::level,    "level");
    t.data(&SystemLog::category, "category");
    t.data(&SystemLog::source,   "source");
    t.data(&SystemLog::message,  "message");
}
}
