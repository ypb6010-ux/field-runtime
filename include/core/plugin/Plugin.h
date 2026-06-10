// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
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

// DLL entry contract. A plugin shared library exports a single C symbol
// `corePluginCreate` returning a heap Plugin* that Core owns (deleted on
// unloadAll). Use this macro in exactly one TU of the plugin:
//
//     CORE_PLUGIN_ENTRY(MyPlugin)
//
#define CORE_PLUGIN_ENTRY(ClassName)                                   \
    extern "C" Q_DECL_EXPORT core::plugin::Plugin* corePluginCreate() { \
        return new ClassName();                                        \
    }
