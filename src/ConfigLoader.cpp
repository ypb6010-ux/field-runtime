// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/config/ConfigLoader.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include <toml++/toml.hpp>

#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"
#include "core/sched/SchedulerTypes.h"

namespace core::config {

namespace {

std::string idx(std::string_view base, int index) {
    return std::string(base) + '[' + std::to_string(index) + ']';
}

std::string toHexLower(uint64_t v) {
    std::ostringstream os;
    os << std::hex << v;
    return os.str();
}

std::optional<std::string> getStr(toml::table const& t, std::string_view key) {
    return t[key].value<std::string>();
}

std::string getStr(toml::table const& t, std::string_view key, std::string const& dflt) {
    return t[key].value<std::string>().value_or(dflt);
}

std::optional<int> getInt(toml::table const& t, std::string_view key) {
    if (auto v = t[key].value<int64_t>()) return int(*v);
    return std::nullopt;
}

int getInt(toml::table const& t, std::string_view key, int dflt) {
    return getInt(t, key).value_or(dflt);
}

std::optional<double> getDouble(toml::table const& t, std::string_view key) {
    if (auto d = t[key].value<double>()) return *d;
    if (auto i = t[key].value<int64_t>()) return double(*i);
    return std::nullopt;
}

double getDouble(toml::table const& t, std::string_view key, double dflt) {
    return getDouble(t, key).value_or(dflt);
}

std::optional<bool> getBool(toml::table const& t, std::string_view key) {
    if (auto b = t[key].value<bool>()) return *b;
    return std::nullopt;
}

void requireStr(toml::table const& t, std::string_view key,
                 std::string const& section, std::string& out,
                 ValidationErrors& errs) {
    auto v = getStr(t, key);
    if (!v) {
        errs.push_back({section, std::string(key),
                        "missing required string field",
                        int(t.source().begin.line)});
        return;
    }
    out = *v;
}

dp::ScalarType parseScalarType(std::string const& s, bool& ok) {
    ok = true;
    static const std::unordered_map<std::string, dp::ScalarType> map = {
        {"Bool",    dp::ScalarType::Bool},
        {"U16",     dp::ScalarType::U16},
        {"S16",     dp::ScalarType::S16},
        {"U32",     dp::ScalarType::U32},
        {"S32",     dp::ScalarType::S32},
        {"F32",     dp::ScalarType::F32},
        {"U64",     dp::ScalarType::U64},
        {"S64",     dp::ScalarType::S64},
        {"F64",     dp::ScalarType::F64},
        {"EnumU16", dp::ScalarType::EnumU16},
        {"String",  dp::ScalarType::String},
    };
    auto it = map.find(s);
    if (it == map.end()) { ok = false; return dp::ScalarType::U16; }
    return it->second;
}

dp::WordOrder parseWordOrder(std::string const& s, bool& ok) {
    ok = true;
    if (s.empty() || s == "ABCD") return dp::WordOrder::ABCD;
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    ok = false;
    return dp::WordOrder::ABCD;
}

sched::Priority parsePriority(std::string const& s) {
    if (s == "Low")      return sched::Priority::Low;
    if (s == "High")     return sched::Priority::High;
    if (s == "Critical") return sched::Priority::Critical;
    return sched::Priority::Normal;
}

sched::SchedulerKind parseSchedulerKind(std::string const& s) {
    if (s == "credit")   return sched::SchedulerKind::Credit;
    if (s == "priority") return sched::SchedulerKind::Priority;
    return sched::SchedulerKind::Serial;
}

transport::TransportKind parseTransportKind(std::string const& s, bool& ok) {
    ok = true;
    if (s == "modbus_tcp_client") return transport::TransportKind::ModbusTcpClient;
    if (s == "modbus_tcp_server") return transport::TransportKind::ModbusTcpServer;
    if (s == "modbus_rtu")        return transport::TransportKind::ModbusRtu;
    if (s == "opc_ua_client")     return transport::TransportKind::OpcUaClient;
    // `mqtt_client` is the back-compat alias for the Qt6::Mqtt backend.
    if (s == "mqtt_client")       return transport::TransportKind::MqttClient;
    if (s == "mqtt_qt_client")    return transport::TransportKind::MqttClient;
    if (s == "mqtt_paho_client")  return transport::TransportKind::MqttPahoClient;
    if (s == "s7_client")         return transport::TransportKind::S7Client;
    ok = false;
    return transport::TransportKind::ModbusTcpClient;
}

// ─── Section parsers ────────────────────────────────────────────────────

MetaConfig parseMeta(toml::table const& root, ValidationErrors& /*errs*/) {
    MetaConfig m;
    if (auto t = root["meta"].as_table()) {
        m.project   = getStr(*t, "project",   {});
        m.version   = getStr(*t, "version",   {});
        m.generated = getStr(*t, "generated", {});
        m.logLevel  = getStr(*t, "log_level", {});
    }
    return m;
}

TransportConfig parseTransport(toml::table const& t,
                                int                index,
                                ValidationErrors&   errs) {
    auto const section = idx("transport", index);
    TransportConfig c;
    requireStr(t, "id",   section, c.id,   errs);

    std::string kindStr;
    requireStr(t, "kind", section, kindStr, errs);
    bool ok = true;
    c.kind = parseTransportKind(kindStr, ok);
    if (!ok && !kindStr.empty()) {
        errs.push_back({section, "kind",
                        "unknown transport kind '" + kindStr + "'",
                        int(t.source().begin.line)});
    }

    c.host           = getStr(t, "host", {});
    c.port           = uint16_t(getInt(t, "port", 502));
    c.slaveId        = getInt(t, "slave_id", 1);
    c.listenAddress  = getStr(t, "listen_address", "0.0.0.0");
    c.listenPort     = getInt(t, "listen_port", 502);
    c.maxClients     = getInt(t, "max_clients", 1);
    // Modbus RTU
    c.portName       = getStr(t, "port_name",  {});
    c.baudRate       = getInt(t, "baud_rate",  9600);
    c.dataBits       = getInt(t, "data_bits",  8);
    c.stopBits       = getInt(t, "stop_bits",  1);
    c.parity         = getStr(t, "parity",     "none");
    // OPC UA
    c.endpointUrl    = getStr(t, "endpoint_url",    {});
    c.securityPolicy = getStr(t, "security_policy", "None");
    c.username       = getStr(t, "username",        {});
    c.password       = getStr(t, "password",        {});
    c.opcuaBackend   = getStr(t, "backend",         "open62541");
    c.nodeIdTemplate = getStr(t, "node_id_template", "ns=2;s=Var_%1");
    // MQTT
    c.clientId       = getStr(t, "client_id",     {});
    c.brokerUri      = getStr(t, "broker_uri",    {});
    c.topicPrefix    = getStr(t, "topic_prefix",  {});
    c.topicTemplate  = getStr(t, "topic_template", "reg/%1");
    c.qos            = getInt(t, "qos",           1);
    c.cleanSession   = getBool(t, "clean_session").value_or(true);
    // S7
    c.rack           = getInt(t, "rack", 0);
    c.slot           = getInt(t, "slot", 1);
    c.s7Db           = getInt(t, "db", 1);
    // Common
    c.reconnectIntervalMs = getInt(t, "reconnect_interval_ms", 15000);
    c.connectTimeoutMs    = getInt(t, "connect_timeout_ms",    3000);
    c.requestTimeoutMs    = getInt(t, "request_timeout_ms",    1000);

    if (auto s = t["scheduler"].as_table()) {
        c.scheduler.kind                    = parseSchedulerKind(getStr(*s, "kind", "serial"));
        c.scheduler.defaultTimeoutMs        = getInt(*s, "default_timeout_ms", 1000);
        c.scheduler.interRequestGapMs       = getInt(*s, "inter_request_gap_ms", 0);
        c.scheduler.maxQueueDepth           = getInt(*s, "max_queue_depth", 256);
        c.scheduler.maxInflight             = getInt(*s, "max_inflight", 1);
        c.scheduler.starvationGuardMs       = getInt(*s, "starvation_guard_ms", 5000);
        c.scheduler.fifoWithinLane          = getBool(*s, "fifo_within_lane").value_or(true);
        c.scheduler.circuitBreakerThreshold = getInt(*s, "circuit_breaker_threshold", 10);
        c.scheduler.circuitBreakerOpenMs    = getInt(*s, "circuit_breaker_open_ms", 5000);
    }
    if (auto lr = t["listen_ranges"].as_array()) {
        for (auto&& n : *lr) {
            if (auto wt = n.as_table()) {
                transport::WatchRange r;
                std::string const tbl = getStr(*wt, "table", "HR");
                if (tbl == "HR" || tbl == "HoldingRegisters")
                    r.table = core::RegisterTable::HoldingRegister;
                else if (tbl == "IR" || tbl == "InputRegisters")
                    r.table = core::RegisterTable::InputRegister;
                else if (tbl == "Coil" || tbl == "Coils")
                    r.table = core::RegisterTable::Coil;
                else if (tbl == "DI" || tbl == "DiscreteInputs")
                    r.table = core::RegisterTable::DiscreteInput;
                else
                    r.table = core::RegisterTable::HoldingRegister;
                if (auto arr = (*wt)["range"].as_array(); arr && arr->size() == 2) {
                    r.startAddress = int(arr->at(0).value<int64_t>().value_or(0));
                    r.size         = int(arr->at(1).value<int64_t>().value_or(0));
                }
                c.listenRanges.push_back(r);
            }
        }
    }
    return c;
}

CodecConfig parseCodec(toml::table const& t,
                        int                index,
                        ValidationErrors&   errs) {
    auto const section = idx("codec", index);
    CodecConfig c;
    requireStr(t, "id",   section, c.id,   errs);
    requireStr(t, "kind", section, c.kind, errs);
    c.script = getStr(t, "script", {});
    c.arg    = getStr(t, "arg", {});
    if (auto m = t["map"].as_table()) {
        for (auto&& [k, v] : *m) {
            dp::Value val;
            if (auto sv = v.value<std::string>())   val = *sv;
            else if (auto iv = v.value<int64_t>())  val = std::int64_t(*iv);
            else if (auto bv = v.value<bool>())     val = *bv;
            c.map.emplace(std::string(k.str()), std::move(val));
        }
    }
    return c;
}

PollRangeConfig parsePollRange(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = idx("poll_range", index);
    PollRangeConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        c.startAddress = int(arr->at(0).value<int64_t>().value_or(0));
        c.count        = int(arr->at(1).value<int64_t>().value_or(0));
    } else {
        errs.push_back({section, "range",
                        "expected [start, count] array",
                        int(t.source().begin.line)});
    }
    c.periodMs = getInt(t, "period_ms", 0);
    if (c.periodMs <= 0) {
        errs.push_back({section, "period_ms",
                        "period_ms must be > 0",
                        int(t.source().begin.line)});
    }
    c.priority = parsePriority(getStr(t, "priority", "Normal"));
    return c;
}

core::RegisterWords parseU16Array(toml::array const& arr) {
    core::RegisterWords out;
    for (auto&& n : arr) {
        if (auto i = n.value<int64_t>()) out.push_back(uint16_t(*i));
    }
    return out;
}

SinkWindowConfig parseSinkWindow(toml::table const& t,
                                   int                index,
                                   ValidationErrors&   errs) {
    auto const section = idx("sink_window", index);
    SinkWindowConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        c.startAddress = int(arr->at(0).value<int64_t>().value_or(0));
        c.size         = int(arr->at(1).value<int64_t>().value_or(0));
    } else {
        errs.push_back({section, "range",
                        "expected [start, size] array",
                        int(t.source().begin.line)});
    }
    if (c.size <= 0) {
        errs.push_back({section, "range",
                        "size must be > 0",
                        int(t.source().begin.line)});
    }
    c.priority = parsePriority(getStr(t, "priority", "High"));
    if (auto f = t["flush"].as_table()) {
        c.flush.debounceMs     = getInt(*f, "debounce_ms", 20);
        c.flush.keepaliveMs    = getInt(*f, "keepalive_ms", 0);
        c.flush.coalesceWrites = getBool(*f, "coalesce").value_or(true);
        c.flush.maxRetries     = getInt(*f, "max_retries", 2);
    }
    if (auto arr = t["initial"].as_array()) {
        c.initial = parseU16Array(*arr);
    }
    return c;
}

