#pragma once

#include <QString>

#include "core/core_global.h"

namespace core::plugin {

class PortRegistry;

class CORE_EXPORT Plugin {
public:
    virtual ~Plugin() = default;

    virtual QString name() const = 0;

    // Called once during Core startup. Plugins bind their In/OutPorts to
    // named datapoints via the registry.
    virtual void registerPorts(PortRegistry& reg) = 0;

    // Called after all plugins have registered, after Core is ready.
    virtual void onInitialized() {}
};

} // namespace core::plugin
