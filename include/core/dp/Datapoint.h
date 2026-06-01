#pragma once

#include <optional>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariant>

#include "core/core_global.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"

namespace core::dp {

enum class Kind {
    Status,
    Command,
    Bidirectional,
};

enum class DpState {
    Ok,
    Stale,
    Error,
    Missing,
};

// Datapoint is exposed to QML as a QObject with the `value` Q_PROPERTY and
// NOTIFY signal, so QML can bind directly and update on change without manual
// Connections blocks. See design 6.1.
class CORE_EXPORT Datapoint : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString  id      READ id       CONSTANT)
    Q_PROPERTY(QVariant value   READ value    NOTIFY valueChanged)
    Q_PROPERTY(bool     valid   READ valid    NOTIFY valueChanged)
    Q_PROPERTY(QDateTime ts     READ timestamp NOTIFY valueChanged)
    Q_PROPERTY(QString  state   READ stateText NOTIFY stateChanged)
public:
    explicit Datapoint(QObject* parent = nullptr);

    QString    id()        const;
    QVariant   value()     const;
    bool       valid()     const;
    QDateTime  timestamp() const;
    DpState    state()     const;
    QString    stateText() const;

    Kind                          kind() const;
    ScalarType                    type() const;
    std::optional<PortRef> const& source() const;
    std::optional<PortRef> const& sink()   const;
    QString                       uiBinding()  const;
    QString                       persistTag() const;

    // Called by codecs / router. Emits valueChanged on the main thread.
    void setValue(QVariant v, QDateTime ts = QDateTime::currentDateTime());
    void setState(DpState s);

    // For Command / Bidirectional datapoints — invoked from QML.
    Q_INVOKABLE void write(QVariant v);

signals:
    void valueChanged();
    void stateChanged();

private:
    class Impl;
    Impl* m_impl;
};

} // namespace core::dp
