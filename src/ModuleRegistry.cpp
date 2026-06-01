#include "core/module/ModuleRegistry.h"

#include <utility>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QTimer>

#include "core/module/FunctionalModule.h"

namespace core::module {

// TickDriver — owns a QTimer per module with tickPeriodMs > 0. Lives on the
// thread that called ModuleRegistry::startAll(); QTimer fires on that
// thread's event loop. driveTick() implementations are non-blocking on the
// timer thread because all blocking I/O is funneled through transport
// worker threads via scheduler().submit(...).
class ModuleRegistry::TickDriver : public QObject {
public:
    TickDriver() = default;
    ~TickDriver() override { stopAll(); }

    void start(FunctionalModule* mod) {
        if (!mod) return;
        int const period = mod->tickPeriodMs();
        if (period <= 0) return;
        if (m_timers.count(mod) != 0) return;
        auto* t = new QTimer(this);
        t->setInterval(period);
        t->setSingleShot(false);
        QPointer<TickDriver> self(this);
        QObject::connect(t, &QTimer::timeout, this, [self, mod]() {
            if (!self) return;
            mod->driveTick();
        });
        t->start();
        m_timers.emplace(mod, t);
    }

    void stop(FunctionalModule* mod) {
        auto it = m_timers.find(mod);
        if (it == m_timers.end()) return;
        it->second->stop();
        it->second->deleteLater();
        m_timers.erase(it);
    }

    void stopAll() {
        for (auto& [_, t] : m_timers) {
            t->stop();
            t->deleteLater();
        }
        m_timers.clear();
    }

    void pauseAll() {
        for (auto& [_, t] : m_timers) t->stop();
    }

    void resumeAll() {
        for (auto& [mod, t] : m_timers) {
            int const p = mod->tickPeriodMs();
            if (p > 0) {
                t->setInterval(p);
                t->start();
            }
        }
    }

private:
    std::map<FunctionalModule*, QTimer*> m_timers;
};

ModuleRegistry::ModuleRegistry()
    : m_tickDriver(std::make_unique<TickDriver>()) {}

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

void ModuleRegistry::startAll() {
    for (auto& [_, m] : m_modules) m->start();
    if (m_autoTick) {
        for (auto& [_, m] : m_modules) m_tickDriver->start(m.get());
    }
}

void ModuleRegistry::stopAll() {
    if (m_tickDriver) m_tickDriver->stopAll();
    for (auto& [_, m] : m_modules) m->stop();
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