HeartbeatConfig parseHeartbeat(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = idx("heartbeat", index);
    HeartbeatConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    c.table       = getStr(t, "table", "HR");
    c.address     = getInt(t, "address", 0);
    if (auto arr = t["values"].as_array()) {
        c.values = parseU16Array(*arr);
    } else if (auto v = getInt(t, "value")) {
        c.values.push_back(uint16_t(*v));
    }
    if (c.values.empty()) {
        errs.push_back({section, "values",
                        "heartbeat requires non-empty values",
                        int(t.source().begin.line)});
    }
    c.periodMs    = getInt(t, "period_ms", 0);
    if (c.periodMs <= 0) {
        errs.push_back({section, "period_ms",
                        "period_ms must be > 0",
                        int(t.source().begin.line)});
    }
    c.priority    = parsePriority(getStr(t, "priority", "Low"));
    c.incrementer = getStr(t, "incrementer", "none");
    return c;
}

AckWatchConfig parseAckWatch(toml::table const& t,
                               int                index,
                               ValidationErrors&   errs) {
    auto const section = idx("ack_watch", index);
    AckWatchConfig c;
    requireStr(t, "module_id", section, c.moduleId, errs);
    requireStr(t, "dp",        section, c.dp,       errs);
    c.timeoutMs = getInt(t, "timeout_ms", 3000);
    if (auto v = t["expected"]; v) {
        if (auto i = v.value<int64_t>())          c.expected = std::int64_t(*i);
        else if (auto b = v.value<bool>())        c.expected = *b;
        else if (auto s = v.value<std::string>()) c.expected = *s;
        else if (auto d = v.value<double>())      c.expected = *d;
    }
    return c;
}

