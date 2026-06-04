#pragma once

#include <functional>
#include <QString>
#include <QVariant>

#include "core/core_global.h"
#include "core/plugin/InPort.h"
#include "core/plugin/OutPort.h"

namespace core::dp { class DatapointRegistry; }
namespace core::bus { class EventBus; }

namespace core::plugin {

// PortRegistry — wires a plugin's typed In/OutPorts to named datapoints.
//   bindIn : datapoint value changes (DpChanged) → InPort<T>::deliver
//   bindOut: OutPort<T>::send(v) → Datapoint::write
// The templates live in the header (plugins instantiate them for their own T)
// and forward to the exported, type-erased helpers so the Impl stays in the .cpp.
class CORE_EXPORT PortRegistry {
public:
    PortRegistry(dp::DatapointRegistry& dps, bus::EventBus& bus);
    ~PortRegistry();

    CORE_DISABLE_COPY_MOVE(PortRegistry)

    template <class T>
    void bindIn(InPort<T>& port, QString const& dpId) {
        bindInErased(dpId, [&port](QVariant const& v) {
            port.deliver(v.value<T>());
        });
    }

    template <class T>
    void bindOut(OutPort<T>& port, QString const& dpId) {
        port.bindEmitter([this, dpId](T const& v) {
            writeErased(dpId, QVariant::fromValue(v));
        });
    }

private:
    void bindInErased(QString const& dpId,
                      std::function<void(QVariant const&)> deliver);
    void writeErased(QString const& dpId, QVariant const& value);

    class Impl;
    Impl* m_impl;
};

} // namespace core::plugin
