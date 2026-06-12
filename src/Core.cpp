// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/ICore.h"
#include "core/internal/Testing.h"

#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <QHash>
#include <QQmlContext>
#include <QTimer>

#include "core/bus/EventBus.h"
#include "core/bus/BusEvents.h"
#include "core/bus/Subscription.h"
#include <QDir>
#include <QFileInfo>

#include "core/codec/BuiltinCodecs.h"
#include "core/codec/CodecRegistry.h"
#include "core/codec/LuaCodec.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/dp/PortRef.h"
#include "core/dp/ValueQt.h"
#include "core/log/Logger.h"
#include "core/log/Sinks.h"
#include "core/qml/LogBridge.h"
#include "core/module/AckWatch.h"
#include "core/module/Command.h"
#include "core/module/Heartbeat.h"
#include "core/module/ModuleRegistry.h"
#include "core/module/PollRange.h"
#include "core/module/SinkWindow.h"
#include "core/plugin/Plugin.h"
#include "core/plugin/PluginRegistry.h"
#include "core/plugin/PortRegistry.h"
#include "core/transport/ModbusRtuTransport.h"
#include "core/transport/ModbusTcpClientTransport.h"
#include "core/transport/ModbusTcpServerTransport.h"
#include "core/transport/MqttClientTransport.h"
#include "core/transport/MqttPahoTransport.h"
#include "core/transport/OpcUaClientTransport.h"
#include "core/transport/S7ClientTransport.h"
#include "core/transport/Transport.h"

namespace core {

namespace {

// Marshal the Qt-free config IR strings into the QString-based runtime types the
// Qt HMI assembly still uses. The config schema (core::config) is Qt-free; this
// is the adapter edge.
inline QString qs(std::string const& s) { return QString::fromStdString(s); }

core::RegisterTable tableFromString(QString const& s) {
    static const QHash<QString, core::RegisterTable> map = {
        {"HR",                core::RegisterTable::HoldingRegister},
        {"HoldingRegisters",  core::RegisterTable::HoldingRegister},
        {"IR",                core::RegisterTable::InputRegister},
        {"InputRegisters",    core::RegisterTable::InputRegister},
        {"Coil",              core::RegisterTable::Coil},
        {"Coils",             core::RegisterTable::Coil},
        {"DI",                core::RegisterTable::DiscreteInput},
        {"DiscreteInputs",    core::RegisterTable::DiscreteInput},
    };
    return map.value(s, core::RegisterTable::HoldingRegister);
}

dp::WordOrder wordOrderFromString(QString const& s) {
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    return dp::WordOrder::ABCD;
}

dp::Kind kindFromString(std::string const& s) {
    if (s == "Command")       return dp::Kind::Command;
    if (s == "Bidirectional") return dp::Kind::Bidirectional;
    return dp::Kind::Status;
}

dp::PortRef makePortRef(config::PortRefConfig const& pc,
                         std::shared_ptr<codec::Codec> codec) {
    dp::PortRef p;
    p.transport = qs(pc.port);
    p.table     = tableFromString(qs(pc.table));
    p.address   = pc.address;
    if (pc.bit >= 0) p.bit = pc.bit;
    p.wordOrder = wordOrderFromString(qs(pc.wordOrder));
    p.shift     = pc.shift;
    p.mask      = pc.mask;
    p.scale     = pc.scale;
    p.offset    = pc.offset;
    p.codec     = std::move(codec);
    p.window    = qs(pc.window);
    return p;
}

} // namespace

class CoreImpl : public ICore {
public:
    CoreImpl(QQmlContext* qml, bool installDefaultConsole)
        : m_qml(qml)
        , m_logger(std::make_unique<log::Logger>())
        , m_bus(std::make_unique<bus::EventBus>())
        , m_codecs(std::make_unique<codec::CodecRegistry>())
        , m_dps(std::make_unique<dp::DatapointRegistry>())
        , m_modules(std::make_unique<module::ModuleRegistry>())
        , m_plugins(std::make_unique<plugin::PluginRegistry>()) {
        m_codecs->loadBuiltins();
        // Built-in console sink so diagnostics surface out of the box; the app
        // adds file / DB sinks via logger().addSink(...). Apps that want to
        // filter or suppress console output pass installDefaultConsole=false
        // and register their own.
        if (installDefaultConsole) {
            m_logger->addSink(std::make_shared<log::ConsoleSink>());
        }
        installLogBridge();
    }

