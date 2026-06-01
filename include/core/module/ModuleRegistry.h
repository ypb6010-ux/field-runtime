#pragma once

#include <map>
#include <memory>
#include <QList>
#include <QString>

#include "core/core_global.h"
#include "core/module/FunctionalModule.h"

namespace core::module {

class CORE_EXPORT ModuleRegistry {
public:
    ModuleRegistry();
    ~ModuleRegistry();

    CORE_DISABLE_COPY_MOVE(ModuleRegistry)

    void registerModule(std::unique_ptr<FunctionalModule> mod);
    FunctionalModule*           find(QString const& moduleId) const;
    QList<FunctionalModule*>    byTransport(QString const& transportId) const;
    QList<FunctionalModule*>    all() const;

    // start*All() also arms / disarms a QTimer per module whose
    // tickPeriodMs() > 0, so PollRange / SinkWindow / Heartbeat run
    // automatically once startAll() returns.
    void startAll();
    void stopAll();
    void pauseAll();
    void resumeAll();

    // Test hook — disable the QTimer driver so the unit tests can drive
    // module ticks explicitly through pollOnce()/onTick().
    void setAutoTickEnabled(bool on);

private:
    class TickDriver;
    std::map<QString, std::unique_ptr<FunctionalModule>> m_modules;
    std::unique_ptr<TickDriver> m_tickDriver;
    bool m_autoTick = true;
};

} // namespace core::module
