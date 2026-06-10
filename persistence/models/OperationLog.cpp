// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "OperationLog.h"

QX_REGISTER_CPP_USER(OperationLog)

namespace qx {
template <> void register_class(QxClass<OperationLog>& t) {
    t.setName("operation_log");
    t.id(&OperationLog::id, "id");
    t.data(&OperationLog::ts,        "ts")->setNotNull(true);
    t.data(&OperationLog::actor,     "actor");
    t.data(&OperationLog::action,    "action");
    t.data(&OperationLog::target,    "target");
    t.data(&OperationLog::old_value, "old_value");
    t.data(&OperationLog::new_value, "new_value");
    t.data(&OperationLog::result,    "result");
    t.data(&OperationLog::note,      "note");
}
}
