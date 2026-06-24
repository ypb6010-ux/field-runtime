// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "core/core_global.h"
#include "core/dp/PortRef.h"
#include "core/dp/ScalarType.h"
#include "core/dp/State.h"

namespace core::dp {

enum class Kind {
    Status,
    Command,
    Bidirectional,
};
// DpState and the Qt-free runtime State live in core/dp/State.h.

// Static configuration captured at construction or via setSpec(). Once a
// datapoint is wired into the runtime the spec does not change; mutable
// runtime state is `value`, `state`, `timestamp`.
struct DatapointSpec {
    std::string            id;
    Kind                   kind = Kind::Status;
    ScalarType             type = ScalarType::U16;
    std::optional<PortRef> source;
    std::optional<PortRef> sink;
    std::string            uiBinding;
    std::string            persistTag;
};

// Datapoint — the Qt-free runtime model: id/value/state/timestamp/writer plus
// change-notification callbacks. The Qt layer wraps it (core/qml/QtDatapoint)
// to expose a `value` Q_PROPERTY for QML; Core's DpChanged publisher registers
// an onValueChanged callback. Keeping this Qt-free is what lets a Qt-free build
// carry datapoint runtime state without QtCore.
class CORE_EXPORT Datapoint {
public:
    using Writer         = std::function<void(Value const&)>;
    using ChangeCallback = std::function<void()>;

    Datapoint();
    explicit Datapoint(DatapointSpec spec);
    ~Datapoint();

    CORE_DISABLE_COPY_MOVE(Datapoint)

    void setSpec(DatapointSpec spec);

    std::string id()        const;
    Value       value()     const;
    bool        valid()     const;
    Timestamp   timestamp() const;
    DpState     state()     const;
    std::string stateText() const;

    // Qt-free snapshot of the reactive runtime state (value + state +
    // timestamp) taken atomically under one lock.
    State       snapshot()  const;

    Kind                          kind() const;
    ScalarType                    type() const;
    std::optional<PortRef> const& source() const;
    std::optional<PortRef> const& sink()   const;
    std::string                   uiBinding()  const;
    std::string                   persistTag() const;

    // Push a decoded value from a codec / router. onValueChanged fires only if
    // (a) state transitions to Ok or (b) the value differs from the current
    // one. Thread-safe; callbacks run on the caller's thread after the lock is
    // released.
    void setValue(Value v, Timestamp ts = std::chrono::system_clock::now());
    void setState(DpState s);

    // Disconnect policy. When the source transport drops, the poll layer calls
    // markDisconnected() instead of a bare setState(Error). If a disconnect
    // value was set, the datapoint is reset to it (e.g. zeroed) with state Error
    // in one atomic step (single notification); otherwise the last value is held
    // and only the state goes Error. Default (unset) preserves hold semantics.
    void setDisconnectValue(std::optional<Value> v);
    void markDisconnected();

    // Register a writer invoked by `write()`. The Core wires this to a
    // SinkWindow stage operation; tests can supply a lambda directly.
    void setWriter(Writer w);

    // For Command / Bidirectional datapoints — invoked from QML (via the Qt
    // wrapper) or business code. Forwards to the registered writer; no-op if
    // none.
    void write(Value v);

    // Qt-free change notification, replacing the old Q_PROPERTY NOTIFY signals.
    // Each slot holds a single listener (Core's publisher / a test); QML
    // observes value changes through the EventBus DpChanged stream instead.
    void setOnValueChanged(ChangeCallback cb);
    void setOnStateChanged(ChangeCallback cb);

private:
    class Impl;
    Impl* m_impl;
};

} // namespace core::dp