CommandConfig parseCommand(toml::table const& t,
                             int                index,
                             ValidationErrors&   errs) {
    auto const section = idx("command", index);
    CommandConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    c.priority      = parsePriority(getStr(t, "priority", "High"));
    c.interruptable = getBool(t, "interruptable").value_or(false);
    c.trigger       = getStr(t, "trigger", {});
    if (auto arr = t["writes"].as_array()) {
        for (auto&& n : *arr) {
            if (auto wt = n.as_table()) {
                CommandWriteEntry e;
                e.table   = getStr(*wt, "table", "HR");
                e.address = getInt(*wt, "address", 0);
                e.value   = uint16_t(getInt(*wt, "value", 0));
                c.writes.push_back(e);
            }
        }
    }
    if (c.writes.empty()) {
        errs.push_back({section, "writes",
                        "command requires non-empty writes",
                        int(t.source().begin.line)});
    }
    return c;
}

RouteConfig parseRoute(toml::table const& t,
                        int                index,
                        ValidationErrors&   errs) {
    auto const section = idx("route", index);
    RouteConfig c;
    c.name   = getStr(t, "name", {});
    requireStr(t, "from", section, c.from, errs);
    requireStr(t, "to",   section, c.to,   errs);
    c.policy = getStr(t, "policy", "ContinuousMirror");
    return c;
}

