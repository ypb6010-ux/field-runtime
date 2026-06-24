// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "GatewayAssembly.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "AsioModbusRtuClient.h"
#include "AsioModbusTcpClient.h"
#include "AsioModbusTcpServer.h"
#ifdef FIELDRUNTIME_GATEWAY_HAS_OPCUA
#include "AsioOpcUaClient.h"
#endif
#ifdef FIELDRUNTIME_GATEWAY_HAS_S7
#include "AsioS7Client.h"
#endif
#include "GatewayJson.h"
#include "SqliteLogSink.h"
#include "StubTransport.h"

#include "core/bus/BusEvents.h"
#include "core/codec/BuiltinCodecs.h"
#include "core/codec/LuaCodec.h"
#include "core/config/ConfigLoader.h"
#include "core/dp/Datapoint.h"
#include "core/dp/ScalarType.h"
#include "core/dp/Value.h"
#include "core/dp/WordOrder.h"
#include "core/log/Sinks.h"
#include "core/module/SinkWindow.h"

namespace core::gateway {

namespace {

core::RegisterTable tableFromString(std::string const& s) {
    if (s == "IR" || s == "InputRegisters") return core::RegisterTable::InputRegister;
    if (s == "Coil" || s == "Coils") return core::RegisterTable::Coil;
    if (s == "DI" || s == "DiscreteInputs") return core::RegisterTable::DiscreteInput;
    return core::RegisterTable::HoldingRegister;
}

dp::WordOrder wordOrderFromString(std::string const& s) {
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    return dp::WordOrder::ABCD;
}

dp::Kind kindFromString(std::string const& s) {
    if (s == "Command") return dp::Kind::Command;
    if (s == "Bidirectional") return dp::Kind::Bidirectional;
    return dp::Kind::Status;
}

dp::PortRef makePortRef(config::PortRefConfig const& pc,
                        std::shared_ptr<codec::Codec> codec) {
    dp::PortRef p;
    p.transport = pc.port;
    p.table = tableFromString(pc.table);
    p.address = pc.address;
    if (pc.bit >= 0) p.bit = pc.bit;
    p.wordOrder = wordOrderFromString(pc.wordOrder);
    p.shift = pc.shift;
    p.mask = pc.mask;
    p.scale = pc.scale;
    p.offset = pc.offset;
    p.codec = std::move(codec);
    p.window = pc.window;
    return p;
}

std::string valueText(dp::Value const& value) {
    return dp::toString(value);
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string stripComment(std::string const& line) {
    bool inQuote = false;
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); i++) {
        char const c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }
        if (c == '#' && !inQuote) return line.substr(0, i);
    }
    return line;
}

std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::optional<ControlConfig> parseControlConfig(std::string const& path,
                                                std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return std::nullopt;
    }

    ControlConfig cfg;
    bool inControl = false;
    bool seenControl = false;
    std::string line;
    while (std::getline(in, line)) {
        auto const trimmed = trim(stripComment(line));
        if (trimmed.empty()) continue;
        if (trimmed.front() == '[') {
            inControl = trimmed == "[control]";
            seenControl = seenControl || inControl;
            continue;
        }
        if (!inControl) continue;

        auto const eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        auto const key = trim(std::string_view(trimmed).substr(0, eq));
        auto const value = unquote(trimmed.substr(eq + 1));
        if (key == "listen_address") {
            cfg.listenAddress = value;
        } else if (key == "listen_port") {
            try {
                cfg.listenPort = std::stoi(value);
            } catch (...) {
                error = "control.listen_port must be an integer";
                return std::nullopt;
            }
        } else if (key == "auth_token") {
            cfg.authToken = value;
        }
    }

    if (!seenControl) return std::nullopt;
    if (cfg.listenPort <= 0 || cfg.listenPort > 65535) {
        error = "control.listen_port must be in 1..65535";
        return std::nullopt;
    }
    if (cfg.listenAddress.empty()) cfg.listenAddress = "127.0.0.1";
    return cfg;
}

