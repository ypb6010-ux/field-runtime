// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/module/ModuleRegistry.h"

#include <utility>
#include <vector>

#include <QObject>
#include <QTimer>

#include "core/module/FunctionalModule.h"

namespace core::module {

// TickDriver — drives each auto-ticking module's driveTick() from timers on the
// thread that owns the Core lifecycle (the GUI / main event loop).
//
// This is safe because every transport now exposes a NON-BLOCKING async path:
// driveTick() submits its read/write through the scheduler, whose AsyncWork
// posts the request to the transport's own client thread (Modbus TCP/RTU via
// QModbusReply, OPC UA via a node chain, MQTT via QueuedConnection publish) and
// returns at once; the reply's completion fires later on that client thread.
// So a tick never parks the GUI thread, and the per-transport worker tick
// threads this class used to spin up are gone — the minimal event-driven form.
//
// INVARIANT: a module may only be wired to a transport whose readAsync/writeAsync
// are non-blocking. The S7 stub and the (build-disabled) Paho stub return
// instantly; if Paho is ever enabled, give it a real async path (or a dedicated
// worker tick) before driving its modules from here, or it will freeze the GUI.
//
// All methods run on the lifecycle thread (Core::start/stop/pause/resume), so no
// cross-thread hops are needed; the timers and their anchor share that thread.
class ModuleRegistry::TickDriver {
public:
    TickDriver() = default;
    ~TickDriver() { stopAll(); }

    void start(FunctionalModule* mod) {
        if (!mod) return;
        int const period = mod->tickPeriodMs();
        if (period <= 0) return;
        if (!m_anchor) m_anchor = new QObject;     // owns the timers, this thread

        auto* t = new QTimer(m_anchor);
        t->setInterval(period);
        t->setSingleShot(false);
        QObject::connect(t, &QTimer::timeout, t, [mod]() { mod->driveTick(); });
        t->start();
    }

    void stopAll() {
        delete m_anchor;        // deletes child timers; same thread that created them
        m_anchor = nullptr;
    }

    void pauseAll()  { forEachTimer([](QTimer* t) { t->stop(); }); }
    void resumeAll() { forEachTimer([](QTimer* t) { if (!t->isActive()) t->start(); }); }

private:
    template <class F>
    void forEachTimer(F fn) {
        if (!m_anchor) return;
        for (auto* t : m_anchor->findChildren<QTimer*>()) fn(t);
    }

    QObject* m_anchor = nullptr;   // parent of all tick timers; lifecycle thread
};

ModuleRegistry::ModuleRegistry()
    : m_tickDriver(std::make_unique<TickDriver>()) {}

ModuleRegistry::~ModuleRegistry() = default;

bool ModuleRegistry::registerModule(std::unique_ptr<FunctionalModule> mod) {
    if (!mod) return false;
    // Reject registration while ticking: an active tick timer captures a raw
    // FunctionalModule*, so replacing/destroying a module now would dangle it.
    // Modules must be wired before startAll() (config reload = stopAll first).
    if (m_started) return false;
    std::string const id = mod->id();
    m_modules.insert_or_assign(id, std::move(mod));
    return true;
}

FunctionalModule* ModuleRegistry::find(std::string const& moduleId) const {
    auto it = m_modules.find(moduleId);
    return it == m_modules.end() ? nullptr : it->second.get();
}

std::vector<FunctionalModule*>
ModuleRegistry::byTransport(std::string const& transportId) const {
    std::vector<FunctionalModule*> out;
    for (auto const& [_, mod] : m_modules) {
        if (mod->transportId() == transportId) out.push_back(mod.get());
    }
    return out;
}

std::vector<FunctionalModule*> ModuleRegistry::all() const {
    std::vector<FunctionalModule*> out;
    out.reserve(m_modules.size());
    for (auto const& [_, mod] : m_modules) out.push_back(mod.get());
    return out;
}

void ModuleRegistry::startAll() {
    m_started = true;
    for (auto& [_, m] : m_modules) m->start();
    if (m_autoTick) {
        for (auto& [_, m] : m_modules) m_tickDriver->start(m.get());
    }
}

void ModuleRegistry::stopAll() {
    if (m_tickDriver) m_tickDriver->stopAll();
    for (auto& [_, m] : m_modules) m->stop();
    m_started = false;
}

void ModuleRegistry::pauseAll() {
    if (m_tickDriver) m_tickDriver->pauseAll();
    for (auto& [_, m] : m_modules) m->pause();
}

void ModuleRegistry::resumeAll() {
    for (auto& [_, m] : m_modules) m->resume();
    if (m_autoTick && m_tickDriver) m_tickDriver->resumeAll();
}

void ModuleRegistry::setAutoTickEnabled(bool on) {
    m_autoTick = on;
}

} // namespace core::module
