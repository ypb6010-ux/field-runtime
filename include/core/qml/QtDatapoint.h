// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariant>

#include "core/core_global.h"
#include "core/bus/Subscription.h"

namespace core::dp  { class Datapoint; }
namespace core::bus { class EventBus; }

namespace core::qml {

// Qt/QML wrapper over the Qt-free dp::Datapoint model. Exposes the runtime
// state as Q_PROPERTYs so QML binds to `value` directly; the NOTIFY signal is
// driven by the EventBus DpChanged stream (the canonical change channel that
// Core publishes), so the wrapper stays decoupled from the model's single
// onValueChanged slot.
class CORE_EXPORT QtDatapoint : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString   id    READ id    CONSTANT)
    Q_PROPERTY(QVariant  value READ value NOTIFY changed)
    Q_PROPERTY(bool      valid READ valid NOTIFY changed)
    Q_PROPERTY(QDateTime ts    READ ts    NOTIFY changed)
    Q_PROPERTY(QString   state READ state NOTIFY changed)
public:
    QtDatapoint(std::shared_ptr<dp::Datapoint> model,
                bus::EventBus& bus,
                QObject* parent = nullptr);
    ~QtDatapoint() override;

    QString   id()    const;
    QVariant  value() const;
    bool      valid() const;
    QDateTime ts()    const;
    QString   state() const;

    Q_INVOKABLE void write(QVariant v);

signals:
    void changed();

private:
    std::shared_ptr<dp::Datapoint> m_model;
    bus::Subscription              m_sub;
};

} // namespace core::qml