bool parseBool(std::string value) {
    value = trim(value);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

std::optional<MqttNorthboundConfig> parseMqttConfig(std::string const& path,
                                                    std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return std::nullopt;
    }

    MqttNorthboundConfig cfg;
    bool inMqtt = false;
    bool seenMqtt = false;
    std::string line;
    while (std::getline(in, line)) {
        auto const trimmed = trim(stripComment(line));
        if (trimmed.empty()) continue;
        if (trimmed.front() == '[') {
            inMqtt = trimmed == "[northbound.mqtt]";
            seenMqtt = seenMqtt || inMqtt;
            continue;
        }
        if (!inMqtt) continue;

        auto const eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        auto const key = trim(std::string_view(trimmed).substr(0, eq));
        auto const value = unquote(trimmed.substr(eq + 1));
        try {
            if (key == "enable") {
                cfg.enable = parseBool(value);
            } else if (key == "host") {
                cfg.host = value;
            } else if (key == "port") {
                cfg.port = std::stoi(value);
            } else if (key == "client_id") {
                cfg.clientId = value;
            } else if (key == "keepalive_s") {
                cfg.keepaliveS = std::stoi(value);
            } else if (key == "topic_prefix") {
                cfg.topicPrefix = value;
            } else if (key == "qos") {
                cfg.qos = std::stoi(value);
            } else if (key == "publish_interval_ms") {
                cfg.publishIntervalMs = std::stoi(value);
            }
        } catch (...) {
            error = "northbound.mqtt." + key + " has invalid value";
            return std::nullopt;
        }
    }

    if (!seenMqtt || !cfg.enable) return std::nullopt;
    if (cfg.host.empty()) {
        error = "northbound.mqtt.host must not be empty";
        return std::nullopt;
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        error = "northbound.mqtt.port must be in 1..65535";
        return std::nullopt;
    }
    if (cfg.keepaliveS <= 0) {
        error = "northbound.mqtt.keepalive_s must be > 0";
        return std::nullopt;
    }
    if (cfg.qos != 0 && cfg.qos != 1) {
        error = "northbound.mqtt.qos currently supports only 0 or 1";
        return std::nullopt;
    }
    if (cfg.publishIntervalMs < 0) {
        error = "northbound.mqtt.publish_interval_ms must be >= 0";
        return std::nullopt;
    }
    if (cfg.clientId.empty()) cfg.clientId = "field_gateway";
    if (cfg.topicPrefix.empty()) cfg.topicPrefix = "field";
    return cfg;
}

std::optional<PersistenceConfig> parsePersistenceConfig(std::string const& path,
                                                        std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return std::nullopt;
    }

    PersistenceConfig cfg;
    bool inPersistence = false;
    bool seenPersistence = false;
    std::string line;
    while (std::getline(in, line)) {
        auto const trimmed = trim(stripComment(line));
        if (trimmed.empty()) continue;
        if (trimmed.front() == '[') {
            inPersistence = trimmed == "[persistence]";
            seenPersistence = seenPersistence || inPersistence;
            continue;
        }
        if (!inPersistence) continue;

        auto const eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        auto const key = trim(std::string_view(trimmed).substr(0, eq));
        auto const value = unquote(trimmed.substr(eq + 1));
        try {
            if (key == "enable") {
                cfg.enable = parseBool(value);
            } else if (key == "path") {
                cfg.path = value;
            } else if (key == "max_rows") {
                cfg.maxRows = std::stoi(value);
            } else if (key == "backfill_batch") {
                cfg.backfillBatch = std::stoi(value);
            }
        } catch (...) {
            error = "persistence." + key + " has invalid value";
            return std::nullopt;
        }
    }

    if (!seenPersistence || !cfg.enable) return std::nullopt;
    if (cfg.path.empty()) cfg.path = "field_gateway.db";
    if (cfg.maxRows <= 0) {
        error = "persistence.max_rows must be > 0";
        return std::nullopt;
    }
    if (cfg.backfillBatch <= 0) {
        error = "persistence.backfill_batch must be > 0";
        return std::nullopt;
    }
    return cfg;
}

} // namespace

GatewayAssembly::GatewayAssembly(gateway_asio::io_context& io)
    : m_io(&io) {
    m_codecs.loadBuiltins();
    m_logger.addSink(std::make_shared<log::ConsoleSink>());
    m_logger.addSink(std::make_shared<log::RollingFileSink>("field_gateway.log"));
}

GatewayAssembly::~GatewayAssembly() {
    stop();
    m_logger.stop();
}

log::Logger& GatewayAssembly::logger() {
    return m_logger;
}

