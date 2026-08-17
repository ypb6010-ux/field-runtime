// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "GatewayAssembly.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <array>
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

bool parseQuoted(std::string const& raw, std::string& value) {
    auto const text = trim(raw);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return false;
    }
    value = text.substr(1, text.size() - 2);
    return true;
}

bool parseInteger(std::string const& raw, int& value) {
    auto const text = trim(raw);
    if (text.empty()) return false;
    auto const* begin = text.data();
    auto const* end = begin + text.size();
    auto const result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parseBoolean(std::string const& raw, bool& value) {
    auto const text = trim(raw);
    if (text == "true") {
        value = true;
        return true;
    }
    if (text == "false") {
        value = false;
        return true;
    }
    return false;
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
        auto const rawValue = trimmed.substr(eq + 1);
        if (key == "listen_address") {
            if (!parseQuoted(rawValue, cfg.listenAddress)) {
                error = "control.listen_address must be a string";
                return std::nullopt;
            }
        } else if (key == "listen_port") {
            if (!parseInteger(rawValue, cfg.listenPort)) {
                error = "control.listen_port must be an integer";
                return std::nullopt;
            }
        } else if (key == "auth_token") {
            if (!parseQuoted(rawValue, cfg.authToken)) {
                error = "control.auth_token must be a string";
                return std::nullopt;
            }
        } else {
            error = "control." + key + " is an unknown field";
            return std::nullopt;
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
        auto const rawValue = trimmed.substr(eq + 1);
        bool valid = true;
        if (key == "enable") {
            valid = parseBoolean(rawValue, cfg.enable);
        } else if (key == "host") {
            valid = parseQuoted(rawValue, cfg.host);
        } else if (key == "port") {
            valid = parseInteger(rawValue, cfg.port);
        } else if (key == "client_id") {
            valid = parseQuoted(rawValue, cfg.clientId);
        } else if (key == "keepalive_s") {
            valid = parseInteger(rawValue, cfg.keepaliveS);
        } else if (key == "topic_prefix") {
            valid = parseQuoted(rawValue, cfg.topicPrefix);
        } else if (key == "qos") {
            valid = parseInteger(rawValue, cfg.qos);
        } else if (key == "publish_interval_ms") {
            valid = parseInteger(rawValue, cfg.publishIntervalMs);
        } else if (key == "command_topic_prefix") {
            valid = parseQuoted(rawValue, cfg.commandTopicPrefix);
        } else {
            error = "northbound.mqtt." + key + " is an unknown field";
            return std::nullopt;
        }
        if (!valid) {
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
    if (cfg.clientId.size() > 65535 || cfg.topicPrefix.size() > 65500
        || cfg.commandTopicPrefix.size() > 65500) {
        error = "northbound.mqtt client_id/topic_prefix is too long";
        return std::nullopt;
    }
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
        auto const rawValue = trimmed.substr(eq + 1);
        bool valid = true;
        if (key == "enable") {
            valid = parseBoolean(rawValue, cfg.enable);
        } else if (key == "path") {
            valid = parseQuoted(rawValue, cfg.path);
        } else if (key == "max_rows") {
            valid = parseInteger(rawValue, cfg.maxRows);
        } else if (key == "backfill_batch") {
            valid = parseInteger(rawValue, cfg.backfillBatch);
        } else {
            error = "persistence." + key + " is an unknown field";
            return std::nullopt;
        }
        if (!valid) {
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
    static constexpr std::array<std::string_view, 3> rootExtensions{
        "control", "northbound", "persistence"};
    auto loaded = loader.loadFromToml(tomlPath, rootExtensions);
    if (!loaded.has_value()) {
        for (auto const& err : loaded.error()) {
            std::cerr << err.section << "." << err.field << ": "
                      << err.message << '\n';
            m_logger.logf(log::LogLevel::Error, "config", tomlPath, err.message);
        }
        return false;
    }
    for (auto const& transportConfig : loaded->transports) {
        bool supported = false;
        switch (transportConfig.kind) {
            case transport::TransportKind::ModbusTcpClient:
            case transport::TransportKind::ModbusTcpServer:
            case transport::TransportKind::ModbusRtu:
                supported = true;
                break;
#ifdef FIELDRUNTIME_GATEWAY_HAS_OPCUA
            case transport::TransportKind::OpcUaClient:
                supported = true;
                break;
#endif
#ifdef FIELDRUNTIME_GATEWAY_HAS_S7
            case transport::TransportKind::S7Client:
                supported = true;
                break;
#endif
            default:
                break;
        }
        if (!supported) {
            auto const message =
                "transport kind is unavailable in this gateway build";
            m_logger.logf(
                log::LogLevel::Error,
                "config",
                transportConfig.id,
                message);
            std::cerr << "transport '" << transportConfig.id
                      << "': " << message << '\n';
            return false;
        }
        if (transportConfig.kind
            != transport::TransportKind::ModbusTcpServer) {
            continue;
        }
        for (auto const& range : transportConfig.listenRanges) {
            if (range.table == core::RegisterTable::HoldingRegister
                || range.table == core::RegisterTable::InputRegister) {
                continue;
            }
            auto const message =
                "gateway Modbus server currently supports only HR and IR "
                "listen ranges";
            m_logger.logf(
                log::LogLevel::Error,
                "config",
                transportConfig.id,
                message);
            std::cerr << "transport '" << transportConfig.id
                      << "': " << message << '\n';
            return false;
        }
    }

    if (!loaded->meta.logLevel.empty()) {
        m_logger.setThreshold(log::levelFromString(loaded->meta.logLevel));
    }

    m_configDir = std::filesystem::path(tomlPath).parent_path().string();

    m_drivers.setLogCallback([this](std::string const& driverId, int level,
                                    std::string const& message) {
        auto const mapped = level >= 4 ? log::LogLevel::Error
                          : level >= 3 ? log::LogLevel::Warn
                          : level <= 0 ? log::LogLevel::Debug
                                       : log::LogLevel::Info;
        m_logger.logf(mapped, "driver", driverId, message);
    });
    m_drivers.setDataCallback(
        [this](std::string const& driverId, std::string const& deviceId,
               std::string const& targetId, std::vector<std::uint8_t> payload) {
            auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            {
                std::lock_guard lock(m_driverDataMtx);
                m_driverData[deviceId + "\n" + targetId] =
                    {driverId, deviceId, targetId, payload, now};
            }
            if (m_mqtt && m_mqttConfig) {
                auto topic = m_mqttConfig->topicPrefix + "/device/"
                           + deviceId + "/" + targetId;
                auto body = std::string(payload.begin(), payload.end());
                auto lifetime = m_callbackLifetime;
                gateway_asio::post(*m_io,
                    [this, lifetime, topic = std::move(topic),
                     body = std::move(body)] {
                        if (!lifetime->load(std::memory_order_acquire)) return;
                        if (m_mqtt) m_mqtt->publish(topic, body, m_mqttConfig->qos);
                    });
            }
        });
    std::string driverError;
    if (!m_drivers.load(loaded->drivers, m_configDir, driverError)) {
        m_logger.logf(log::LogLevel::Error, "driver", "load", driverError);
        std::cerr << "driver: " << driverError << '\n';
        return false;
    }

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
        m_mqtt->setMessageCallback(
            [this](std::string topic, std::vector<std::uint8_t> payload) {
                if (!m_mqttConfig || m_mqttConfig->commandTopicPrefix.empty()) return;
                auto prefix = m_mqttConfig->commandTopicPrefix;
                while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
                prefix += '/';
                if (topic.rfind(prefix, 0) != 0) return;
                auto const suffix = topic.substr(prefix.size());
                auto const slash = suffix.find('/');
                if (slash == std::string::npos
                    || suffix.find('/', slash + 1) != std::string::npos) return;
                auto actorId = suffix.substr(0, slash);
                auto targetId = suffix.substr(slash + 1);
                if (actorId.empty() || targetId.empty()) return;
                control::ActorContext actor;
                actor.id = actorId;
                actor.clientId = actorId;
                actor.channel = "mqtt";
                auto ackTopic = m_mqttConfig->topicPrefix + "/control_ack/"
                              + actorId + "/" + targetId;
                if (!writeControlAsync(
                        std::move(actor), targetId, std::move(payload),
                        [this, lifetime = m_callbackLifetime, ackTopic](
                            bool ok, std::string error) mutable {
                            gateway_asio::post(*m_io,
                                [this, lifetime, ackTopic = std::move(ackTopic), ok,
                                 error = std::move(error)] {
                                    if (!lifetime->load(
                                            std::memory_order_acquire)) return;
                                    if (m_mqtt) m_mqtt->publish(
                                        ackTopic, ok ? "ok" : "error:" + error, 1);
                                });
                        })) {
                    m_mqtt->publish(ackTopic, "error:unknown control target", 1);
                }
            });
    }
    if (m_persistenceConfig) {
        auto persistencePath =
            std::filesystem::path(m_persistenceConfig->path);
        if (persistencePath.is_relative()) {
            persistencePath =
                std::filesystem::path(m_configDir) / persistencePath;
            m_persistenceConfig->path =
                std::filesystem::absolute(persistencePath).lexically_normal()
                    .string();
        }
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
    m_callbackLifetime->store(true, std::memory_order_release);
    m_started = true;
    installEventWiring();
    m_drivers.start();

    if (m_mqtt) {
        m_mqtt->start();
        m_logger.logf(log::LogLevel::Info,
                      "gateway",
                      "mqtt",
                      "northbound connecting "
                          + m_mqttConfig->host + ":"
                          + std::to_string(m_mqttConfig->port));
    }

    m_connectThreads.clear();
    for (auto& [id, transport] : m_transports) {
        auto const kind = transport->kind();
        if (kind == transport::TransportKind::ModbusTcpClient) {
            // The Asio TCP client owns a fully asynchronous resolver/connect
            // path. Use it for startup too, otherwise the synchronous
            // connect() can park daemon startup on an unreachable endpoint.
            transport->requestReconnect();
            m_logger.logf(
                log::LogLevel::Info, "transport", id, "connecting");
            continue;
        }
        if (kind != transport::TransportKind::OpcUaClient
            && kind != transport::TransportKind::S7Client) {
            auto connected = transport->connect();
            if (!connected.has_value()) {
                m_logger.logf(
                    log::LogLevel::Error,
                    "transport",
                    id,
                    connected.error());
                if (kind == transport::TransportKind::ModbusTcpServer) {
                    throw std::runtime_error(
                        "server transport '" + id
                        + "' failed to start: " + connected.error());
                }
            } else {
                m_bus.publish(bus::TransportEvent{
                    id, bus::TransportEventKind::Connected, {}});
                m_logger.logf(
                    log::LogLevel::Info, "transport", id, "connected");
            }
            continue;
        }
        auto* transportPtr = transport.get();
        m_connectThreads.emplace_back([this, id, transportPtr] {
            auto connected = transportPtr->connect();
            if (!m_started.load(std::memory_order_acquire)) return;
            if (connected.has_value()) {
                m_bus.publish(bus::TransportEvent{
                    id, bus::TransportEventKind::Connected, {}});
                m_logger.logf(
                    log::LogLevel::Info, "transport", id, "connected");
            } else {
                m_logger.logf(
                    log::LogLevel::Error,
                    "transport",
                    id,
                    connected.error());
            }
        });
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
    m_callbackLifetime->store(false, std::memory_order_release);
    m_drivers.stop();

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
    for (auto& thread : m_connectThreads) {
        if (thread.joinable()) thread.join();
    }
    m_connectThreads.clear();
    for (auto& [id, transport] : m_transports) {
        transport->disconnect();
        m_bus.publish(bus::TransportEvent{id, bus::TransportEventKind::Disconnected, {}});
    }
    m_serverWriteSub.reset();
    m_pollRangeCompletedSub.reset();
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

bool GatewayAssembly::reconnectTransport(std::string const& transportId) {
    auto it = m_transports.find(transportId);
    if (it == m_transports.end()) return false;
    // Async, non-blocking, scheduler-preserving (see Transport::requestReconnect).
    // Avoid disconnect()+connect() here: disconnect() stops the async scheduler
    // and connect() would block the single io thread on an unreachable PLC.
    it->second->requestReconnect();
    m_logger.logf(log::LogLevel::Info, "transport", transportId, "reconnect requested");
    return true;
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
    configureControl(schema);
    registerCodecs(schema);
    buildTransports(schema);
    auto byId = buildDatapoints(schema);
    buildPollRanges(schema, byId);
    buildSinkWindows(schema);
    buildBridges(schema);
}

void GatewayAssembly::configureControl(config::ConfigSchema const& schema) {
    m_actors = schema.actors;
    m_deviceRouteConfigs = schema.deviceRoutes;
    m_controlTargets = schema.controlTargets;
    m_controlArbiter.clearPolicies();
    m_controlArbiter.clearLeases();
    m_controlArbiter.setDefaultPolicy(
        {"default", control::PolicyMode::Open, 0, 0});
    for (auto const& policy : schema.controlPolicies) {
        m_controlArbiter.setPolicy(
            policy.targetId,
            {policy.id, policy.mode, policy.leaseMs, policy.minPriority});
    }

    std::vector<control::DeviceRoute> routes;
    routes.reserve(schema.deviceRoutes.size());
    for (auto const& route : schema.deviceRoutes) {
        routes.push_back({route.id, route.deviceId, route.driverId,
                          route.transportId, route.protocol,
                          route.writable, route.active});
    }
    std::string error;
    if (!m_deviceRoutes.configure(std::move(routes), error)) {
        throw std::logic_error("invalid device routes: " + error);
    }
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
            auto server = std::make_unique<AsioModbusTcpServer>(tc, *m_io, m_bus);
            server->setWriteAuthorizer(
                [this](bus::ServerWriteEvent const& event, std::string& error) {
                    return authorizeServerWrite(event, error);
                });
            transport = std::move(server);
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
            throw std::logic_error(
                "unsupported gateway transport reached assembly: " + tc.id);
        }
        m_transports.emplace(tc.id, std::move(transport));
    }
}

std::vector<control::DeviceRoute> GatewayAssembly::deviceRoutes() const {
    return m_deviceRoutes.routes();
}

std::vector<DriverSnapshot> GatewayAssembly::driverSnapshots() const {
    return m_drivers.snapshots();
}

std::vector<GatewayDriverDataSnapshot>
GatewayAssembly::driverDataSnapshots() const {
    std::lock_guard lock(m_driverDataMtx);
    std::vector<GatewayDriverDataSnapshot> out;
    out.reserve(m_driverData.size());
    for (auto const& [key, value] : m_driverData) {
        (void)key;
        out.push_back(value);
    }
    return out;
}

bool GatewayAssembly::writeControlAsync(
    control::ActorContext actor,
    std::string const& targetId,
    std::vector<std::uint8_t> payload,
    std::function<void(bool, std::string)> done) {
    for (auto const& configured : m_actors) {
        if (!configured.enabled || configured.channel != actor.channel) continue;
        if (!configured.clientId.empty()
            && configured.clientId != actor.clientId) continue;
        if (!configured.sourceAddress.empty()
            && configured.sourceAddress != actor.sourceAddress) continue;
        actor.id = configured.id;
        actor.role = configured.role;
        actor.priority = configured.priority;
        break;
    }
    auto const targetConfig = std::find_if(
        m_controlTargets.begin(), m_controlTargets.end(),
        [&](auto const& target) { return target.id == targetId; });
    if (targetConfig == m_controlTargets.end()) return false;
    auto const active = m_deviceRoutes.activeRoute(targetConfig->deviceId);
    if (!active) {
        if (done) done(false, "device has no active write route");
        return true;
    }
    if (!targetConfig->routeId.empty() && targetConfig->routeId != active->id) {
        if (done) done(false, "target is not on the active device route");
        return true;
    }
    auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    control::ControlTarget target{targetConfig->id, targetConfig->deviceId,
                                  active->id, targetConfig->address};
    auto decision = m_controlArbiter.authorize(
        {"", std::move(actor), std::move(target), now, 0}, now);
    if (!decision.allowed) {
        if (done) done(false, decision.reason);
        return true;
    }

    if (!active->driverId.empty()) {
        auto completion = std::make_shared<
            std::function<void(bool, std::string)>>(std::move(done));
        if (!m_drivers.write(active->driverId, active->deviceId, targetId,
                             std::move(payload),
                             [completion](bool ok, std::string error) {
                                 if (*completion) (*completion)(ok, std::move(error));
                             })) {
            if (*completion) (*completion)(false, "driver is not loaded");
        }
        return true;
    }
    if (active->protocol == "mqtt") {
        if (!m_mqtt || !m_mqttConfig) {
            if (done) done(false, "MQTT northbound is not configured");
            return true;
        }
        m_mqtt->publish(targetConfig->address.resource,
                        std::string(payload.begin(), payload.end()),
                        m_mqttConfig->qos);
        if (done) done(true, {});
        return true;
    }
    if (payload.empty() || payload.size() % 2 != 0) {
        if (done) done(false, "register target payload must contain big-endian words");
        return true;
    }
    if (payload.size() / 2
        > std::size_t(std::max<std::int64_t>(1, targetConfig->address.width))) {
        if (done) done(false, "register payload exceeds control target width");
        return true;
    }
    transport::WriteBatch batch;
    batch.table = core::RegisterTable::HoldingRegister;
    batch.startAddress = int(targetConfig->address.offset);
    for (std::size_t i = 0; i < payload.size(); i += 2) {
        batch.values.push_back(std::uint16_t(
            (std::uint16_t(payload[i]) << 8) | payload[i + 1]));
    }
    auto completion = std::make_shared<
        std::function<void(bool, std::string)>>(std::move(done));
    if (!writeTransportAsync(active->transportId, std::move(batch),
        [completion](transport::WriteResult result) {
            if (*completion) {
                (*completion)(result.ok, std::move(result.errorMessage));
            }
        })) {
        if (*completion) (*completion)(false, "active transport is unavailable");
    }
    return true;
}

bool GatewayAssembly::writeRegisterControlAsync(
    control::ActorContext actor,
    std::string const& transportId,
    int startAddress,
    core::RegisterWords values,
    std::function<void(bool, std::string)> done) {
    std::vector<std::string> candidates;
    for (auto const& route : m_deviceRouteConfigs) {
        if (route.transportId != transportId
            || !m_deviceRoutes.isActive(route.deviceId, route.id)) continue;
        for (auto const& target : m_controlTargets) {
            if (target.deviceId != route.deviceId
                || (!target.routeId.empty() && target.routeId != route.id)
                || target.address.protocol != route.protocol
                || (target.address.resource != "HR"
                    && target.address.resource != "HoldingRegisters")
                || target.address.offset != startAddress
                || target.address.width < std::int64_t(values.size())) continue;
            candidates.push_back(target.id);
        }
    }
    if (candidates.size() != 1) {
        if (done) done(false, candidates.empty()
            ? "no active control target covers the register write"
            : "register write matches multiple control targets");
        return true;
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(values.size() * 2);
    for (auto value : values) {
        payload.push_back(std::uint8_t(value >> 8));
        payload.push_back(std::uint8_t(value & 0xFF));
    }
    return writeControlAsync(std::move(actor), candidates.front(),
                             std::move(payload), std::move(done));
}

std::vector<control::LeaseSnapshot> GatewayAssembly::controlLeases() const {
    auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return m_controlArbiter.leases(now);
}

bool GatewayAssembly::setActiveDeviceRoute(std::string const& deviceId,
                                           std::string const& routeId,
                                           std::string& error) {
    if (!m_deviceRoutes.setActive(deviceId, routeId, error)) return false;
    for (auto const& target : m_controlTargets) {
        if (target.deviceId == deviceId) m_controlArbiter.releaseTarget(target.id);
    }
    m_logger.logf(log::LogLevel::Info, "control", deviceId,
                  "active write route changed to " + routeId);
    return true;
}

bool GatewayAssembly::authorizeServerWrite(
    bus::ServerWriteEvent const& event,
    std::string& error) {
    control::ActorContext actor;
    actor.id = event.sessionId.empty()
        ? "modbus:" + event.sourceAddress : event.sessionId;
    actor.sessionId = event.sessionId;
    actor.sourceAddress = event.sourceAddress;
    actor.clientId = event.transportId;
    actor.channel = "modbus";
    for (auto const& configured : m_actors) {
        if (!configured.enabled || configured.channel != "modbus") continue;
        if (!configured.clientId.empty()
            && configured.clientId != event.transportId) continue;
        if (!configured.sourceAddress.empty()
            && configured.sourceAddress != event.sourceAddress) continue;
        actor.id = configured.id;
        actor.role = configured.role;
        actor.priority = configured.priority;
        break;
    }

    auto const eventEnd = event.startAddress + int(event.values.size());
    for (auto const& bridge : m_bridges) {
        if (bridge.server != event.transportId) continue;
        auto const overlapStart = std::max(event.startAddress, bridge.writeStart);
        auto const overlapEnd = std::min(eventEnd,
                                         bridge.writeStart + bridge.writeCount);
        if (overlapStart >= overlapEnd) continue;
        auto const mappedStart = overlapStart - bridge.offset;
        auto const mappedWidth = overlapEnd - overlapStart;

        for (auto const& route : m_deviceRouteConfigs) {
            if (route.transportId != bridge.plc) continue;
            bool routeTargetMatched = false;
            for (auto const& configured : m_controlTargets) {
                if (configured.deviceId != route.deviceId) continue;
                if (!configured.routeId.empty()
                    && configured.routeId != route.id) continue;
                if (configured.address.protocol != "modbus") continue;
                if (configured.address.resource != "HR"
                    && configured.address.resource != "HoldingRegisters") continue;
                control::ControlAddress incoming = configured.address;
                incoming.offset = mappedStart;
                incoming.width = mappedWidth;
                incoming.mask = ~std::uint64_t{0};
                if (!configured.address.conflictsWith(incoming)) continue;
                routeTargetMatched = true;
                if (!m_deviceRoutes.isActive(route.deviceId, route.id)) {
                    error = "device route '" + route.id + "' is not active";
                    return false;
                }

                control::ControlTarget target{
                    configured.id, configured.deviceId, route.id,
                    configured.address};
                auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                auto decision = m_controlArbiter.authorize(
                    {event.sessionId, actor, std::move(target), now, 0}, now);
                if (!decision.allowed) {
                    error = decision.reason;
                    m_logger.logf(log::LogLevel::Warn, "control", configured.id,
                                  "write denied for actor " + actor.id + ": "
                                      + decision.reason);
                    return false;
                }
            }
            if (!routeTargetMatched) {
                error = "no control target covers the device write";
                return false;
            }
        }
    }
    return true;
}

namespace {
// Build a disconnect/reset value matching the datapoint's natural scalar type.
// Quality goes Error regardless, so the exact numeric subtype is cosmetic.
dp::Value makeDisconnectValue(dp::ScalarType type, double dv) {
    switch (type) {
        case dp::ScalarType::Bool:   return dv != 0.0;
        case dp::ScalarType::F32:
        case dp::ScalarType::F64:    return dv;
        case dp::ScalarType::S16:
        case dp::ScalarType::S32:
        case dp::ScalarType::S64:    return std::int64_t(dv);
        case dp::ScalarType::String: return std::string();
        default:                     return std::uint64_t(dv);  // U16/U32/U64/EnumU16
    }
}
} // namespace

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
        // Disconnect policy (#5): "reset" zeros to disconnect_value on source
        // drop; "hold" keeps the last value. Only polled (source) datapoints.
        if (dc.hasSource && dc.onDisconnect != "hold") {
            datapoint->setDisconnectValue(
                makeDisconnectValue(dc.type, dc.disconnectValue));
        }
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
            pc.moduleId, *it->second, req, pc.periodMs, pc.priority, &m_bus);
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
    m_bridgeMirrorStates.clear();
    m_bridgeMirrorStates.reserve(m_bridges.size());
    for (size_t i = 0; i < m_bridges.size(); ++i) {
        m_bridgeMirrorStates.push_back(
            std::make_shared<BridgeMirrorState>());
    }
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
    m_pollRangeCompletedSub = std::make_unique<bus::Subscription>(
        m_bus.subscribe<bus::PollRangeCompleted>(
            [this](bus::PollRangeCompleted const& e) {
                onPollRangeCompleted(e);
            }));
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
        sink->forceFlush();
    }
}

void GatewayAssembly::onPollRangeCompleted(
    bus::PollRangeCompleted const& event) {
    if (event.table != core::RegisterTable::HoldingRegister) return;
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.mirrorCount <= 0
            || bridge.plc != event.transportId) {
            continue;
        }
        int const offset = bridge.mirrorStart - event.startAddress;
        if (offset < 0
            || offset + bridge.mirrorCount > int(event.values.size())) {
            continue;
        }
        auto const state = m_bridgeMirrorStates[size_t(i)];
        {
            std::lock_guard lock(state->mutex);
            state->values.assign(event.values.begin() + offset,
                                 event.values.begin() + offset
                                     + bridge.mirrorCount);
            ++state->version;
        }
        if (bridge.mirrorPolicy == config::BridgeMirrorPolicy::AfterPoll) {
            scheduleBridgeMirror(i);
        }
    }
}

