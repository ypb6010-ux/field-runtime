// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
// PluginRegistry + PortRegistry — the new-core DLL plugin system. A plugin is a
// shared library exporting `corePluginCreate` (see CORE_PLUGIN_ENTRY); it binds
// typed In/OutPorts to named datapoints in Plugin::registerPorts.

#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"
#include "core/plugin/Plugin.h"

#include <map>
#include <vector>

#include <QLibrary>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/bus/Subscription.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/log/Logger.h"

namespace core::plugin {

// ── PortRegistry ───────────────────────────────────────────────────────────
class PortRegistry::Impl {
public:
    Impl(dp::DatapointRegistry& d, bus::EventBus& b) : dps(d), bus(b) {
        // One DpChanged subscription fans out to every bound InPort by id.
        sub = bus.subscribe<bus::DpChanged>([this](bus::DpChanged const& e) {
            auto range = handlers.equal_range(e.id);
            for (auto it = range.first; it != range.second; ++it) it->second(e.value);
        });
    }

    dp::DatapointRegistry& dps;
    bus::EventBus&         bus;
    std::multimap<QString, std::function<void(QVariant const&)>> handlers;
    bus::Subscription      sub;
};

PortRegistry::PortRegistry(dp::DatapointRegistry& dps, bus::EventBus& bus)
    : m_impl(new Impl(dps, bus)) {}

PortRegistry::~PortRegistry() { delete m_impl; }

void PortRegistry::bindInErased(QString const& dpId,
                                std::function<void(QVariant const&)> deliver) {
    m_impl->handlers.emplace(dpId, std::move(deliver));
}

void PortRegistry::writeErased(QString const& dpId, QVariant const& value) {
    if (auto dp = m_impl->dps.find(dpId)) dp->write(value);
}

// ── PluginRegistry ─────────────────────────────────────────────────────────
class PluginRegistry::Impl {
public:
    struct Loaded {
        QLibrary* lib    = nullptr;
        Plugin*   plugin = nullptr;
    };
    std::vector<Loaded> loaded;
};

PluginRegistry::PluginRegistry()  : m_impl(std::make_unique<Impl>()) {}
PluginRegistry::~PluginRegistry() { unloadAll(); }

bool PluginRegistry::load(QString const& dllPath) {
    using CreateFn = Plugin* (*)();
    auto* lib = new QLibrary(dllPath);
    if (!lib->load()) { delete lib; return false; }
    auto create = reinterpret_cast<CreateFn>(lib->resolve("corePluginCreate"));
    if (!create) { lib->unload(); delete lib; return false; }
    Plugin* p = create();
    if (!p) { lib->unload(); delete lib; return false; }
    m_impl->loaded.push_back({lib, p});
    return true;
}

void PluginRegistry::unloadAll() {
    // Destroy plugin instances (and their port emitters) before unloading the
    // libraries so no live lambda outlives its code.
    for (auto it = m_impl->loaded.rbegin(); it != m_impl->loaded.rend(); ++it) {
        delete it->plugin;
        if (it->lib) { it->lib->unload(); delete it->lib; }
    }
    m_impl->loaded.clear();
}

void PluginRegistry::registerAllPorts(PortRegistry& reg) {
    for (auto& l : m_impl->loaded) l.plugin->registerPorts(reg);
}

QList<Plugin*> PluginRegistry::all() const {
    QList<Plugin*> out;
    out.reserve(int(m_impl->loaded.size()));
    for (auto& l : m_impl->loaded) out.append(l.plugin);
    return out;
}

} // namespace core::plugin