bool GatewayAssembly::load(std::string const& tomlPath) {
    config::ConfigLoader loader;
    auto loaded = loader.loadFromToml(tomlPath);
    if (!loaded.has_value()) {
        for (auto const& err : loaded.error()) {
            std::cerr << err.section << "." << err.field << ": "
                      << err.message << '\n';
            m_logger.logf(log::LogLevel::Error, "config", tomlPath, err.message);
        }
        return false;
    }

    if (!loaded->meta.logLevel.empty()) {
        m_logger.setThreshold(log::levelFromString(loaded->meta.logLevel));
    }

    m_configDir = std::filesystem::path(tomlPath).parent_path().string();

    std::string controlError;
    m_control = parseControlConfig(tomlPath, controlError);
    if (!controlError.empty()) {
        m_logger.logf(log::LogLevel::Error, "config", "control", controlError);
        std::cerr << "control: " << controlError << '\n';
        return false;
    }

    std::string mqttError;
    m_mqttConfig = parseMqttConfig(tomlPath, mqttError);
    if (!mqttError.empty()) {
        m_logger.logf(log::LogLevel::Error, "config", "northbound.mqtt", mqttError);
        std::cerr << "northbound.mqtt: " << mqttError << '\n';
        return false;
    }

    std::string persistenceParseError;
    m_persistenceConfig = parsePersistenceConfig(tomlPath, persistenceParseError);
    if (!persistenceParseError.empty()) {
        m_logger.logf(log::LogLevel::Error,
                      "config",
                      "persistence",
                      persistenceParseError);
        std::cerr << "persistence: " << persistenceParseError << '\n';
        return false;
    }

    wireFromSchema(*loaded);
    if (m_mqttConfig) {
        m_mqtt = std::make_unique<AsioMqttClient>(*m_io, *m_mqttConfig);
        m_mqtt->setConnectedCallback([this] {
            backfillPersistenceOnce();
        });
    }
    if (m_persistenceConfig) {
        m_persistence = std::make_unique<Persistence>();
        std::string persistenceError;
        if (!m_persistence->open(*m_persistenceConfig, persistenceError)) {
            m_logger.logf(log::LogLevel::Error,
                          "persistence",
                          m_persistenceConfig->path,
                          persistenceError);
            std::cerr << "persistence: " << persistenceError << '\n';
            return false;
        }
        // Persist Info+ logs into the same db's `logs` table. The sink owns its
        // own connection and runs on the Logger dispatch thread (see its docs).
        m_logger.addSink(
            std::make_shared<SqliteLogSink>(m_persistenceConfig->path));
    }
    m_logger.logf(log::LogLevel::Info, "gateway", "assembly", "loaded",
                  {{"transports", std::int64_t(loaded->transports.size())},
                   {"datapoints", std::int64_t(loaded->datapoints.size())},
                   {"poll_ranges", std::int64_t(loaded->pollRanges.size())}});
    return true;
}

void GatewayAssembly::start() {
    if (m_started) return;
    m_started = true;
    installEventWiring();

    if (m_mqtt) {
        m_mqtt->start();
        m_logger.logf(log::LogLevel::Info,
                      "gateway",
                      "mqtt",
                      "northbound connecting "
                          + m_mqttConfig->host + ":"
                          + std::to_string(m_mqttConfig->port));
    }

    for (auto& [id, transport] : m_transports) {
        auto connected = transport->connect();
        if (connected.has_value()) {
            m_bus.publish(bus::TransportEvent{id, bus::TransportEventKind::Connected, {}});
            m_logger.logf(log::LogLevel::Info, "transport", id, "connected");
        } else {
            m_logger.logf(log::LogLevel::Error, "transport", id, connected.error());
        }
    }

    for (auto& poll : m_pollRanges) poll->start();
    for (auto& sink : m_sinkWindows) sink->start();
    for (auto& timer : m_pollTimers) schedulePoll(timer);
    for (auto& timer : m_sinkTimers) scheduleSink(timer);
    startMirrorPump();
    startMqttSnapshotPump();
    startPersistenceBackfillPump();
    m_bus.publish(bus::CoreReady{});
}

void GatewayAssembly::stop() {
    if (!m_started) return;
    m_started = false;

    for (auto& timer : m_pollTimers) {
        if (timer.timer) timer.timer->cancel();
    }
    for (auto& timer : m_sinkTimers) {
        if (timer.timer) timer.timer->cancel();
    }
    if (m_mirrorTimer) m_mirrorTimer->cancel();
    if (m_mqttSnapshotTimer) m_mqttSnapshotTimer->cancel();
    if (m_backfillTimer) m_backfillTimer->cancel();
    for (auto& poll : m_pollRanges) poll->stop();
    for (auto& sink : m_sinkWindows) sink->stop();
    for (auto& [id, transport] : m_transports) {
        transport->disconnect();
        m_bus.publish(bus::TransportEvent{id, bus::TransportEventKind::Disconnected, {}});
    }
    m_serverWriteSub.reset();
    m_mqttDpChangedSub.reset();
    if (m_mqtt) m_mqtt->stop();
    if (m_persistence) m_persistence->close();
    m_bus.publish(bus::CoreStopping{});
    m_logger.flush();
}

void GatewayAssembly::setServerForwardEnabled(std::string const& serverTransportId,
                                              bool enabled) {
    bool wasEnabled = true;
    {
        std::lock_guard lk(m_forwardMtx);
        auto it = m_forwardEnabled.find(serverTransportId);
        if (it != m_forwardEnabled.end()) wasEnabled = it->second;
        m_forwardEnabled[serverTransportId] = enabled;
    }
    if (wasEnabled && !enabled) zeroBridgeForward(serverTransportId);
}