    ~CoreImpl() override {
        // Async-safe teardown order:
        //   1. drop event-bus subscriptions (no more routing into modules);
        //   2. stop the module tick driver so no NEW driveTick() is issued
        //      (modules stay alive);
        //   3. destroy the transports — each stops its scheduler's async pump
        //      (stopAsync) then joins its worker thread, SAFELY ABANDONING any
        //      in-flight completion (a pending reply may be cancelled, not
        //      delivered). Whatever completion does still run invokes module
        //      callbacks (apply result / clear in-flight), so the modules MUST
        //      still be alive here — hence transports before modules. The
        //      modules' raw Transport* go dangling but are never dereferenced
        //      after the tick driver stopped;
        //   4. now destroy the modules;
        //   5. datapoints / codecs / bus, then the logger last.
        stopMirrorPump();                      // no bridge mirror fires during teardown
        m_bridgeMirrors.clear();               // drop datapoint refs before the registry dies
        m_bridgeFwdSinks.clear();              // raw ptrs owned by m_modules
        m_bridges.clear();
        m_transportEventSub.reset();
        m_serverWriteSub.reset();
        joinConnectThreads();                  // no connect() in flight on a dying transport
        if (m_modules) m_modules->stopAll();   // stop ticks; keep modules alive
        m_transports.clear();                  // join worker threads → drain in-flight
        m_pollRangePtrs.clear();
        m_sinkWindowPtrs.clear();
        m_modules.reset();                     // safe: no completion can fire now
        m_plugins.reset();                     // destroy plugins (and their port emitters) first
        m_ports.reset();                       // then the port registry (drops its bus sub)
        m_dps.reset();
        m_codecs.reset();
        m_bus.reset();
        // Logger last — subsystems above may log during teardown.
        m_logBridge.reset();
        if (m_logger) m_logger->stop();
        m_logger.reset();
    }

    std::expected<void, config::ValidationErrors>
    loadConfig(QString const& path) override {
        config::ConfigLoader loader;
        auto schema = loader.loadFromToml(path.toStdString());
        if (!schema.has_value()) {
            for (auto const& err : schema.error()) {
                m_logger->logf(log::LogLevel::Error,
                               QStringLiteral("config"), path,
                               qs(err.message));
            }
            return std::unexpected(schema.error());
        }

        if (!schema->meta.logLevel.empty()) {
            m_logger->setThreshold(log::levelFromString(schema->meta.logLevel));
        }
        // Resolve config-relative paths (e.g. lua codec scripts) against the
        // config file's directory.
        m_configDir = QFileInfo(path).absolutePath();
        wireFromSchema(*schema);
        m_logger->logf(log::LogLevel::Info, QStringLiteral("config"), path,
                       QStringLiteral("loaded"),
                       {{QStringLiteral("transports"),
                         int(schema->transports.size())},
                        {QStringLiteral("datapoints"),
                         int(schema->datapoints.size())}});
        return {};
    }

    bus::EventBus&            bus()        override { return *m_bus; }
    dp::DatapointRegistry&    datapoints() override { return *m_dps; }
    codec::CodecRegistry&     codecs()     override { return *m_codecs; }
    module::ModuleRegistry&   modules()    override { return *m_modules; }
    plugin::PluginRegistry&   plugins()    override { return *m_plugins; }
    log::Logger&              logger()     override { return *m_logger; }

    transport::Transport* transport(QString const& id) const override {
        auto it = m_transports.find(id);
        return it == m_transports.end() ? nullptr : it->second.get();
    }

    QStringList transportIds() const override {
        QStringList ids;
        ids.reserve(int(m_transports.size()));
        for (auto const& [id, t] : m_transports) ids << id;
        return ids;
    }

    void setServerForwardEnabled(QString const& serverTransportId, bool enabled) override {
        bool wasEnabled;
        {
            std::lock_guard lk(m_forwardMtx);
            wasEnabled = m_forwardEnabled.value(serverTransportId, true);
            m_forwardEnabled[serverTransportId] = enabled;
        }
        // 关闭瞬间(true→false):把该 server 桥接的转发区在程序内置 0,取消尚未写入
        // PLC 的操作箱指令(中性/停机)。只在边沿触发,避免每次调用重复 stage。
        if (wasEnabled && !enabled) {
            zeroBridgeForward(serverTransportId);
        }
    }

    bool serverForwardEnabled(QString const& serverTransportId) const override {
        std::lock_guard lk(m_forwardMtx);
        return m_forwardEnabled.value(serverTransportId, true);
    }

    void start() override {
        if (m_started) return;   // one-shot: a second start() must not spawn a
        m_started = true;        // second concurrent connect() per transport
        installEventWiring();
        // Connect transports in PARALLEL, off the calling (GUI) thread: a slow
        // or unreachable transport (e.g. a wrong MQTT / OPC UA endpoint) must
        // not stall start() for its whole connect timeout. The threads are
        // joined at stop()/teardown. Modules start polling immediately; reads
        // before a connection completes simply report "not connected".
        for (auto& [id, t] : m_transports) {
            transport::Transport* tp = t.get();
            m_connectThreads.emplace_back([tp]() { (void)tp->connect(); });
        }
        m_modules->startAll();
        startStatsPump();
        startMirrorPump();
        m_bus->publish(bus::CoreReady{});
        m_logger->logf(log::LogLevel::Info, QStringLiteral("core"),
                       QStringLiteral("ICore"), QStringLiteral("started"));
    }

