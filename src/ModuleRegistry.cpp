#include "core/module/ModuleRegistry.h"

#include <utility>

#include "core/module/FunctionalModule.h"

namespace core::module {

ModuleRegistry::ModuleRegistry()  = default;
ModuleRegistry::~ModuleRegistry() = default;

void ModuleRegistry::registerModule(std::unique_ptr<FunctionalModule> mod) {
    if (!mod) return;
    QString const id = mod->id();
    m_modules.insert_or_assign(id, std::move(mod));
}

FunctionalModule* ModuleRegistry::find(QString const& moduleId) const {
    auto it = m_modules.find(moduleId);
    return it == m_modules.end() ? nullptr : it->second.get();
}

QList<FunctionalModule*>
ModuleRegistry::byTransport(QString const& transportId) const {
    QList<FunctionalModule*> out;
    for (auto const& [_, mod] : m_modules) {
        if (mod->transportId() == transportId) out.append(mod.get());
    }
    return out;
}

QList<FunctionalModule*> ModuleRegistry::all() const {
    QList<FunctionalModule*> out;
    out.reserve(int(m_modules.size()));
    for (auto const& [_, mod] : m_modules) out.append(mod.get());
    return out;
}

void ModuleRegistry::startAll()  { for (auto& [_, m] : m_modules) m->start(); }
void ModuleRegistry::stopAll()   { for (auto& [_, m] : m_modules) m->stop(); }
void ModuleRegistry::pauseAll()  { for (auto& [_, m] : m_modules) m->pause(); }
void ModuleRegistry::resumeAll() { for (auto& [_, m] : m_modules) m->resume(); }

} // namespace core::module