std::optional<ControlConfig> GatewayAssembly::controlConfig() const {
    return m_control;
}

std::vector<GatewayTransportSnapshot> GatewayAssembly::transportSnapshots() {
    std::vector<GatewayTransportSnapshot> out;
    out.reserve(m_transports.size());
    for (auto& [id, transport] : m_transports) {
        GatewayTransportSnapshot snap;
        snap.id = id;
        snap.kind = transport->kind();
        snap.state = transport->state();
        snap.stats = transport->scheduler().stats();
        out.push_back(std::move(snap));
    }
    return out;
}

std::vector<GatewayDatapointSnapshot> GatewayAssembly::datapointSnapshots() const {
    auto all = m_datapoints.all();
    std::sort(all.begin(), all.end(), [](auto const& a, auto const& b) {
        return a->id() < b->id();
    });

    std::vector<GatewayDatapointSnapshot> out;
    out.reserve(all.size());
    for (auto const& dp : all) {
        auto const state = dp->snapshot();
        out.push_back(GatewayDatapointSnapshot{
            dp->id(),
            state.value,
            state.state,
            state.timestamp
        });
    }
    return out;
}

bool GatewayAssembly::hasTransport(std::string const& transportId) const {
    return m_transports.find(transportId) != m_transports.end();
}

bool GatewayAssembly::hasServerTransport(std::string const& transportId) const {
    auto it = m_transports.find(transportId);
    if (it == m_transports.end()) return false;
    return it->second->kind() == transport::TransportKind::ModbusTcpServer;
}

bool GatewayAssembly::writeTransportAsync(
    std::string const& transportId,
    transport::WriteBatch batch,
    std::function<void(transport::WriteResult)> done) {
    auto it = m_transports.find(transportId);
    if (it == m_transports.end()) return false;
    auto* transport = it->second.get();

    sched::RequestTag tag;
    tag.moduleId = "control.write." + transportId;
    tag.priority = sched::Priority::High;
    tag.coalesce = false;

    auto doneShared = std::make_shared<std::function<void(transport::WriteResult)>>(
        std::move(done));
    auto submit = transport->scheduler().submitAsync(
        std::move(tag),
        [transport, batch = std::move(batch), doneShared](
            sched::AsyncDone schedulerDone) mutable {
            transport->writeAsync(batch,
                [schedulerDone = std::move(schedulerDone),
                 doneShared](transport::WriteResult result) mutable {
                    bool const ok = result.ok;
                    if (*doneShared) (*doneShared)(std::move(result));
                    schedulerDone(ok);
                });
        });
    if (submit.kind != sched::ResultKind::Ok) {
        if (*doneShared) (*doneShared)(transport::WriteResult{false, submit.errorMessage});
    }
    return true;
}

void GatewayAssembly::printSnapshot(std::size_t limit) const {
    auto all = m_datapoints.all();
    std::sort(all.begin(), all.end(), [](auto const& a, auto const& b) {
        return a->id() < b->id();
    });

    std::cout << "snapshot";
    std::size_t n = 0;
    for (auto const& dp : all) {
        if (n++ >= limit) break;
        auto const state = dp->snapshot();
        std::cout << ' ' << dp->id() << '=' << valueText(state.value)
                  << '(' << dp->stateText() << ')';
    }
    std::cout << std::endl;
}

void GatewayAssembly::wireFromSchema(config::ConfigSchema const& schema) {
    registerCodecs(schema);
    buildTransports(schema);
    auto byId = buildDatapoints(schema);
    buildPollRanges(schema, byId);
    buildSinkWindows(schema);
    buildBridges(schema);
}

