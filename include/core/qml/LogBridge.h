#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

#include "core/core_global.h"

namespace core::log { class Logger; }

namespace core::qml {

// Thin QObject exposed to QML as the context property `log`. UI code emits
// through this controlled facade instead of touching the C++ Logger directly,
// so the encapsulation boundary holds: QML can only EMIT (write-only, value
// types), never register sinks or reconfigure the pipeline.
class CORE_EXPORT LogBridge : public QObject {
    Q_OBJECT
public:
    explicit LogBridge(log::Logger& logger, QObject* parent = nullptr);
    ~LogBridge() override;

    // System / diagnostic. `category` groups the message (e.g. "ui", "alarm").
    Q_INVOKABLE void trace(QString category, QString message);
    Q_INVOKABLE void debug(QString category, QString message);
    Q_INVOKABLE void info (QString category, QString message);
    Q_INVOKABLE void warn (QString category, QString message);
    Q_INVOKABLE void error(QString category, QString message);

    // Run / operation / audit. actor defaults to "ui:user"; records who did
    // what to which target with what result — always persisted, never filtered.
    Q_INVOKABLE void operation(QString action,
                               QString target,
                               QVariant oldValue,
                               QVariant newValue,
                               QString result,
                               QString note = QString());

private:
    log::Logger& m_logger;
};

} // namespace core::qml