    void stop() override {
        if (!m_started) return;
        m_started = false;
        m_logger->logf(log::LogLevel::Info, QStringLiteral("core"),
                       QStringLiteral("ICore"), QStringLiteral("stopping"));
        joinConnectThreads();   // no connect in flight before we disconnect
        m_bus->publish(bus::CoreStopping{});
        stopStatsPump();
        stopMirrorPump();
        m_modules->stopAll();
        for (auto& [id, t] : m_transports) t->disconnect();
        m_logger->flush();
    }

    void joinConnectThreads() {
        for (auto& th : m_connectThreads) {
            if (th.joinable()) th.join();
        }
        m_connectThreads.clear();
    }

    void pollAllOnce() {
        for (auto* poll : m_pollRangePtrs) poll->pollOnce();
    }

    void tickSinkWindowsOnce() {
        for (auto* sw : m_sinkWindowPtrs) sw->onTick();
    }

    void publishSchedulerStatsOnce() {
        for (auto& [id, t] : m_transports) {
            m_bus->publish(bus::SchedulerStatsEvent{id.toStdString(), t->scheduler().stats()});
        }
    }

    void setSchedulerStatsIntervalMs(int ms) { m_statsIntervalMs = ms; }

private:
    void wireFromSchema(config::ConfigSchema const& schema) {
        registerCustomCodecs(schema);
        buildTransports(schema);
        auto byId = buildDatapoints(schema);
        buildPollRanges(schema, byId);
        buildSinkWindows(schema);
        buildHeartbeats(schema);
        buildAckWatches(schema);
        buildCommands(schema);
        m_routes = schema.routes;
        buildBridges(schema);
        loadPlugins(schema);
    }

    // Load each [[plugin]] DLL, let it bind In/OutPorts to datapoints (which now
    // exist), then notify all that Core is wired. dll paths resolve against the
    // config dir when relative.
    void loadPlugins(config::ConfigSchema const& schema) {
        if (schema.plugins.empty()) return;
        m_ports = std::make_unique<plugin::PortRegistry>(*m_dps, *m_bus);
        for (auto const& pc : schema.plugins) {
            QString dll = qs(pc.dllPath);
            if (QFileInfo(dll).isRelative() && !m_configDir.isEmpty())
                dll = QDir(m_configDir).filePath(dll);
            if (!m_plugins->load(dll)) {
                m_logger->logf(log::LogLevel::Error, QStringLiteral("plugin"), dll,
                               QStringLiteral("failed to load plugin '%1'").arg(qs(pc.name)));
            }
        }
        m_plugins->registerAllPorts(*m_ports);
        for (auto* p : m_plugins->all()) p->onInitialized();
    }

    void startStatsPump() {
        if (m_statsIntervalMs <= 0) return;
        if (m_statsTimer) return;
        m_statsTimer = new QTimer(m_bus.get());
        m_statsTimer->setInterval(m_statsIntervalMs);
        m_statsTimer->setSingleShot(false);
        QObject::connect(m_statsTimer, &QTimer::timeout, m_bus.get(),
            [this]() { publishSchedulerStatsOnce(); });
        m_statsTimer->start();
    }

    void stopStatsPump() {
        if (!m_statsTimer) return;
        m_statsTimer->stop();
        m_statsTimer->deleteLater();
        m_statsTimer = nullptr;
    }

    void installLogBridge() {
        if (!m_qml) return;
        m_logBridge = std::make_unique<qml::LogBridge>(*m_logger);
        m_qml->setContextProperty(QStringLiteral("log"), m_logBridge.get());
    }

    void installEventWiring() {
        // On Transport reconnect (Connected after Disconnected), force every
        // SinkWindow attached to that transport to flush its whole snapshot
        // so PLCs come back with a fresh full picture rather than waiting on
        // sporadic stages.
        m_transportEventSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::TransportEvent>(
                [this](bus::TransportEvent const& e) {
                    logTransportEvent(e);
                    if (e.kind != bus::TransportEventKind::Connected) return;
                    for (auto* sw : m_sinkWindowPtrs) {
                        if (sw->transportId() == qs(e.transportId)) sw->forceFlush();
                    }
                }));
        // On operator-box write into a server transport, fan out into a
        // SinkWindow on the PLC-side transport via the configured routes.
        m_serverWriteSub = std::make_unique<bus::Subscription>(
            m_bus->subscribe<bus::ServerWriteEvent>(
                [this](bus::ServerWriteEvent const& e) {
                    log::OperationRecord op;
                    op.actor    = "operator-box";
                    op.action   = "server-write";
                    op.target   = e.transportId;
                    op.newValue = std::to_string(e.values.size()) + " regs @ "
                                + std::to_string(e.startAddress);
                    op.result   = "ok";
                    op.category = "audit";
                    op.eventKey = "server-write:" + std::to_string(e.startAddress);
                    m_logger->logOperation(std::move(op));
                    if (!serverForwardEnabled(qs(e.transportId))) return;   // 业务闸门:不转发
                    routeServerWrite(e);
                    forwardBridges(e);
                }));
    }