// Register the schema's `[[codec]]` entries (enum_u16 / lua) into m_codecs
// BEFORE datapoints are built. Without this, buildDatapoints / wireBindings
// look up the configured codec id, miss, and silently fall back to the builtin
// scalar codec — so any configured enum/lua transform was being ignored.
// Mirrors Core::registerCustomCodecs (Qt assembly). If the Base library was
// built without Lua (CORE_BUILD_LUA=OFF) LuaCodec::fromFile returns nullptr and
// we log, exactly like the Qt path.
void GatewayAssembly::registerCodecs(config::ConfigSchema const& schema) {
    for (auto const& cc : schema.codecs) {
        if (cc.kind == "enum_u16") {
            std::unordered_map<std::uint16_t, std::string> map;
            for (auto const& [k, v] : cc.map) {
                try {
                    auto const raw = std::uint16_t(std::stoul(k));
                    map.emplace(raw, dp::toString(v));
                } catch (...) {
                    // skip non-numeric enum keys
                }
            }
            m_codecs.registerCodec(
                std::make_shared<codec::EnumU16Codec>(cc.id, std::move(map)));
        } else if (cc.kind == "lua") {
            std::string script = cc.script;
            std::filesystem::path scriptPath(script);
            if (scriptPath.is_relative() && !m_configDir.empty()) {
                script = (std::filesystem::path(m_configDir) / scriptPath).string();
            }
            std::string err;
            auto lc = codec::LuaCodec::fromFile(cc.id, script, cc.arg, &err);
            if (lc) {
                m_codecs.registerCodec(std::move(lc));
            } else {
                m_logger.logf(log::LogLevel::Error, "config", script,
                              err.empty()
                                  ? std::string("lua codec load failed (CORE_BUILD_LUA?)")
                                  : err);
            }
        } else {
            m_logger.logf(log::LogLevel::Warn, "config", cc.id,
                          "unknown codec kind '" + cc.kind + "'");
        }
    }
}

void GatewayAssembly::buildTransports(config::ConfigSchema const& schema) {
    for (auto const& tc : schema.transports) {
        std::unique_ptr<transport::Transport> transport;
        if (tc.kind == transport::TransportKind::ModbusTcpClient) {
            transport = std::make_unique<AsioModbusTcpClient>(tc, *m_io);
        } else if (tc.kind == transport::TransportKind::ModbusTcpServer) {
            transport = std::make_unique<AsioModbusTcpServer>(tc, *m_io, m_bus);
        } else if (tc.kind == transport::TransportKind::ModbusRtu) {
            transport = std::make_unique<AsioModbusRtuClient>(tc, *m_io);
#ifdef FIELDRUNTIME_GATEWAY_HAS_OPCUA
        } else if (tc.kind == transport::TransportKind::OpcUaClient) {
            transport = std::make_unique<AsioOpcUaClient>(tc, *m_io);
#endif
#ifdef FIELDRUNTIME_GATEWAY_HAS_S7
        } else if (tc.kind == transport::TransportKind::S7Client) {
            transport = std::make_unique<AsioS7Client>(tc, *m_io);
#endif
        } else {
            transport = std::make_unique<StubTransport>(tc, *m_io);
        }
        m_transports.emplace(tc.id, std::move(transport));
    }
}

GatewayAssembly::DpById GatewayAssembly::buildDatapoints(config::ConfigSchema const& schema) {
    DpById out;
    for (auto const& dc : schema.datapoints) {
        std::shared_ptr<codec::Codec> sourceCodec;
        if (dc.hasSource) {
            if (!dc.source.codec.empty()) {
                sourceCodec = m_codecs.find(dc.source.codec);
            }
            if (!sourceCodec) {
                sourceCodec = m_codecs.find(codec::BuiltinScalarCodec::idFor(dc.type));
            }
        }

        dp::DatapointSpec spec;
        spec.id = dc.id;
        spec.kind = kindFromString(dc.kind);
        spec.type = dc.type;
        if (dc.hasSource) spec.source = makePortRef(dc.source, sourceCodec);
        if (dc.hasSink) {
            spec.sink = makePortRef(
                dc.sink,
                m_codecs.find(codec::BuiltinScalarCodec::idFor(dc.type)));
        }
        spec.uiBinding = dc.ui;
        spec.persistTag = dc.persist;

        auto datapoint = std::make_shared<dp::Datapoint>(std::move(spec));
        std::weak_ptr<dp::Datapoint> weak = datapoint;
        datapoint->setOnValueChanged([this, weak] {
            auto sp = weak.lock();
            if (!sp) return;
            auto const snap = sp->snapshot();
            m_bus.publish(bus::DpChanged{sp->id(), snap.value, snap.timestamp});
        });

        m_datapoints.registerDp(datapoint);
        out.emplace(dc.id, std::move(datapoint));
    }
    return out;
}

void GatewayAssembly::buildPollRanges(config::ConfigSchema const& schema,
                                      DpById const& byId) {
    for (auto const& pc : schema.pollRanges) {
        auto it = m_transports.find(pc.transport);
        if (it == m_transports.end()) continue;

        transport::ReadRequest req;
        req.table = tableFromString(pc.table);
        req.startAddress = pc.startAddress;
        req.count = pc.count;

        auto poll = std::make_unique<module::PollRange>(
            pc.moduleId, *it->second, req, pc.periodMs, pc.priority);
        wireBindings(*poll, schema, byId, pc.transport, req);

        m_pollTimers.push_back(PollTimer{
            poll.get(),
            std::make_unique<gateway_asio::steady_timer>(*m_io)
        });
        m_pollRanges.push_back(std::move(poll));
    }
}