BridgeConfig parseBridge(toml::table const& t,
                          int                index,
                          ValidationErrors&   errs) {
    auto const section = idx("bridge", index);
    BridgeConfig c;
    requireStr(t, "server", section, c.server, errs);
    requireStr(t, "plc",    section, c.plc,    errs);
    c.offset         = getInt(t, "offset", 0);
    c.writeStart     = getInt(t, "write_start", 0);
    c.writeCount     = getInt(t, "write_count", 0);
    c.mirrorStart    = getInt(t, "mirror_start", 0);
    c.mirrorCount    = getInt(t, "mirror_count", 0);
    c.mirrorPeriodMs = getInt(t, "mirror_period_ms", 100);
    return c;
}

PluginConfig parsePlugin(toml::table const& t,
                          int                index,
                          ValidationErrors&   errs) {
    auto const section = idx("plugin", index);
    PluginConfig c;
    requireStr(t, "dll", section, c.dllPath, errs);
    c.name   = getStr(t, "name",   {});
    c.config = getStr(t, "config", {});
    return c;
}

PortRefConfig parsePortRef(toml::table const& t,
                             std::string const& section,
                             ValidationErrors&   errs) {
    PortRefConfig p;
    p.port    = getStr(t, "port",    {});
    p.table   = getStr(t, "table",   {});
    p.address = getInt(t, "addr",    0);
    p.bit     = getInt(t, "bit",     -1);
    p.wordOrder = getStr(t, "wordOrder", {});
    p.shift   = getInt(t, "shift",   0);
    if (auto m = getInt(t, "mask")) p.mask = uint64_t(*m);
    p.scale   = getDouble(t, "scale", 1.0);
    p.offset  = getDouble(t, "offset", 0.0);
    p.codec   = getStr(t, "codec", {});
    p.dedupe  = getStr(t, "dedupe", "none");
    p.window  = getStr(t, "window", {});

    (void)section; (void)errs;
    return p;
}

