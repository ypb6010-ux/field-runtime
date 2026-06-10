// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <QString>
#include <QxOrm.h>
#include "../CorePersistence_global.h"

// 采集数据历史 —— 每个带 persistTag 的 datapoint 变化一行。
class COREPERSISTENCE_EXPORT Telemetry {
public:
    long    id      = 0;
    QString tag;            // datapoint persistTag
    quint64 ts      = 0;    // epoch milliseconds
    QString value;          // serialized scalar
    int     quality = 0;    // DpState: 0 Ok / 1 Stale / 2 Error / 3 Missing
};

QX_REGISTER_PRIMARY_KEY(Telemetry, long)
QX_REGISTER_HPP_USER(Telemetry, qx::trait::no_base_class_defined, 1)