void GatewayAssembly::buildSinkWindows(config::ConfigSchema const& schema) {
    for (auto const& sc : schema.sinkWindows) {
        auto it = m_transports.find(sc.transport);
        if (it == m_transports.end()) continue;

        module::SinkWindow::Config cfg;
        cfg.moduleId = sc.moduleId;
        cfg.table = tableFromString(sc.table);
        cfg.startAddress = sc.startAddress;
        cfg.size = sc.size;
        cfg.priority = sc.priority;
        cfg.debounceMs = sc.flush.debounceMs;
        cfg.keepAlivePeriodMs = sc.flush.keepaliveMs;
        cfg.coalesceWrites = sc.flush.coalesceWrites;
        cfg.initial = sc.initial;

        auto sink = std::make_unique<module::SinkWindow>(std::move(cfg), *it->second);
        m_sinkTimers.push_back(SinkTimer{
            sink.get(),
            std::make_unique<gateway_asio::steady_timer>(*m_io)
        });
        m_sinkWindows.push_back(std::move(sink));
    }
}

void GatewayAssembly::buildBridges(config::ConfigSchema const& schema) {
    m_bridges = schema.bridges;
    m_bridgeFwdSinks.assign(m_bridges.size(), nullptr);

    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.writeCount <= 0) continue;
        auto it = m_transports.find(bridge.plc);
        if (it == m_transports.end()) continue;

        module::SinkWindow::Config cfg;
        cfg.moduleId = "bridge.fwd." + bridge.server;
        cfg.table = core::RegisterTable::HoldingRegister;
        cfg.startAddress = bridge.writeStart - bridge.offset;
        cfg.size = bridge.writeCount;
        cfg.priority = sched::Priority::High;
        cfg.debounceMs = 0;
        cfg.keepAlivePeriodMs = 0;
        cfg.coalesceWrites = true;

        auto sink = std::make_unique<module::SinkWindow>(std::move(cfg), *it->second);
        m_bridgeFwdSinks[size_t(i)] = sink.get();
        m_sinkTimers.push_back(SinkTimer{
            sink.get(),
            std::make_unique<gateway_asio::steady_timer>(*m_io)
        });
        m_sinkWindows.push_back(std::move(sink));
    }
}

void GatewayAssembly::installEventWiring() {
    if (m_serverWriteSub) return;
    m_serverWriteSub = std::make_unique<bus::Subscription>(
        m_bus.subscribe<bus::ServerWriteEvent>(
            [this](bus::ServerWriteEvent const& e) {
                m_logger.logf(log::LogLevel::Info,
                              "gateway",
                              e.transportId,
                              "server-write "
                                  + std::to_string(e.values.size())
                                  + " regs @ "
                                  + std::to_string(e.startAddress));
                if (!serverForwardEnabled(e.transportId)) return;
                forwardBridges(e);
            }));
    if (m_mqtt && !m_mqttDpChangedSub) {
        m_mqttDpChangedSub = std::make_unique<bus::Subscription>(
            m_bus.subscribe<bus::DpChanged>(
                [this](bus::DpChanged const& e) {
                    auto dp = m_datapoints.find(e.id);
                    if (!dp) return;
                    auto const snap = dp->snapshot();
                    publishMqttDatapoint(e.id, snap.value, snap.state, snap.timestamp);
                }));
    }
}

void GatewayAssembly::publishMqttDatapoint(std::string const& id,
                                           dp::Value const& value,
                                           dp::DpState state,
                                           dp::Timestamp timestamp) {
    if (m_persistence && m_persistence->enabled()) {
        std::string error;
        auto rowId = m_persistence->insertTelemetry(id, value, state, timestamp, error);
        if (!rowId) {
            if (!error.empty()) {
                m_logger.logf(log::LogLevel::Error, "persistence", id, error);
            }
            return;
        }
        if (m_mqtt && m_mqtt->connected() && m_mqttConfig) {
            TelemetryRow row;
            row.rowId = *rowId;
            row.dpId = id;
            row.valueJson = json::value(value);
            row.quality = json::dpState(state);
            row.ts = json::timestampMs(timestamp);
            publishMqttRow(row);
        }
        return;
    }

    if (!m_mqtt || !m_mqttConfig) return;
    auto topic = m_mqttConfig->topicPrefix + "/" + id;
    std::string payload = "{\"value\":";
    payload += json::value(value);
    payload += ",\"quality\":";
    json::appendString(payload, json::dpState(state));
    payload += ",\"ts\":" + std::to_string(json::timestampMs(timestamp));
    payload += "}";
    m_mqtt->publish(std::move(topic), std::move(payload), m_mqttConfig->qos);
}

