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

    void startAll();
    void stopAll();
    void pauseAll();
    void resumeAll();

private:
    std::map<QString, std::unique_ptr<FunctionalModule>> m_modules;
};

} // namespace core::module