    void logTransportEvent(bus::TransportEvent const& e) {
        char const* what = nullptr;
        auto level = log::LogLevel::Info;
        switch (e.kind) {
            case bus::TransportEventKind::Connected:       what = "connected"; break;
            case bus::TransportEventKind::Disconnected:    what = "disconnected";
                                                           level = log::LogLevel::Warn; break;
            case bus::TransportEventKind::CircuitOpened:   what = "circuit opened";
                                                           level = log::LogLevel::Error; break;
            case bus::TransportEventKind::CircuitClosed:   what = "circuit closed"; break;
            case bus::TransportEventKind::CircuitHalfOpen: what = "circuit half-open"; break;
            default: return;   // Read/WriteCompleted are too noisy for the log
        }
        m_logger->logf(level, QStringLiteral("transport"), qs(e.transportId),
                       QString::fromLatin1(what));
    }

    void routeServerWrite(bus::ServerWriteEvent const& e) {
        // Routes operate on datapoint IDs; for the server→PLC path, we look
        // at each `from` datapoint whose source is the server transport and
        // whose source address falls within the written range, then call
        // stageRegister on the SinkWindow associated with the `to` datapoint.
        for (auto const& r : m_routes) {
            auto fromIt = m_datapointById.find(r.from);
            auto toIt   = m_datapointById.find(r.to);
            if (fromIt == m_datapointById.end() || toIt == m_datapointById.end()) continue;
            auto const& fromDp = fromIt->second;
            auto const& toDp   = toIt->second;
            if (!fromDp->source().has_value()) continue;
            auto const& fromSrc = *fromDp->source();
            if (fromSrc.transport != qs(e.transportId)) continue;
            if (fromSrc.table     != e.table)       continue;
            int const offset = fromSrc.address - e.startAddress;
            if (offset < 0 || offset >= e.values.size()) continue;
            quint16 const raw = e.values.at(offset);

            // Resolve which SinkWindow owns the sink-side register.
            if (!toDp->sink().has_value()) continue;
            auto const& sink = *toDp->sink();
            module::SinkWindow* target = nullptr;
            // Look up by named window first, fall back to address-based match.
            for (auto* sw : m_sinkWindowPtrs) {
                if (!sink.window.isEmpty() && sw->id() == sink.window) {
                    target = sw;
                    break;
                }
            }
            if (!target && !sink.transport.isEmpty()) {
                for (auto* sw : m_sinkWindowPtrs) {
                    if (sw->transportId() != sink.transport) continue;
                    int const addr = sink.address;
                    if (addr < sw->startAddress()
                     || addr >= sw->startAddress() + sw->size()) continue;
                    target = sw;
                    break;
                }
            }
            if (!target) continue;
            int const addr = sink.address;
            if (addr < target->startAddress()
             || addr >= target->startAddress() + target->size()) continue;
            quint16 mask = quint16(sink.mask & 0xFFFFu);
            if (mask == 0) mask = 0xFFFFu;
            target->stageRegister(addr, raw, mask);
        }
    }

    // ——— 整段桥接(替代旧 ModbusServer 中继) ———
    void buildBridges(config::ConfigSchema const& schema) {
        m_bridges = schema.bridges;
        m_bridgeMirrors.assign(m_bridges.size(), {});
        m_bridgeFwdSinks.assign(m_bridges.size(), nullptr);
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            auto& list = m_bridgeMirrors[size_t(i)];
            for (auto const& dp : m_dps->all()) {
                auto const& src = dp->source();
                if (!src.has_value()) continue;
                if (src->transport != qs(b.plc)) continue;
                if (src->table != core::RegisterTable::HoldingRegister) continue;
                int const a = src->address;
                if (a < b.mirrorStart || a >= b.mirrorStart + b.mirrorCount) continue;
                list.emplace_back(a, dp);
            }
            // 转发走 PLC 侧的 SinkWindow(同 route 路径):线程安全地 stageRegister,
            // 由 TickDriver 在生命周期线程带重试/coalesce 地刷到 PLC —— 避免单次
            // submitAsync 在调度器繁忙时被丢弃。
            if (b.writeCount > 0) {
                if (auto* plc = transport(qs(b.plc))) {
                    module::SinkWindow::Config cfg;
                    cfg.moduleId       = QStringLiteral("bridge.fwd.") + qs(b.server);
                    cfg.table          = core::RegisterTable::HoldingRegister;
                    cfg.startAddress   = b.writeStart - b.offset;
                    cfg.size           = b.writeCount;
                    cfg.priority       = sched::Priority::High;
                    cfg.debounceMs     = 0;
                    cfg.keepAlivePeriodMs = 0;
                    cfg.coalesceWrites = true;
                    auto sw = std::make_unique<module::SinkWindow>(std::move(cfg), *plc);
                    m_bridgeFwdSinks[size_t(i)] = sw.get();
                    m_sinkWindowPtrs.push_back(sw.get());
                    m_modules->registerModule(std::move(sw));
                }
            }
        }
    }

    // 操作箱写 server → 写区子段 stage 到 PLC 侧 SinkWindow(server 地址 - offset)。
    // stageRegister 线程安全,可直接在 server transport 线程调用;刷写由 TickDriver 做。
    void forwardBridges(bus::ServerWriteEvent const& e) {
        if (e.table != core::RegisterTable::HoldingRegister) return;
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            if (b.server != e.transportId) continue;
            auto* sink = m_bridgeFwdSinks[size_t(i)];
            if (!sink) continue;
            int const eStart = e.startAddress;
            int const eEnd   = e.startAddress + int(e.values.size());
            int const s  = std::max(eStart, b.writeStart);
            int const en = std::min(eEnd, b.writeStart + b.writeCount);
            for (int a = s; a < en; ++a) {
                sink->stageRegister(a - b.offset, e.values.at(a - eStart));
            }
        }
    }

    // 关闭转发瞬间:把该 server 桥接的整个转发写区在 PLC 侧 SinkWindow 内置 0,
    // 由 TickDriver 下发 —— 取消尚未写入 PLC 的操作箱指令。
    void zeroBridgeForward(QString const& server) {
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            if (qs(b.server) != server) continue;
            auto* sink = m_bridgeFwdSinks[size_t(i)];
            if (!sink) continue;
            for (int a = b.writeStart; a < b.writeStart + b.writeCount; ++a) {
                sink->stageRegister(a - b.offset, 0);
            }
        }
    }