void GatewayAssembly::publishMqttRow(TelemetryRow const& row) {
    if (!m_mqtt || !m_mqttConfig || !m_persistence) return;
    if (!m_mqtt->connected()) return;

    // Max telemetry rows outstanding (handed to MQTT, awaiting PUBACK) at once.
    static constexpr std::size_t kMaxInflightRows = 256;
    auto const rowId = row.rowId;
    // Already awaiting PUBACK, or too many outstanding: skip. The backfill pump
    // retries once acks drain — so a delayed/suppressed PUBACK no longer makes
    // us resend the same pending row every cycle (duplicates) or grow unbounded.
    if (m_inflightRows.count(rowId)) return;
    if (m_inflightRows.size() >= kMaxInflightRows) return;

    std::string topic = m_mqttConfig->topicPrefix + "/" + row.dpId;
    std::string payload = "{\"value\":";
    payload += row.valueJson;
    payload += ",\"quality\":";
    json::appendString(payload, row.quality);
    payload += ",\"ts\":" + std::to_string(row.ts);
    payload += "}";

    m_inflightRows.insert(rowId);
    bool const sent = m_mqtt->publishTracked(
        std::move(topic),
        std::move(payload),
        m_mqttConfig->qos,
        [this, rowId](bool ok) {
            m_inflightRows.erase(rowId);
            if (!ok || !m_persistence) return;
            std::string error;
            if (!m_persistence->markPublished(rowId, error) && !error.empty()) {
                m_logger.logf(log::LogLevel::Error, "persistence", "mark", error);
            }
        });
    if (!sent) m_inflightRows.erase(rowId);
}

void GatewayAssembly::publishMqttSnapshot() {
    if (!m_mqtt) return;
    for (auto const& dp : datapointSnapshots()) {
        publishMqttDatapoint(dp.id, dp.value, dp.state, dp.timestamp);
    }
}

void GatewayAssembly::backfillPersistenceOnce() {
    if (!m_persistence || !m_persistence->enabled()
        || !m_mqtt || !m_mqtt->connected()) {
        return;
    }

    std::string error;
    auto rows = m_persistence->pendingTelemetry(
        m_persistence->config().backfillBatch,
        error);
    if (!error.empty()) {
        m_logger.logf(log::LogLevel::Error, "persistence", "backfill", error);
        return;
    }
    for (auto const& row : rows) {
        publishMqttRow(row);
    }
    if (!m_persistence->prune(error) && !error.empty()) {
        m_logger.logf(log::LogLevel::Warn, "persistence", "prune", error);
    }
}

void GatewayAssembly::startPersistenceBackfillPump() {
    if (!m_persistence || !m_persistence->enabled()) return;
    if (!m_backfillTimer) {
        m_backfillTimer = std::make_unique<gateway_asio::steady_timer>(*m_io);
    }
    m_backfillTimer->expires_after(std::chrono::milliseconds(500));
    m_backfillTimer->async_wait([this](auto const& ec) {
        if (ec || !m_started) return;
        backfillPersistenceOnce();
        startPersistenceBackfillPump();
    });
}

void GatewayAssembly::startMqttSnapshotPump() {
    if (!m_mqttConfig || m_mqttConfig->publishIntervalMs <= 0) return;
    if (!m_mqttSnapshotTimer) {
        m_mqttSnapshotTimer = std::make_unique<gateway_asio::steady_timer>(*m_io);
    }
    m_mqttSnapshotTimer->expires_after(
        std::chrono::milliseconds(m_mqttConfig->publishIntervalMs));
    m_mqttSnapshotTimer->async_wait([this](auto const& ec) {
        if (ec || !m_started) return;
        publishMqttSnapshot();
        startMqttSnapshotPump();
    });
}

bool GatewayAssembly::serverForwardEnabled(std::string const& serverTransportId) const {
    std::lock_guard lk(m_forwardMtx);
    auto it = m_forwardEnabled.find(serverTransportId);
    return it == m_forwardEnabled.end() ? true : it->second;
}

void GatewayAssembly::forwardBridges(bus::ServerWriteEvent const& e) {
    if (e.table != core::RegisterTable::HoldingRegister) return;
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.server != e.transportId) continue;
        auto* sink = m_bridgeFwdSinks[size_t(i)];
        if (!sink) continue;

        int const eventStart = e.startAddress;
        int const eventEnd = e.startAddress + int(e.values.size());
        int const start = std::max(eventStart, bridge.writeStart);
        int const end = std::min(eventEnd, bridge.writeStart + bridge.writeCount);
        for (int address = start; address < end; address++) {
            sink->stageRegister(address - bridge.offset, e.values[size_t(address - eventStart)]);
        }
    }
}

