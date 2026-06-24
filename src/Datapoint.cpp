// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/dp/Datapoint.h"

#include <mutex>
#include <utility>

namespace core::dp {

class Datapoint::Impl {
public:
    mutable std::mutex mtx;
    DatapointSpec      spec;
    State              st;       // value + state + timestamp
    Writer             writer;
    ChangeCallback     onValueChanged;
    ChangeCallback     onStateChanged;
    std::optional<Value> disconnectValue;  // reset target on disconnect (nullopt = hold)
};

Datapoint::Datapoint() : m_impl(new Impl) {}

Datapoint::Datapoint(DatapointSpec spec) : m_impl(new Impl) {
    m_impl->spec = std::move(spec);
}

Datapoint::~Datapoint() { delete m_impl; }

void Datapoint::setSpec(DatapointSpec spec) {
    std::lock_guard lk(m_impl->mtx);
    m_impl->spec = std::move(spec);
}

std::string Datapoint::id() const {
    std::lock_guard lk(m_impl->mtx);
    return m_impl->spec.id;
}

Value Datapoint::value() const {
    std::lock_guard lk(m_impl->mtx);
    return m_impl->st.value;
}

bool Datapoint::valid() const {
    std::lock_guard lk(m_impl->mtx);
    return m_impl->st.state == DpState::Ok;
}

Timestamp Datapoint::timestamp() const {
    std::lock_guard lk(m_impl->mtx);
    return m_impl->st.timestamp;
}

DpState Datapoint::state() const {
    std::lock_guard lk(m_impl->mtx);
    return m_impl->st.state;
}

State Datapoint::snapshot() const {
    std::lock_guard lk(m_impl->mtx);
    return m_impl->st;
}

std::string Datapoint::stateText() const {
    switch (state()) {
        case DpState::Ok:      return "Ok";
        case DpState::Stale:   return "Stale";
        case DpState::Error:   return "Error";
        case DpState::Missing: return "Missing";
    }
    return "Missing";
}

Kind       Datapoint::kind() const { std::lock_guard lk(m_impl->mtx); return m_impl->spec.kind; }
ScalarType Datapoint::type() const { std::lock_guard lk(m_impl->mtx); return m_impl->spec.type; }
std::string Datapoint::uiBinding()  const { std::lock_guard lk(m_impl->mtx); return m_impl->spec.uiBinding; }
std::string Datapoint::persistTag() const { std::lock_guard lk(m_impl->mtx); return m_impl->spec.persistTag; }

std::optional<PortRef> const& Datapoint::source() const { return m_impl->spec.source; }
std::optional<PortRef> const& Datapoint::sink()   const { return m_impl->spec.sink; }

void Datapoint::setValue(Value v, Timestamp ts) {
    bool valueChangedFlag = false;
    bool stateChangedFlag = false;
    {
        std::lock_guard lk(m_impl->mtx);
        if (m_impl->st.state != DpState::Ok) {
            m_impl->st.state = DpState::Ok;
            stateChangedFlag = true;
        }
        if (m_impl->st.value != v) {
            m_impl->st.value     = std::move(v);
            m_impl->st.timestamp = ts;
            valueChangedFlag     = true;
        } else {
            // Refresh timestamp even when value is unchanged so consumers
            // that look at staleness see liveness.
            m_impl->st.timestamp = ts;
        }
    }
    if (valueChangedFlag && m_impl->onValueChanged) m_impl->onValueChanged();
    if (stateChangedFlag && m_impl->onStateChanged) m_impl->onStateChanged();
}

void Datapoint::setState(DpState s) {
    bool changed = false;
    {
        std::lock_guard lk(m_impl->mtx);
        if (m_impl->st.state != s) {
            m_impl->st.state = s;
            changed          = true;
        }
    }
    if (changed) {
        // value did not change, but its validity did: notify both so a QML
        // `valid` binding rebinds (mirrors the old dual-signal behaviour).
        if (m_impl->onStateChanged) m_impl->onStateChanged();
        if (m_impl->onValueChanged) m_impl->onValueChanged();
    }
}

void Datapoint::setDisconnectValue(std::optional<Value> v) {
    std::lock_guard lk(m_impl->mtx);
    m_impl->disconnectValue = std::move(v);
}

void Datapoint::markDisconnected() {
    bool changed = false;
    {
        std::lock_guard lk(m_impl->mtx);
        if (m_impl->disconnectValue.has_value()
            && m_impl->st.value != *m_impl->disconnectValue) {
            m_impl->st.value     = *m_impl->disconnectValue;
            m_impl->st.timestamp = std::chrono::system_clock::now();
            changed = true;
        }
        if (m_impl->st.state != DpState::Error) {
            m_impl->st.state = DpState::Error;
            changed = true;
        }
    }
    if (changed) {
        if (m_impl->onStateChanged) m_impl->onStateChanged();
        if (m_impl->onValueChanged) m_impl->onValueChanged();
    }
}

void Datapoint::setWriter(Writer w) {
    std::lock_guard lk(m_impl->mtx);
    m_impl->writer = std::move(w);
}

void Datapoint::write(Value v) {
    Writer cb;
    {
        std::lock_guard lk(m_impl->mtx);
        cb = m_impl->writer;
    }
    if (cb) cb(v);
}

void Datapoint::setOnValueChanged(ChangeCallback cb) {
    std::lock_guard lk(m_impl->mtx);
    m_impl->onValueChanged = std::move(cb);
}

void Datapoint::setOnStateChanged(ChangeCallback cb) {
    std::lock_guard lk(m_impl->mtx);
    m_impl->onStateChanged = std::move(cb);
}

} // namespace core::dp
