// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/qml/QtDatapoint.h"

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/dp/Datapoint.h"
#include "core/dp/TimeQt.h"
#include "core/dp/ValueQt.h"

namespace core::qml {

QtDatapoint::QtDatapoint(std::shared_ptr<dp::Datapoint> model,
                         bus::EventBus& bus,
                         QObject* parent)
    : QObject(parent), m_model(std::move(model)) {
    // Re-emit on every DpChanged for this datapoint. Core publishes DpChanged
    // on the bus thread, so `changed()` is delivered on that thread (the QML
    // engine thread in a desktop app).
    m_sub = bus.subscribe<bus::DpChanged>([this](bus::DpChanged const& e) {
        if (m_model && e.id == m_model->id()) emit changed();
    });
}

QtDatapoint::~QtDatapoint() = default;

QString QtDatapoint::id() const {
    return m_model ? QString::fromStdString(m_model->id()) : QString();
}

QVariant QtDatapoint::value() const {
    return m_model ? dp::toQVariant(m_model->value()) : QVariant();
}

bool QtDatapoint::valid() const {
    return m_model && m_model->valid();
}

QDateTime QtDatapoint::ts() const {
    return m_model ? dp::toQDateTime(m_model->timestamp()) : QDateTime();
}

QString QtDatapoint::state() const {
    return m_model ? QString::fromStdString(m_model->stateText()) : QString();
}

void QtDatapoint::write(QVariant v) {
    if (m_model) m_model->write(dp::fromQVariant(v));
}

} // namespace core::qml