void GatewayAssembly::scheduleBridgeMirror(int index) {
    if (index < 0 || index >= int(m_bridges.size())) return;
    auto const& bridge = m_bridges[size_t(index)];
    if (bridge.mirrorCount <= 0) return;
    auto it = m_transports.find(bridge.server);
    if (it == m_transports.end()) return;
    auto* server = it->second.get();
    auto const state = m_bridgeMirrorStates[size_t(index)];

    core::RegisterWords values;
    std::uint64_t version = 0;
    {
        std::lock_guard lock(state->mutex);
        if (state->inFlight
            || int(state->values.size()) != bridge.mirrorCount) {
            return;
        }
        state->inFlight = true;
        values = state->values;
        version = state->version;
    }

    transport::WriteBatch batch;
    batch.table = core::RegisterTable::HoldingRegister;
    batch.startAddress = bridge.mirrorStart + bridge.offset;
    batch.values = std::move(values);

    sched::RequestTag tag;
    tag.moduleId = "bridge.mirror." + bridge.server + "."
                 + std::to_string(index);
    tag.priority = sched::Priority::Low;
    auto const submitted = server->scheduler().submitAsync(
        tag,
        [this, index, server, batch = std::move(batch), state, version](
            sched::AsyncDone done) mutable {
            try {
                server->writeAsync(
                    batch,
                    [this, index, state, version,
                     done = std::move(done)](
                        transport::WriteResult result) mutable {
                        bool repeat = false;
                        {
                            std::lock_guard lock(state->mutex);
                            state->inFlight = false;
                            repeat = state->version > version;
                        }
                        done(result.ok);
                        if (repeat && m_started
                            && index < int(m_bridges.size())
                            && m_bridges[size_t(index)].mirrorPolicy
                                == config::BridgeMirrorPolicy::AfterPoll) {
                            scheduleBridgeMirror(index);
                        }
                    });
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->inFlight = false;
                throw;
            }
        });
    if (submitted.kind == sched::ResultKind::Ok) {
        std::lock_guard lock(state->mutex);
        state->lastSubmittedAt = std::chrono::steady_clock::now();
    } else {
        std::lock_guard lock(state->mutex);
        state->inFlight = false;
    }
}

void GatewayAssembly::mirrorBridgesPeriodically() {
    auto const now = std::chrono::steady_clock::now();
    for (int i = 0; i < int(m_bridges.size()); i++) {
        auto const& bridge = m_bridges[size_t(i)];
        if (bridge.mirrorCount <= 0
            || bridge.mirrorPolicy
                != config::BridgeMirrorPolicy::Periodic) {
            continue;
        }
        auto const state = m_bridgeMirrorStates[size_t(i)];
        {
            std::lock_guard lock(state->mutex);
            if (state->lastSubmittedAt.time_since_epoch().count() > 0
                && now - state->lastSubmittedAt
                    < std::chrono::milliseconds(bridge.mirrorPeriodMs)) {
                continue;
            }
        }
        scheduleBridgeMirror(i);
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
    int period = std::numeric_limits<int>::max();
    bool any = false;
    for (auto const& bridge : m_bridges) {
        if (bridge.mirrorCount <= 0
            || bridge.mirrorPolicy
                != config::BridgeMirrorPolicy::Periodic) {
            continue;
        }
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
        mirrorBridgesPeriodically();
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
