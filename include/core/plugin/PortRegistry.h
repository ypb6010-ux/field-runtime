#pragma once

#include <QString>

#include "core/core_global.h"
#include "core/plugin/InPort.h"
#include "core/plugin/OutPort.h"

namespace core::dp { class DatapointRegistry; }
namespace core::bus { class EventBus; }

namespace core::plugin {

class CORE_EXPORT PortRegistry {
public:
    PortRegistry(dp::DatapointRegistry& dps, bus::EventBus& bus);
    ~PortRegistry();

    CORE_DISABLE_COPY_MOVE(PortRegistry)

    template <class T>
    void bindIn(InPort<T>& port, QString const& dpId);

    template <class T>
    void bindOut(OutPort<T>& port, QString const& dpId);

private:
    class Impl;
    Impl* m_impl;
};

} // namespace core::plugin
