// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QList>
#include <QString>

#include "core/core_global.h"

namespace core::plugin {

class Plugin;
class PortRegistry;

class CORE_EXPORT PluginRegistry {
public:
    PluginRegistry();
    ~PluginRegistry();

    CORE_DISABLE_COPY_MOVE(PluginRegistry)

    bool             load(QString const& dllPath);
    void             unloadAll();
    void             registerAllPorts(PortRegistry& reg);
    QList<Plugin*>   all() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::plugin