DatapointConfig parseDatapoint(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = idx("datapoint", index);
    DatapointConfig c;
    requireStr(t, "id",   section, c.id,   errs);
    c.kind = getStr(t, "kind", "Status");
    std::string typeStr = getStr(t, "type", "U16");
    bool typeOk = true;
    c.type = parseScalarType(typeStr, typeOk);
    if (!typeOk) {
        errs.push_back({section, "type",
                        "unknown ScalarType '" + typeStr + "'",
                        int(t.source().begin.line)});
    }

    if (auto s = t["source"].as_table()) {
        c.source    = parsePortRef(*s, section + ".source", errs);
        c.hasSource = true;
    }
    if (auto s = t["sink"].as_table()) {
        c.sink    = parsePortRef(*s, section + ".sink", errs);
        c.hasSink = true;
    }
    c.policy  = getStr(t, "policy",  {});
    c.ui      = getStr(t, "ui",      {});
    c.persist = getStr(t, "persist", {});
    c.onDisconnect    = getStr(t, "on_disconnect", "reset");
    c.disconnectValue = getDouble(t, "disconnect_value", 0.0);

    if (auto a = t["ack"].as_table()) {
        c.ack.dp        = getStr(*a, "dp", {});
        c.ack.timeoutMs = getInt(*a, "timeout_ms", 3000);
        if (auto v = (*a)["expected"]; v) {
            if (auto i = v.value<int64_t>())          c.ack.expected = std::int64_t(*i);
            else if (auto b = v.value<bool>())        c.ack.expected = *b;
            else if (auto s = v.value<std::string>()) c.ack.expected = *s;
            else if (auto d = v.value<double>())      c.ack.expected = *d;
        }
        c.hasAck = true;
    }
    return c;
}

template <class Section, class Fn>
void parseArray(toml::table const& root, std::string_view key,
                 std::vector<Section>& out, Fn parseOne, ValidationErrors& errs) {
    if (auto arr = root[key].as_array()) {
        int i = 0;
        for (auto&& node : *arr) {
            if (auto t = node.as_table()) {
                out.push_back(parseOne(*t, i, errs));
            }
            ++i;
        }
    }
}

// ─── Validation ────────────────────────────────────────────────────────

void checkUnique(std::vector<std::string> const& ids, std::string const& section,
                  std::string const& field, ValidationErrors& errs) {
    std::set<std::string> seen;
    for (auto const& id : ids) {
        if (!seen.insert(id).second) {
            errs.push_back({section, field,
                            "duplicate id '" + id + "'", -1});
        }
    }
}

// Built-in codec IDs that ConfigLoader::validate accepts without a [[codec]]
// declaration — these are registered at CodecRegistry::loadBuiltins time.
bool isBuiltinCodecId(std::string const& id) {
    return id.starts_with("builtin.");
}

// Maximum register count for a single Modbus write-multiple-registers (FC16)
// request — see Modbus spec.
constexpr int kMaxSinkWindowSize = 123;

