// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <expected>
#include <memory>
#include <QString>
#include <QStringList>

#include "core/core_global.h"
#include "core/config/ValidationError.h"
#include "core/transport/TransportTypes.h"

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
    // With QML support enabled, a non-null context receives `dp`
    // (DatapointQmlBridge) and `log` (LogBridge) context properties.
    // installDefaultConsole=false lets the app own console output entirely
    // (e.g. wrap a ConsoleSink in its own LogFilter, or omit it on a field box).
    static std::unique_ptr<ICore> create(QQmlContext* qmlContext = nullptr,
                                         bool installDefaultConsole = true);

    virtual ~ICore() = default;

    // Initial configuration: call exactly once before start(). A second load
    // or a load while running is rejected instead of partially mixing two
    // object graphs; use reloadConfig() for an explicit replacement.
    virtual std::expected<void, config::ValidationErrors>
            loadConfig(QString const& tomlPath) = 0;

    // Explicit transactional reload. Parsing/validation failures leave the
    // active graph untouched; runtime construction failures roll back to the
    // previous validated schema. File watching belongs in the application.
    virtual std::expected<void, config::ValidationErrors>
            reloadConfig(QString const& tomlPath) = 0;

    virtual bus::EventBus&            bus()        = 0;
    virtual dp::DatapointRegistry&    datapoints() = 0;
    virtual codec::CodecRegistry&     codecs()     = 0;
    virtual module::ModuleRegistry&   modules()    = 0;
    virtual plugin::PluginRegistry&   plugins()    = 0;
    virtual log::Logger&              logger()     = 0;

    // Live-object access for orchestration. Returned pointers are invalidated
    // by a successful reload; UI/diagnostics should use the value snapshots.
    virtual transport::Transport*     transport(QString const& id) const = 0;
    virtual QStringList               transportIds() const = 0;
    virtual transport::TransportStatus transportStatus(QString const& id) const = 0;
    virtual QList<transport::TransportStatus> transportStatuses() const = 0;
    virtual QList<transport::PeerSession> peerSessions(QString const& id) const = 0;

    virtual void start() = 0;
    virtual void stop()  = 0;

    // 操作箱(modbus server)→ PLC 的写转发开关,按 server 透传 id 控制,运行时可调
    // (默认开启)。关闭(true→false)时立即把该 server 对应桥接的转发区在程序内置 0,
    // 取消尚未写入 PLC 的指令;PLC→操作箱的回显镜像不受影响。由业务逻辑(如按模式位)调用。
    virtual void setServerForwardEnabled(QString const& serverTransportId, bool enabled) = 0;
    virtual bool serverForwardEnabled(QString const& serverTransportId) const = 0;
};

} // namespace core