public:
    // 周期把 PLC 读区数据整段镜像回 server 自己的寄存器表(操作箱即可读到)。
    // 也是测试入口(internal::mirrorBridgesOnce)。
    void mirrorBridgesOnce() {
        for (int i = 0; i < int(m_bridges.size()); ++i) {
            auto const& b = m_bridges[size_t(i)];
            if (b.mirrorCount <= 0) continue;
            auto* server = transport(qs(b.server));
            if (!server) continue;
            core::RegisterWords values(b.mirrorCount, quint16(0));
            for (auto const& [addr, dp] : m_bridgeMirrors[size_t(i)]) {
                int const idx = addr - b.mirrorStart;
                if (idx >= 0 && idx < b.mirrorCount) values[idx] = quint16(dp::toUInt64(dp->value()));
            }
            transport::WriteBatch batch;
            batch.table        = core::RegisterTable::HoldingRegister;
            batch.startAddress = b.mirrorStart + b.offset;
            batch.values       = std::move(values);
            sched::RequestTag tag;
            tag.moduleId = QStringLiteral("bridge.mirror.") + qs(b.server);
            tag.priority = sched::Priority::Low;
            tag.coalesce = true;
            server->scheduler().submitAsync(tag, [server, batch](sched::AsyncDone done) {
                server->writeAsync(batch, [done](transport::WriteResult w) mutable { done(w.ok); });
            });
        }
    }

