#include "core/module/ModuleRegistry.h"

#include <map>
#include <utility>
#include <vector>

#include <QObject>
#include <QThread>
#include <QTimer>

#include "core/module/FunctionalModule.h"

namespace core::module {

// TickDriver — drives each auto-ticking module's driveTick() on a dedicated
// worker thread, grouped by transport.
//
// driveTick() is non-blocking ONLY when the transport implements a true async
// path: ModbusTcpClient overrides readAsync/writeAsync to post the request to
// its client thread and return at once. The other transports (Modbus RTU,
// OPC UA, MQTT, S7) still fall back to the base Transport::readAsync default,
// which runs the SYNCHRONOUS read/writeBatch on the caller — i.e. it blocks.
//
// Because a poll/sink/heartbeat module can be wired to any of them, we keep one
// tick thread per transport: a blocking driveTick() (and any slow/unreachable
// PLC) stalls only that thread, never the GUI thread, and one transport's
// stall does not affect another's ticks. The modules are plain (non-QObject)
// and internally synchronized, so calling driveTick() from a worker thread is
// safe. (Once every transport has a non-blocking async path, ticks could move
// back to the GUI thread and these threads be dropped.)
class ModuleRegistry::TickDriver {
public:
    TickDriver() = default;
    ~TickDriver() { stopAll(); }

    void start(FunctionalModule* mod) {
        if (!mod) return;
        int const period = mod->tickPeriodMs();
        if (period <= 0) return;

        QString key = mod->transportId();
        if (key.isEmpty()) key = QStringLiteral("<none>");
        QObject* anchor = ensureLane(key);

        // Create + start the timer on the lane's worker thread so its timeout —
        // and the blocking driveTick() it triggers — run off the caller thread.
        QMetaObject::invokeMethod(anchor, [anchor, mod, period]() {
            auto* t = new QTimer(anchor);              // child → lives on this thread
            t->setInterval(period);
            t->setSingleShot(false);
            QObject::connect(t, &QTimer::timeout, t, [mod]() { mod->driveTick(); });
            t->start();
        }, Qt::BlockingQueuedConnection);
    }

    void stopAll() {
        for (auto& [_, lane] : m_lanes) {
            if (lane.anchor) {
                QObject* a = lane.anchor;
                // Delete the anchor (and its child timers) on its own thread —
                // destroying a QObject from a foreign thread aborts the process.
                QMetaObject::invokeMethod(a, [a]() { delete a; },
                                          Qt::BlockingQueuedConnection);
            }
            if (lane.thread) {
                lane.thread->quit();
                lane.thread->wait();
                delete lane.thread;
            }
        }
        m_lanes.clear();
    }

    void pauseAll()  { forEachLaneTimers([](QTimer* t) { t->stop(); }); }
    void resumeAll() { forEachLaneTimers([](QTimer* t) { if (!t->isActive()) t->start(); }); }

private:
    struct Lane {
        QThread* thread = nullptr;
        QObject* anchor = nullptr;   // parent of the lane's timers; lives on `thread`
    };

    QObject* ensureLane(QString const& key) {
        auto it = m_lanes.find(key);
        if (it != m_lanes.end()) return it->second.anchor;
        Lane lane;
        lane.thread = new QThread;
        lane.anchor = new QObject;          // created here, then handed to the thread
        lane.anchor->moveToThread(lane.thread);
        lane.thread->start();
        auto [ins, ok] = m_lanes.emplace(key, lane);
        return ins->second.anchor;
    }

    template <class F>
    void forEachLaneTimers(F fn) {
        for (auto& [_, lane] : m_lanes) {
            QObject* a = lane.anchor;
            if (!a) continue;
            QMetaObject::invokeMethod(a, [a, fn]() {
                for (auto* t : a->findChildren<QTimer*>()) fn(t);
            }, Qt::BlockingQueuedConnection);
        }
    }

    std::map<QString, Lane> m_lanes;   // by transportId
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
    QString const id = mod->id();
    m_modules.insert_or_assign(id, std::move(mod));
    return true;
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