int typeBitWidth(dp::ScalarType t) {
    switch (t) {
        case dp::ScalarType::Bool:    return 1;
        case dp::ScalarType::U16:
        case dp::ScalarType::S16:
        case dp::ScalarType::EnumU16: return 16;
        case dp::ScalarType::U32:
        case dp::ScalarType::S32:
        case dp::ScalarType::F32:     return 32;
        case dp::ScalarType::U64:
        case dp::ScalarType::S64:
        case dp::ScalarType::F64:     return 64;
        case dp::ScalarType::String:  return 64;  // mask check skipped
    }
    return 64;
}

void validateRefs(ConfigSchema const& s, ValidationErrors& errs) {
    std::set<std::string> transports;
    for (auto const& t : s.transports) transports.insert(t.id);

    std::set<std::string> sinkWindowIds;
    std::map<std::string, SinkWindowConfig const*> sinkWindowById;
    for (auto const& sw : s.sinkWindows) {
        sinkWindowIds.insert(sw.moduleId);
        sinkWindowById.emplace(sw.moduleId, &sw);
    }

    std::set<std::string> codecIds;
    for (auto const& c : s.codecs) codecIds.insert(c.id);

    std::set<std::string> datapointIds;
    for (auto const& d : s.datapoints) datapointIds.insert(d.id);

    auto checkTransportRef = [&](std::string const& tid,
                                  std::string const& section,
                                  std::string const& field) {
        if (!tid.empty() && !transports.count(tid)) {
            errs.push_back({section, field,
                            "references unknown transport '" + tid + "'", -1});
        }
    };

    auto checkPortRef = [&](PortRefConfig const& p, std::string const& section) {
        if (!p.port.empty() && !transports.count(p.port)) {
            errs.push_back({section, "port",
                            "references unknown transport '" + p.port + "'", -1});
        }
    };

    for (size_t i = 0; i < s.pollRanges.size(); ++i) {
        checkTransportRef(s.pollRanges[i].transport,
                          idx("poll_range", int(i)), "transport");
    }
    for (size_t i = 0; i < s.sinkWindows.size(); ++i) {
        auto const& sw = s.sinkWindows[i];
        auto const sec = idx("sink_window", int(i));
        checkTransportRef(sw.transport, sec, "transport");
        if (sw.size > kMaxSinkWindowSize) {
            errs.push_back({sec, "range",
                "size " + std::to_string(sw.size) + " exceeds Modbus FC16 max ("
                    + std::to_string(kMaxSinkWindowSize) + ")", -1});
        }
    }
    for (size_t i = 0; i < s.heartbeats.size(); ++i) {
        checkTransportRef(s.heartbeats[i].transport,
                          idx("heartbeat", int(i)), "transport");
    }
    for (size_t i = 0; i < s.commands.size(); ++i) {
        checkTransportRef(s.commands[i].transport,
                          idx("command", int(i)), "transport");
    }

    // Codec ref + kind / sink / source consistency.
    auto checkCodecRef = [&](std::string const& id, std::string const& section) {
        if (id.empty() || codecIds.count(id) || isBuiltinCodecId(id)) return;
        errs.push_back({section, "codec",
            "references unknown codec '" + id + "'", -1});
    };

    for (size_t i = 0; i < s.datapoints.size(); ++i) {
        auto const& d  = s.datapoints[i];
        auto const sec = idx("datapoint", int(i));
        if (d.hasSource) {
            checkPortRef(d.source, sec + ".source");
            checkCodecRef(d.source.codec, sec + ".source");
        }
        if (d.hasSink) {
            if (!d.sink.window.empty() && !sinkWindowIds.count(d.sink.window)) {
                errs.push_back({sec + ".sink", "window",
                                "references unknown sink_window '" + d.sink.window + "'",
                                -1});
            }
            checkPortRef(d.sink, sec + ".sink");
            checkCodecRef(d.sink.codec, sec + ".sink");

            // sink.addr must fall within the referenced sink_window.
            if (!d.sink.window.empty()) {
                auto it = sinkWindowById.find(d.sink.window);
                if (it != sinkWindowById.end()) {
                    auto const* sw = it->second;
                    int const lo = sw->startAddress;
                    int const hi = sw->startAddress + sw->size;
                    if (d.sink.address < lo || d.sink.address >= hi) {
                        errs.push_back({sec + ".sink", "addr",
                            "addr " + std::to_string(d.sink.address)
                                + " outside sink_window '" + d.sink.window + "' ["
                                + std::to_string(lo) + "," + std::to_string(hi) + ")", -1});
                    }
                }
            }
        }
        if (dp::isMultiRegister(d.type) && d.hasSource && d.source.wordOrder.empty()) {
            errs.push_back({sec + ".source", "wordOrder",
                            "type=" + std::string(dp::scalarTypeName(d.type))
                                + " requires wordOrder (ABCD/CDAB/BADC/DCBA)", -1});
        }
        if (d.type == dp::ScalarType::Bool && d.hasSource && d.source.bit < 0) {
            errs.push_back({sec + ".source", "bit",
                            "type=Bool requires bit (0..15)", -1});
        }

        // EnumU16 requires an explicit codec (no builtin enum codec exists).
        if (d.type == dp::ScalarType::EnumU16 && d.hasSource
         && d.source.codec.empty()) {
            errs.push_back({sec + ".source", "codec",
                "type=EnumU16 requires an explicit codec", -1});
        }

        // Kind ↔ source/sink consistency.
        if (d.kind == "Status" && !d.hasSource) {
            errs.push_back({sec, "kind", "kind=Status requires source", -1});
        }
        if (d.kind == "Command" && !d.hasSink) {
            errs.push_back({sec, "kind", "kind=Command requires sink", -1});
        }
        if (d.kind == "Bidirectional" && (!d.hasSource || !d.hasSink)) {
            errs.push_back({sec, "kind",
                "kind=Bidirectional requires both source and sink", -1});
        }

        // UntilAck policy needs an ack block; reuse hasAck flag set by parser.
        if (d.policy == "UntilAck" && !d.hasAck) {
            errs.push_back({sec, "policy",
                "policy=UntilAck requires [datapoint.ack]", -1});
        }

        // Mask must fit in the type's bit width.
        auto checkMask = [&](PortRefConfig const& p, std::string const& port_sec) {
            int const width = typeBitWidth(d.type);
            if (width >= 64) return;   // String and 64-bit types: skip
            uint64_t const limit = ((uint64_t(1) << width) - 1);
            if (p.mask != ~uint64_t(0) && (p.mask & ~limit) != 0) {
                errs.push_back({port_sec, "mask",
                    "mask 0x" + toHexLower(p.mask) + " exceeds type="
                        + std::string(dp::scalarTypeName(d.type))
                        + " bit width (" + std::to_string(width) + ")", -1});
            }
        };
        if (d.hasSource) checkMask(d.source, sec + ".source");
        if (d.hasSink)   checkMask(d.sink,   sec + ".sink");
    }

    for (size_t i = 0; i < s.ackWatches.size(); ++i) {
        auto const& a   = s.ackWatches[i];
        auto const sec  = idx("ack_watch", int(i));
        if (!a.dp.empty() && !datapointIds.count(a.dp)) {
            errs.push_back({sec, "dp",
                "references unknown datapoint '" + a.dp + "'", -1});
        }
    }

    for (size_t i = 0; i < s.routes.size(); ++i) {
        auto const& r   = s.routes[i];
        auto const sec  = idx("route", int(i));
        if (!r.from.empty() && !datapointIds.count(r.from)) {
            errs.push_back({sec, "from",
                "references unknown datapoint '" + r.from + "'", -1});
        }
        if (!r.to.empty() && !datapointIds.count(r.to)) {
            errs.push_back({sec, "to",
                "references unknown datapoint '" + r.to + "'", -1});
        }
    }

    for (size_t i = 0; i < s.bridges.size(); ++i) {
        auto const& b   = s.bridges[i];
        auto const sec  = idx("bridge", int(i));
        checkTransportRef(b.server, sec, "server");
        checkTransportRef(b.plc,    sec, "plc");
        if (b.writeCount < 0 || b.mirrorCount < 0) {
            errs.push_back({sec, "count",
                "write_count / mirror_count must be >= 0", -1});
        }
        // 镜像区必须至少有一个来自 plc 的 HR datapoint,否则镜像恒为 0(配错的常见原因)。
        if (b.mirrorCount > 0 && transports.count(b.plc)) {
            bool anyDp = false;
            for (auto const& d : s.datapoints) {
                if (!d.hasSource || d.source.port != b.plc) continue;
                if (d.source.table != "HR" && d.source.table != "HoldingRegisters") continue;
                int const a = d.source.address;
                if (a >= b.mirrorStart && a < b.mirrorStart + b.mirrorCount) { anyDp = true; break; }
            }
            if (!anyDp) {
                errs.push_back({sec, "mirror",
                    "mirror range [" + std::to_string(b.mirrorStart) + ","
                        + std::to_string(b.mirrorStart + b.mirrorCount)
                        + ") has no datapoint sourced from plc '" + b.plc
                        + "' (HR); mirror would be all zeros", -1});
            }
        }
    }
}

} // namespace