private:
    void startMirrorPump() {
        if (m_mirrorTimer) return;
        int period = 100;
        bool any = false;
        for (auto const& b : m_bridges) {
            if (b.mirrorCount > 0) { any = true; period = std::min(period, std::max(20, b.mirrorPeriodMs)); }
        }
        if (!any) return;
        m_mirrorTimer = new QTimer(m_bus.get());
        m_mirrorTimer->setInterval(period);
        m_mirrorTimer->setSingleShot(false);
        QObject::connect(m_mirrorTimer, &QTimer::timeout, m_bus.get(),
            [this]() { mirrorBridgesOnce(); });
        m_mirrorTimer->start();
    }

    void stopMirrorPump() {
        if (!m_mirrorTimer) return;
        m_mirrorTimer->stop();
        m_mirrorTimer->deleteLater();
        m_mirrorTimer = nullptr;
    }

    void registerCustomCodecs(config::ConfigSchema const& schema) {
        for (auto const& cc : schema.codecs) {
            if (cc.kind == "enum_u16") {
                std::unordered_map<quint16, QString> map;
                for (auto const& [k, v] : cc.map) {
                    try {
                        quint16 raw = quint16(std::stoul(k));
                        map.emplace(raw, qs(dp::toString(v)));
                    } catch (...) { /* skip non-numeric enum keys */ }
                }
                m_codecs->registerCodec(
                    std::make_shared<codec::EnumU16Codec>(qs(cc.id), std::move(map)));
            } else if (cc.kind == "lua") {
                QString script = qs(cc.script);
                if (QFileInfo(script).isRelative() && !m_configDir.isEmpty())
                    script = QDir(m_configDir).filePath(script);
                QString err;
                auto lc = codec::LuaCodec::fromFile(qs(cc.id), script, qs(cc.arg), &err);
                if (lc) {
                    m_codecs->registerCodec(std::move(lc));
                } else {
                    m_logger->logf(log::LogLevel::Error, QStringLiteral("config"),
                                   script,
                                   err.isEmpty()
                                       ? QStringLiteral("lua codec load failed")
                                       : err);
                }
            }
        }
    }

    void buildTransports(config::ConfigSchema const& schema) {
        for (auto const& tc : schema.transports) {
            if (tc.kind == transport::TransportKind::ModbusTcpClient) {
                transport::ModbusTcpClientTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.host                 = qs(tc.host);
                cfg.port                 = quint16(tc.port);
                cfg.slaveId              = tc.slaveId;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::ModbusTcpClientTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::ModbusTcpServer) {
                transport::ModbusTcpServerTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.listenAddress        = qs(tc.listenAddress);
                cfg.listenPort           = quint16(tc.listenPort);
                cfg.slaveId              = tc.slaveId;
                cfg.maxClients           = tc.maxClients;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.listenRanges         = QList<transport::WatchRange>(
                                               tc.listenRanges.begin(),
                                               tc.listenRanges.end());
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::ModbusTcpServerTransport>(
                        std::move(cfg), *m_bus));
            } else if (tc.kind == transport::TransportKind::ModbusRtu) {
                transport::ModbusRtuTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.portName             = qs(tc.portName);
                cfg.baudRate             = tc.baudRate;
                cfg.dataBits             = tc.dataBits;
                cfg.stopBits             = tc.stopBits;
                if      (tc.parity == "even") cfg.parity = transport::ModbusRtuTransport::Parity::Even;
                else if (tc.parity == "odd")  cfg.parity = transport::ModbusRtuTransport::Parity::Odd;
                else                          cfg.parity = transport::ModbusRtuTransport::Parity::None;
                cfg.slaveId              = tc.slaveId;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::ModbusRtuTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::OpcUaClient) {
                transport::OpcUaClientTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.endpointUrl          = qs(tc.endpointUrl);
                cfg.securityPolicy       = qs(tc.securityPolicy);
                cfg.username             = qs(tc.username);
                cfg.password             = qs(tc.password);
                cfg.backend              = qs(tc.opcuaBackend);
                cfg.nodeIdTemplate       = qs(tc.nodeIdTemplate);
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::OpcUaClientTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::MqttClient) {
                transport::MqttClientTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.brokerUri            = qs(tc.brokerUri);
                cfg.clientId             = qs(tc.clientId);
                cfg.username             = qs(tc.username);
                cfg.password             = qs(tc.password);
                cfg.topicPrefix          = qs(tc.topicPrefix);
                cfg.topicTemplate        = qs(tc.topicTemplate);
                cfg.qos                  = tc.qos;
                cfg.cleanSession         = tc.cleanSession;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::MqttClientTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::MqttPahoClient) {
                transport::MqttPahoTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.brokerUri            = qs(tc.brokerUri);
                cfg.clientId             = qs(tc.clientId);
                cfg.username             = qs(tc.username);
                cfg.password             = qs(tc.password);
                cfg.topicPrefix          = qs(tc.topicPrefix);
                cfg.topicTemplate        = qs(tc.topicTemplate);
                cfg.qos                  = tc.qos;
                cfg.cleanSession         = tc.cleanSession;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::MqttPahoTransport>(
                        std::move(cfg), m_bus.get()));
            } else if (tc.kind == transport::TransportKind::S7Client) {
                transport::S7ClientTransport::Config cfg;
                cfg.id                   = qs(tc.id);
                cfg.host                 = qs(tc.host);
                cfg.port                 = tc.port;
                cfg.rack                 = tc.rack;
                cfg.slot                 = tc.slot;
                cfg.connectTimeoutMs     = tc.connectTimeoutMs;
                cfg.requestTimeoutMs     = tc.requestTimeoutMs;
                cfg.reconnectIntervalMs  = tc.reconnectIntervalMs;
                cfg.scheduler            = tc.scheduler;
                m_transports.emplace(
                    qs(tc.id),
                    std::make_unique<transport::S7ClientTransport>(
                        std::move(cfg), m_bus.get()));
            }
        }
    }

    using DpById = std::map<std::string, std::shared_ptr<dp::Datapoint>>;

    void buildSinkWindows(config::ConfigSchema const& schema) {
        for (auto const& sc : schema.sinkWindows) {
            auto* t = transport(qs(sc.transport));
            if (!t) continue;
            module::SinkWindow::Config cfg;
            cfg.moduleId          = qs(sc.moduleId);
            cfg.table             = tableFromString(qs(sc.table));
            cfg.startAddress      = sc.startAddress;
            cfg.size              = sc.size;
            cfg.priority          = sc.priority;
            cfg.debounceMs        = sc.flush.debounceMs;
            cfg.keepAlivePeriodMs = sc.flush.keepaliveMs;
            cfg.coalesceWrites    = sc.flush.coalesceWrites;
            cfg.initial           = sc.initial;
            auto sw = std::make_unique<module::SinkWindow>(std::move(cfg), *t);
            auto* raw = sw.get();
            m_modules->registerModule(std::move(sw));
            m_sinkWindowPtrs.push_back(raw);
        }
    }

    void buildHeartbeats(config::ConfigSchema const& schema) {
        for (auto const& hc : schema.heartbeats) {
            auto* t = transport(qs(hc.transport));
            if (!t) continue;
            module::Heartbeat::Config cfg;
            cfg.moduleId = qs(hc.moduleId);
            cfg.table    = tableFromString(qs(hc.table));
            cfg.address  = hc.address;
            cfg.values   = hc.values;
            cfg.periodMs = hc.periodMs;
            cfg.priority = hc.priority;
            m_modules->registerModule(
                std::make_unique<module::Heartbeat>(std::move(cfg), *t));
        }
    }

    void buildAckWatches(config::ConfigSchema const& schema) {
        for (auto const& ac : schema.ackWatches) {
            module::AckWatch::Config cfg;
            cfg.moduleId  = qs(ac.moduleId);
            cfg.dpId      = qs(ac.dp);
            cfg.expected  = dp::toQVariant(ac.expected);
            cfg.timeoutMs = ac.timeoutMs;
            m_modules->registerModule(
                std::make_unique<module::AckWatch>(std::move(cfg), *m_bus));
        }
    }

    void buildCommands(config::ConfigSchema const& schema) {
        for (auto const& cc : schema.commands) {
            auto* t = transport(qs(cc.transport));
            if (!t) continue;
            module::Command::Config cfg;
            cfg.moduleId      = qs(cc.moduleId);
            cfg.priority      = cc.priority;
            cfg.interruptable = cc.interruptable;
            for (auto const& w : cc.writes) {
                module::Command::Entry e;
                e.table   = tableFromString(qs(w.table));
                e.address = w.address;
                e.value   = w.value;
                cfg.writes.append(e);
            }
            m_modules->registerModule(
                std::make_unique<module::Command>(std::move(cfg), *t));
        }
    }

    DpById buildDatapoints(config::ConfigSchema const& schema) {
        DpById out;
        for (auto const& dc : schema.datapoints) {
            std::shared_ptr<codec::Codec> sourceCodec;
            if (dc.hasSource) {
                if (!dc.source.codec.empty()) {
                    sourceCodec = m_codecs->find(qs(dc.source.codec));
                }
                if (!sourceCodec) {
                    sourceCodec = m_codecs->find(
                        codec::BuiltinScalarCodec::idFor(dc.type));
                }
            }

            dp::DatapointSpec spec;
            spec.id         = dc.id;
            spec.kind       = kindFromString(dc.kind);
            spec.type       = dc.type;
            if (dc.hasSource) spec.source = makePortRef(dc.source, sourceCodec);
            if (dc.hasSink) {
                spec.sink = makePortRef(dc.sink,
                    m_codecs->find(codec::BuiltinScalarCodec::idFor(dc.type)));
            }
            spec.uiBinding  = dc.ui;
            spec.persistTag = dc.persist;

            auto datapoint = std::make_shared<dp::Datapoint>(std::move(spec));
            m_dps->registerDp(datapoint);
            out.emplace(dc.id, datapoint);
            m_datapointById.emplace(dc.id, datapoint);

            // Auto-publish DpChanged on every value change so plugins /
            // database / dashboard subscribers don't need to poll. The model is
            // Qt-free now, so we hop to the bus (QObject) thread ourselves to
            // preserve the old queued-signal semantics: setValue runs on a
            // transport worker thread, DpChanged is published on the bus thread.
            std::weak_ptr<dp::Datapoint> weak = datapoint;
            datapoint->setOnValueChanged([this, weak] {
                QMetaObject::invokeMethod(m_bus.get(), [this, weak] {
                    auto sp = weak.lock();
                    if (!sp) return;
                    auto const snap = sp->snapshot();
                    m_bus->publish(bus::DpChanged{
                        sp->id(), snap.value, snap.timestamp});
                }, Qt::QueuedConnection);
            });

            // For Command / Bidirectional datapoints with a SinkWindow sink,
            // wire a writer that encodes the value through the codec then
            // stages into the named SinkWindow. QML calls `dp.write(v)` and
            // the new value lands on the PLC at the next flush tick.
            if ((datapoint->kind() == dp::Kind::Command
                 || datapoint->kind() == dp::Kind::Bidirectional)
                && datapoint->sink().has_value()
                && !datapoint->sink()->window.isEmpty()) {
                QString const windowId = datapoint->sink()->window;
                datapoint->setWriter([this, weak, windowId](core::dp::Value const& v) {
                    auto sp = weak.lock();
                    if (!sp || !sp->sink().has_value()) return;
                    auto sink = *sp->sink();
                    if (!sink.codec) return;
                    auto encoded = sink.codec->encode(v, sink);
                    if (encoded.empty()) return;
                    for (auto* sw : m_sinkWindowPtrs) {
                        if (sw->id() != windowId) continue;
                        for (int i = 0; i < encoded.size(); ++i) {
                            quint16 mask = (sink.bit.has_value() && i == 0)
                                ? quint16(1u << *sink.bit)
                                : 0xFFFFu;
                            sw->stageRegister(sink.address + i,
                                              encoded.at(i), mask);
                        }
                        break;
                    }
                });
            }
        }
        return out;
    }

    void buildPollRanges(config::ConfigSchema const& schema,
                          DpById const&                byId) {
        for (auto const& pc : schema.pollRanges) {
            auto* t = transport(qs(pc.transport));
            if (!t) continue;
            transport::ReadRequest req;
            req.table        = tableFromString(qs(pc.table));
            req.startAddress = pc.startAddress;
            req.count        = pc.count;

            auto poll = std::make_unique<module::PollRange>(
                qs(pc.moduleId), *t, req, pc.periodMs, pc.priority);
            wireBindings(*poll, schema, byId, pc.transport, req);

            auto* raw = poll.get();
            m_modules->registerModule(std::move(poll));
            m_pollRangePtrs.push_back(raw);
        }
    }

    void wireBindings(module::PollRange&            poll,
                       config::ConfigSchema const&    schema,
                       DpById const&                   byId,
                       std::string const&              transportId,
                       transport::ReadRequest const&   req) {
        for (auto const& dc : schema.datapoints) {
            if (!dc.hasSource) continue;
            if (dc.source.port != transportId) continue;
            if (tableFromString(qs(dc.source.table)) != req.table) continue;
            int const rc     = dp::registerCountFor(dc.type);
            int const offset = dc.source.address - req.startAddress;
            if (offset < 0 || offset + rc > req.count) continue;
            auto itDp = byId.find(dc.id);
            if (itDp == byId.end()) continue;

            auto codec = m_codecs->find(dc.source.codec.empty()
                ? codec::BuiltinScalarCodec::idFor(dc.type)
                : qs(dc.source.codec));
            if (!codec) continue;
            poll.bind(itDp->second, codec, offset);
        }
    }