void GatewayAssembly::zeroBridgeForward(std::string const& serverTransportId) {
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.server != serverTransportId) continue;
        auto* sink = m_bridgeFwdSinks[size_t(i)];
        if (!sink) continue;
        for (int address = bridge.writeStart;
             address < bridge.writeStart + bridge.writeCount;
             address++) {
            sink->stageRegister(address - bridge.offset, 0);
        }
    }
}

void GatewayAssembly::mirrorBridgesOnce() {
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.mirrorCount <= 0) continue;
        auto it = m_transports.find(bridge.server);
        if (it == m_transports.end()) continue;
        auto* server = it->second.get();

        // Mirror the RAW holding-register words the PLC last returned — NOT the
        // decoded engineering value. Decoding would corrupt the mirror: a
        // scale=0.1 point reading raw 230 decodes to 23, and a U32 would lose
        // its high word. The operator box reads these as raw registers.
        core::RegisterWords values(size_t(bridge.mirrorCount), 0);
        auto plcIt = m_transports.find(bridge.plc);
        if (plcIt != m_transports.end()) {
            // Any transport that caches raw polled registers (Modbus TCP/RTU,
            // future S7/...) exposes them via RegisterSnapshotSource.
            if (auto* src = dynamic_cast<RegisterSnapshotSource*>(plcIt->second.get())) {
                values = src->snapshotHoldingRegisters(bridge.mirrorStart,
                                                       bridge.mirrorCount);
            }
        }

        transport::WriteBatch batch;
        batch.table = core::RegisterTable::HoldingRegister;
        batch.startAddress = bridge.mirrorStart + bridge.offset;
        batch.values = std::move(values);

        sched::RequestTag tag;
        tag.moduleId = "bridge.mirror." + bridge.server;
        tag.priority = sched::Priority::Low;
        tag.coalesce = true;
        server->scheduler().submitAsync(tag, [server, batch](sched::AsyncDone done) {
            server->writeAsync(batch, [done](transport::WriteResult result) mutable {
                done(result.ok);
            });
        });
    }
}

void GatewayAssembly::wireBindings(module::PollRange& poll,
                                   config::ConfigSchema const& schema,
                                   DpById const& byId,
                                   std::string const& transportId,
                                   transport::ReadRequest const& req) {
    for (auto const& dc : schema.datapoints) {
        if (!dc.hasSource) continue;
        if (dc.source.port != transportId) continue;
        if (tableFromString(dc.source.table) != req.table) continue;

        int const rc = dp::registerCountFor(dc.type);
        int const offset = dc.source.address - req.startAddress;
        if (offset < 0 || offset + rc > req.count) continue;

        auto itDp = byId.find(dc.id);
        if (itDp == byId.end()) continue;

        auto codec = m_codecs.find(dc.source.codec.empty()
            ? codec::BuiltinScalarCodec::idFor(dc.type)
            : dc.source.codec);
        if (!codec) continue;
        poll.bind(itDp->second, std::move(codec), offset);
    }
}

void GatewayAssembly::schedulePoll(PollTimer& pollTimer) {
    if (!m_started || !pollTimer.poll || !pollTimer.timer) return;

    pollTimer.timer->expires_after(std::chrono::milliseconds(pollTimer.poll->periodMs()));
    pollTimer.timer->async_wait([this, &pollTimer](auto const& ec) {
        if (ec || !m_started) return;
        pollTimer.poll->driveTick();
        schedulePoll(pollTimer);
    });
}

void GatewayAssembly::startMirrorPump() {
    int period = 100;
    bool any = false;
    for (auto const& bridge : m_bridges) {
        if (bridge.mirrorCount <= 0) continue;
        any = true;
        period = std::min(period, std::max(20, bridge.mirrorPeriodMs));
    }
    if (!any) return;
    if (!m_mirrorTimer) {
        m_mirrorTimer = std::make_unique<gateway_asio::steady_timer>(*m_io);
    }
    m_mirrorTimer->expires_after(std::chrono::milliseconds(period));
    m_mirrorTimer->async_wait([this, period](auto const& ec) {
        if (ec || !m_started || !m_mirrorTimer) return;
        mirrorBridgesOnce();
        startMirrorPump();
    });
}

void GatewayAssembly::scheduleSink(SinkTimer& sinkTimer) {
    if (!m_started || !sinkTimer.sink || !sinkTimer.timer) return;
    sinkTimer.timer->expires_after(std::chrono::milliseconds(sinkTimer.sink->tickPeriodMs()));
    sinkTimer.timer->async_wait([this, &sinkTimer](auto const& ec) {
        if (ec || !m_started) return;
        sinkTimer.sink->driveTick();
        scheduleSink(sinkTimer);
    });
}

} // namespace core::gateway
