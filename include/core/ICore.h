#pragma once

#include <expected>
#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/config/ValidationError.h"

class QQmlContext;

namespace core::bus      { class EventBus; }
namespace core::dp       { class DatapointRegistry; }
namespace core::codec    { class CodecRegistry; }
namespace core::module   { class ModuleRegistry; }
namespace core::plugin   { class PluginRegistry; }
namespace core::transport{ class Transport; }

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

    virtual transport::Transport*     transport(QString const& id) const = 0;

    virtual void start() = 0;
    virtual void stop()  = 0;
};

} // namespace core
