// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <QString>
#include <QxOrm.h>
#include "../CorePersistence_global.h"

// 运行 / 操作日志(审计)—— 谁在何时对什么做了什么,结果如何。
class COREPERSISTENCE_EXPORT OperationLog {
public:
    long    id  = 0;
    quint64 ts  = 0;        // epoch milliseconds
    QString actor;          // "ui:user" / "operator-box" / "auto"
    QString action;
    QString target;
    QString old_value;
    QString new_value;
    QString result;
    QString note;
};

QX_REGISTER_PRIMARY_KEY(OperationLog, long)
QX_REGISTER_HPP_USER(OperationLog, qx::trait::no_base_class_defined, 1)
