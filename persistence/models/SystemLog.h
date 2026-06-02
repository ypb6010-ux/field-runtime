#pragma once
#include <QString>
#include <QxOrm.h>
#include "../CorePersistence_global.h"

// 系统 / 诊断日志 —— transport / scheduler / module / config 的技术事件。
class COREPERSISTENCE_EXPORT SystemLog {
public:
    long    id  = 0;
    quint64 ts  = 0;        // epoch milliseconds
    int     level = 0;      // core::log::LogLevel value
    QString category;
    QString source;
    QString message;
};

QX_REGISTER_PRIMARY_KEY(SystemLog, long)
QX_REGISTER_HPP_USER(SystemLog, qx::trait::no_base_class_defined, 1)
