// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <optional>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariant>

#include "core/core_global.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/dp/Value.h"

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

// Static configuration captured at construction or via setSpec(). Once a
// datapoint is wired into the runtime the spec does not change; mutable
// runtime state is `value`, `state`, `timestamp`.
struct DatapointSpec {
    QString                id;
    Kind                   kind = Kind::Status;
    ScalarType             type = ScalarType::U16;
    std::optional<PortRef> source;
    std::optional<PortRef> sink;
    QString                uiBinding;
    QString                persistTag;
};

// Datapoint is exposed to QML as a QObject with the `value` Q_PROPERTY and
// NOTIFY signal, so QML can bind directly and update on change without
// manual Connections blocks. See design 6.1.
class CORE_EXPORT Datapoint : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString    id        READ id        CONSTANT)
    Q_PROPERTY(QVariant   value     READ value     NOTIFY valueChanged)
    Q_PROPERTY(bool       valid     READ valid     NOTIFY valueChanged)
    Q_PROPERTY(QDateTime  ts        READ timestamp NOTIFY valueChanged)
    Q_PROPERTY(QString    state     READ stateText NOTIFY stateChanged)
public:
    using Writer = std::function<void(Value const&)>;

    explicit Datapoint(QObject* parent = nullptr);
    explicit Datapoint(DatapointSpec spec, QObject* parent = nullptr);
    ~Datapoint() override;

    CORE_DISABLE_COPY_MOVE(Datapoint)

    void setSpec(DatapointSpec spec);

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

    // Push a decoded value from a codec / router. valueChanged is emitted
    // only if (a) state transitions to Ok or (b) the value differs from the
    // current one. Thread-safe — emissions cross thread boundaries via Qt's
    // signal/slot machinery.
    void setValue(Value v, QDateTime ts = QDateTime::currentDateTime());
    void setState(DpState s);

    // Register a writer invoked by `write()`. The Core wires this to a
    // SinkWindow stage operation; tests can supply a lambda directly.
    void setWriter(Writer w);

    // For Command / Bidirectional datapoints — invoked from QML or business
    // code. Forwards to the registered writer; no-op if none.
    Q_INVOKABLE void write(QVariant v);

signals:
    void valueChanged();
    void stateChanged();

private:
    class Impl;
    Impl* m_impl;
};

} // namespace core::dp
