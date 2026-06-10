// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <expected>
#include <memory>
#include <QString>
#include <QStringList>

#include "core/core_global.h"
#include "core/config/ValidationError.h"

class QQmlContext;

namespace core::bus      { class EventBus; }
namespace core::dp       { class DatapointRegistry; }
namespace core::codec    { class CodecRegistry; }
namespace core::module   { class ModuleRegistry; }
namespace core::plugin   { class PluginRegistry; }
namespace core::transport{ class Transport; }
namespace core::log      { class Logger; }

namespace core {

// Top-level facade. One instance per application; owns all subsystems.
class CORE_EXPORT ICore {
public:
    static std::unique_ptr<ICore> create(QQmlContext* qmlContext = nullptr);

    virtual ~ICore() = default;

    virtual std::expected<void, config::ValidationErrors>
            loadConfig(QString const& tomlPath) = 0;

    virtual bus::EventBus&            bus()        = 0;
    virtual dp::DatapointRegistry&    datapoints() = 0;
    virtual codec::CodecRegistry&     codecs()     = 0;
    virtual module::ModuleRegistry&   modules()    = 0;
    virtual plugin::PluginRegistry&   plugins()    = 0;
    virtual log::Logger&              logger()     = 0;

    virtual transport::Transport*     transport(QString const& id) const = 0;
    virtual QStringList               transportIds() const = 0;

    virtual void start() = 0;
    virtual void stop()  = 0;
};

} // namespace core