private:
    QQmlContext*                                                m_qml;
    QString                                                     m_configDir;
    std::unique_ptr<log::Logger>                                m_logger;
    std::unique_ptr<qml::LogBridge>                             m_logBridge;
    std::unique_ptr<bus::EventBus>                              m_bus;
    std::unique_ptr<codec::CodecRegistry>                       m_codecs;
    std::unique_ptr<dp::DatapointRegistry>                      m_dps;
    std::unique_ptr<module::ModuleRegistry>                     m_modules;
    std::unique_ptr<plugin::PluginRegistry>                     m_plugins;
    std::unique_ptr<plugin::PortRegistry>                       m_ports;
    std::map<QString, std::unique_ptr<transport::Transport>>    m_transports;
    std::vector<std::thread>                                    m_connectThreads;
    bool                                                        m_started = false;
    std::vector<module::PollRange*>                             m_pollRangePtrs;
    std::vector<module::SinkWindow*>                            m_sinkWindowPtrs;
    std::map<std::string, std::shared_ptr<dp::Datapoint>>      m_datapointById;
    std::vector<config::RouteConfig>                            m_routes;
    std::vector<config::BridgeConfig>                           m_bridges;
    std::vector<std::vector<std::pair<int, std::shared_ptr<dp::Datapoint>>>> m_bridgeMirrors;
    std::vector<module::SinkWindow*>                            m_bridgeFwdSinks;
    mutable std::mutex                                          m_forwardMtx;
    QHash<QString, bool>                                        m_forwardEnabled;   // server id → 转发使能(默认 true)
    QTimer*                                                     m_mirrorTimer = nullptr;
    std::unique_ptr<bus::Subscription>                          m_transportEventSub;
    std::unique_ptr<bus::Subscription>                          m_serverWriteSub;
    QTimer*                                                     m_statsTimer = nullptr;
    int                                                         m_statsIntervalMs = 1000;
};

std::unique_ptr<ICore> ICore::create(QQmlContext* qml, bool installDefaultConsole) {
    return std::make_unique<CoreImpl>(qml, installDefaultConsole);
}

namespace internal {
void pollAllOnce(ICore& core) {
    static_cast<CoreImpl&>(core).pollAllOnce();
}
void tickSinkWindowsOnce(ICore& core) {
    static_cast<CoreImpl&>(core).tickSinkWindowsOnce();
}
void publishSchedulerStatsOnce(ICore& core) {
    static_cast<CoreImpl&>(core).publishSchedulerStatsOnce();
}
void mirrorBridgesOnce(ICore& core) {
    static_cast<CoreImpl&>(core).mirrorBridgesOnce();
}
} // namespace internal

} // namespace core