// ───────────────────────────────────────────────────────────────────────

std::expected<ConfigSchema, ValidationErrors>
ConfigLoader::loadFromToml(std::string const& path) {
    ConfigSchema schema;
    ValidationErrors errs;

    toml::table root;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            errs.push_back({"meta", "path", "cannot open '" + path + "'", -1});
            return std::unexpected(std::move(errs));
        }
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        root = toml::parse(content);
    } catch (toml::parse_error const& e) {
        errs.push_back({"meta", "toml",
                        std::string(e.description()),
                        int(e.source().begin.line)});
        return std::unexpected(std::move(errs));
    }

    schema.meta = parseMeta(root, errs);
    parseArray(root, "transport",   schema.transports,  parseTransport,  errs);
    parseArray(root, "codec",       schema.codecs,      parseCodec,      errs);
    parseArray(root, "poll_range",  schema.pollRanges,  parsePollRange,  errs);
    parseArray(root, "sink_window", schema.sinkWindows, parseSinkWindow, errs);
    parseArray(root, "heartbeat",   schema.heartbeats,  parseHeartbeat,  errs);
    parseArray(root, "ack_watch",   schema.ackWatches,  parseAckWatch,   errs);
    parseArray(root, "command",     schema.commands,    parseCommand,    errs);
    parseArray(root, "datapoint",   schema.datapoints,  parseDatapoint,  errs);
    parseArray(root, "route",       schema.routes,      parseRoute,      errs);
    parseArray(root, "bridge",      schema.bridges,     parseBridge,     errs);
    parseArray(root, "plugin",      schema.plugins,     parsePlugin,     errs);

    auto vErrs = validate(schema);
    if (!vErrs.has_value()) {
        for (auto const& e : vErrs.error()) errs.push_back(e);
    }

    if (!errs.empty()) return std::unexpected(std::move(errs));
    return schema;
}

