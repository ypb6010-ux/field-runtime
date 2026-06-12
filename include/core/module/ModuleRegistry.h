// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/core_global.h"
#include "core/module/FunctionalModule.h"

namespace core::module {

class CORE_EXPORT ModuleRegistry {
public:
    ModuleRegistry();
    ~ModuleRegistry();

    CORE_DISABLE_COPY_MOVE(ModuleRegistry)

    // Register a module. MUST be called before startAll(): once ticking is
    // live, a tick timer holds a raw FunctionalModule*, so replacing/destroying
    // a module would leave that timer calling a dangling pointer. Returns false
    // (and ignores the module) if called after startAll() without an
    // intervening stopAll().
    bool registerModule(std::unique_ptr<FunctionalModule> mod);
    FunctionalModule*           find(std::string const& moduleId) const;
    std::vector<FunctionalModule*> byTransport(std::string const& transportId) const;
    std::vector<FunctionalModule*> all() const;

    // start*All() also arms / disarms a timer per module whose
    // tickPeriodMs() > 0, so PollRange / SinkWindow / Heartbeat run
    // automatically once startAll() returns.
    void startAll();
    void stopAll();
    void pauseAll();
    void resumeAll();

    // Test hook — disable the timer driver so the unit tests can drive
    // module ticks explicitly through pollOnce()/onTick().
    void setAutoTickEnabled(bool on);

private:
    class TickDriver;
    std::map<std::string, std::unique_ptr<FunctionalModule>> m_modules;
    std::unique_ptr<TickDriver> m_tickDriver;
    bool m_autoTick = true;
    bool m_started  = false;   // true between startAll() and stopAll()
};

} // namespace core::module
