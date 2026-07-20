// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/dp/Datapoint.h"

#include <QMutex>
#include <QMutexLocker>
#include <stdexcept>
#include <utility>

namespace core::dp {

class Datapoint::Impl {
public:
    mutable QMutex mtx;
    DatapointSpec  spec;
    QVariant       value;
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
    if (!m_impl->spec.id.isEmpty() && spec.id != m_impl->spec.id) {
        throw std::invalid_argument("Datapoint id is immutable once assigned");
    }
    m_impl->spec = std::move(spec);
}

QString Datapoint::id() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->spec.id;
}

QVariant Datapoint::value() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->value;
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

std::optional<PortRef> Datapoint::source() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->spec.source;
}

std::optional<PortRef> Datapoint::sink() const {
    QMutexLocker lk(&m_impl->mtx);
    return m_impl->spec.sink;
}

void Datapoint::setValue(QVariant v, QDateTime ts) {
    bool valueChangedFlag = false;
    bool stateChangedFlag = false;
    bool timestampChangedFlag = false;
    {
        QMutexLocker lk(&m_impl->mtx);
        if (m_impl->state != DpState::Ok) {
            m_impl->state    = DpState::Ok;
            stateChangedFlag = true;
        }
        if (m_impl->value != v) {
            m_impl->value    = std::move(v);
            valueChangedFlag = true;
        }
        // Refresh timestamp even when value is unchanged so consumers that
        // look at staleness see liveness. It has its own notification to avoid
        // making value-only QML bindings re-evaluate on every poll.
        if (m_impl->timestamp != ts) {
            m_impl->timestamp       = std::move(ts);
            timestampChangedFlag    = true;
        }
    }
    if (valueChangedFlag) emit valueChanged();
    if (stateChangedFlag) emit stateChanged();
    if (timestampChangedFlag) emit timestampChanged();
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
        emit stateChanged();
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
    if (cb) cb(v);
}

} // namespace core::dp