std::expected<void, ValidationErrors>
ConfigLoader::validate(ConfigSchema const& schema) {
    ValidationErrors errs;

    std::vector<std::string> tIds;
    for (auto const& t : schema.transports) tIds.push_back(t.id);
    checkUnique(tIds, "transport", "id", errs);

    // Module IDs are unique across all module-bearing sections.
    std::vector<std::string> mIds;
    for (auto const& m : schema.pollRanges)  mIds.push_back(m.moduleId);
    for (auto const& m : schema.sinkWindows) mIds.push_back(m.moduleId);
    for (auto const& m : schema.heartbeats)  mIds.push_back(m.moduleId);
    for (auto const& m : schema.ackWatches)  mIds.push_back(m.moduleId);
    for (auto const& m : schema.commands)    mIds.push_back(m.moduleId);
    checkUnique(mIds, "module", "module_id", errs);

    std::vector<std::string> dIds;
    for (auto const& d : schema.datapoints) dIds.push_back(d.id);
    checkUnique(dIds, "datapoint", "id", errs);

    std::vector<std::string> cIds;
    for (auto const& c : schema.codecs) cIds.push_back(c.id);
    checkUnique(cIds, "codec", "id", errs);

    validateRefs(schema, errs);

    if (!errs.empty()) return std::unexpected(std::move(errs));
    return {};
}

} // namespace core::config
