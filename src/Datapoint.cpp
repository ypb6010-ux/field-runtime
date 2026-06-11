// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/dp/Datapoint.h"

#include <QMutex>
#include <QMutexLocker>
#include <utility>

#include "core/dp/ValueQt.h"

namespace core::dp {

class Datapoint::Impl {
public:
    mutable QMutex mtx;
    DatapointSpec  spec;
    Value          value;
    DpState        state = DpState::Missing;
    QDateTime      timestamp;
    Writer         writer;
};

Datapoint::Datapoint(QObject* parent) : QObject(parent), m_impl(new Impl) {}

Datapoint::Datapoint(DatapointSpec spec, QObject* parent)
    : QObject(parent)
    , m_impl(new Impl) {
    m_impl->spec = std::move(spec);
}

Datapoint::~Datapoint() { delete m_impl; }

void Datapoint::setSpec(DatapointSpec spec) {
    QMutexLocker lk(&m_impl->mtx);
    m_impl->spec = std::move(spec);
}

QString Datapoint::id() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->spec.id;
}

QVariant Datapoint::value() const {
    QMutexLocker lk(&m_impl->mtx);
    return toQVariant(m_impl->value);
}

bool Datapoint::valid() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->state == DpState::Ok;
}

QDateTime Datapoint::timestamp() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->timestamp;
}

DpState Datapoint::state() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->state;
}

QString Datapoint::stateText() const {
    switch (state()) {
        case DpState::Ok:      return QStringLiteral("Ok");
        case DpState::Stale:   return QStringLiteral("Stale");
        case DpState::Error:   return QStringLiteral("Error");
        case DpState::Missing: return QStringLiteral("Missing");
    }
    return QStringLiteral("Missing");
}

Kind       Datapoint::kind() const { QMutexLocker lk(&m_impl->mtx); return m_impl->spec.kind; }
ScalarType Datapoint::type() const { QMutexLocker lk(&m_impl->mtx); return m_impl->spec.type; }
QString    Datapoint::uiBinding()  const { QMutexLocker lk(&m_impl->mtx); return m_impl->spec.uiBinding; }
QString    Datapoint::persistTag() const { QMutexLocker lk(&m_impl->mtx); return m_impl->spec.persistTag; }

std::optional<PortRef> const& Datapoint::source() const { return m_impl->spec.source; }
std::optional<PortRef> const& Datapoint::sink()   const { return m_impl->spec.sink; }

void Datapoint::setValue(Value v, QDateTime ts) {
    bool valueChangedFlag = false;
    bool stateChangedFlag = false;
    {
        QMutexLocker lk(&m_impl->mtx);
        if (m_impl->state != DpState::Ok) {
            m_impl->state    = DpState::Ok;
            stateChangedFlag = true;
        }
        if (m_impl->value != v) {
            m_impl->value     = std::move(v);
            m_impl->timestamp = std::move(ts);
            valueChangedFlag  = true;
        } else {
            // Refresh timestamp even when value is unchanged so consumers
            // that look at staleness see liveness.
            m_impl->timestamp = std::move(ts);
        }
    }
    if (valueChangedFlag) emit valueChanged();
    if (stateChangedFlag) emit stateChanged();
}

void Datapoint::setState(DpState s) {
    bool changed = false;
    {
        QMutexLocker lk(&m_impl->mtx);
        if (m_impl->state != s) {
            m_impl->state = s;
            changed       = true;
        }
    }
    if (changed) {
        // valueChanged is emitted so `valid` rebinds in QML; the dp value
        // itself has not changed, but its validity has.
        emit stateChanged();
        emit valueChanged();
    }
}

void Datapoint::setWriter(Writer w) {
    QMutexLocker lk(&m_impl->mtx);
    m_impl->writer = std::move(w);
}

void Datapoint::write(QVariant v) {
    Writer cb;
    {
        QMutexLocker lk(&m_impl->mtx);
        cb = m_impl->writer;
    }
    if (cb) cb(fromQVariant(v));
}

} // namespace core::dp
