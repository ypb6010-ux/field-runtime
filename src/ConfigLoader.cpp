// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/config/ConfigLoader.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <fstream>
#include <iterator>
#include <limits>
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

bool rangesOverlap(std::int64_t lhsStart, std::int64_t lhsCount,
                   std::int64_t rhsStart, std::int64_t rhsCount) {
    if (lhsCount <= 0 || rhsCount <= 0) return false;
    return lhsStart < rhsStart + rhsCount && rhsStart < lhsStart + lhsCount;
}

bool isHoldingRegisterTable(std::string const& table) {
    return table == "HR" || table == "HoldingRegisters";
}

std::optional<core::RegisterTable>
registerTableFromString(std::string const& table) {
    if (isHoldingRegisterTable(table)) {
        return core::RegisterTable::HoldingRegister;
    }
    if (table == "IR" || table == "InputRegisters") {
        return core::RegisterTable::InputRegister;
    }
    if (table == "Coil" || table == "Coils") {
        return core::RegisterTable::Coil;
    }
    if (table == "DI" || table == "DiscreteInputs") {
        return core::RegisterTable::DiscreteInput;
    }
    return std::nullopt;
}

std::optional<std::string> getStr(toml::table const& t, std::string_view key) {
    return t[key].value<std::string>();
}

std::string getStr(toml::table const& t, std::string_view key, std::string const& dflt) {
    return t[key].value<std::string>().value_or(dflt);
}

int narrowInt(std::int64_t value) {
    if (value > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    if (value < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    return static_cast<int>(value);
}

std::optional<int> getInt(toml::table const& t, std::string_view key) {
    if (auto v = t[key].value<int64_t>()) return narrowInt(*v);
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

void rejectUnknownKeys(toml::table const& t, std::string const& section,
                       std::initializer_list<std::string_view> allowed,
                       ValidationErrors& errs) {
    std::set<std::string_view> const known(allowed.begin(), allowed.end());
    for (auto const& [key, node] : t) {
        if (known.contains(key.str())) continue;
        errs.push_back({section, std::string(key.str()), "unknown field",
                        int(node.source().begin.line)});
    }
}

enum class TomlFieldType { String, Integer, Number, Boolean, Array, Table };

struct TomlFieldSpec {
    std::string_view key;
    TomlFieldType type;
};

void rejectWrongTypes(toml::table const& t, std::string const& section,
                      std::initializer_list<TomlFieldSpec> specs,
                      ValidationErrors& errs) {
    for (auto const& spec : specs) {
        auto const node = t[spec.key];
        if (!node) continue;
        bool valid = false;
        char const* expected = nullptr;
        switch (spec.type) {
            case TomlFieldType::String:
                valid = node.as_string() != nullptr; expected = "string"; break;
            case TomlFieldType::Integer:
                valid = node.as_integer() != nullptr; expected = "integer"; break;
            case TomlFieldType::Number:
                valid = node.as_integer() != nullptr
                     || node.as_floating_point() != nullptr;
                expected = "number";
                break;
            case TomlFieldType::Boolean:
                valid = node.as_boolean() != nullptr; expected = "boolean"; break;
            case TomlFieldType::Array:
                valid = node.as_array() != nullptr; expected = "array"; break;
            case TomlFieldType::Table:
                valid = node.as_table() != nullptr; expected = "table"; break;
        }
        if (!valid) {
            errs.push_back({section, std::string(spec.key),
                            std::string("expected ") + expected,
                            int(node.node()->source().begin.line)});
        }
    }
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

std::optional<sched::Priority> parsePriority(std::string const& s) {
    if (s == "Low")      return sched::Priority::Low;
    if (s == "Normal")   return sched::Priority::Normal;
    if (s == "High")     return sched::Priority::High;
    if (s == "Critical") return sched::Priority::Critical;
    return std::nullopt;
}

std::optional<sched::SchedulerKind> parseSchedulerKind(std::string const& s) {
    if (s == "serial")   return sched::SchedulerKind::Serial;
    if (s == "credit")   return sched::SchedulerKind::Credit;
    if (s == "priority") return sched::SchedulerKind::Priority;
    return std::nullopt;
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

MetaConfig parseMeta(toml::table const& root, ValidationErrors& errs) {
    MetaConfig m;
    if (auto t = root["meta"].as_table()) {
        rejectUnknownKeys(*t, "meta",
                          {"project", "version", "generated", "log_level"},
                          errs);
        rejectWrongTypes(
            *t, "meta",
            {{"project", TomlFieldType::String},
             {"version", TomlFieldType::String},
             {"generated", TomlFieldType::String},
             {"log_level", TomlFieldType::String}},
            errs);
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
    rejectUnknownKeys(
        t, section,
        {"id", "kind", "host", "port", "slave_id", "listen_address",
         "listen_port", "max_clients", "listen_ranges", "port_name",
         "baud_rate", "data_bits", "stop_bits", "parity", "endpoint_url",
         "security_policy", "username", "password", "backend",
         "node_id_template", "client_id", "broker_uri", "topic_prefix",
         "topic_template", "qos", "clean_session", "rack", "slot", "db",
         "reconnect_interval_ms", "connect_timeout_ms", "request_timeout_ms",
         "scheduler"},
        errs);
    rejectWrongTypes(
        t, section,
        {{"id", TomlFieldType::String}, {"kind", TomlFieldType::String},
         {"host", TomlFieldType::String}, {"port", TomlFieldType::Integer},
         {"slave_id", TomlFieldType::Integer},
         {"listen_address", TomlFieldType::String},
         {"listen_port", TomlFieldType::Integer},
         {"max_clients", TomlFieldType::Integer},
         {"listen_ranges", TomlFieldType::Array},
         {"port_name", TomlFieldType::String},
         {"baud_rate", TomlFieldType::Integer},
         {"data_bits", TomlFieldType::Integer},
         {"stop_bits", TomlFieldType::Integer},
         {"parity", TomlFieldType::String},
         {"endpoint_url", TomlFieldType::String},
         {"security_policy", TomlFieldType::String},
         {"username", TomlFieldType::String},
         {"password", TomlFieldType::String},
         {"backend", TomlFieldType::String},
         {"node_id_template", TomlFieldType::String},
         {"client_id", TomlFieldType::String},
         {"broker_uri", TomlFieldType::String},
         {"topic_prefix", TomlFieldType::String},
         {"topic_template", TomlFieldType::String},
         {"qos", TomlFieldType::Integer},
         {"clean_session", TomlFieldType::Boolean},
         {"rack", TomlFieldType::Integer},
         {"slot", TomlFieldType::Integer},
         {"db", TomlFieldType::Integer},
         {"reconnect_interval_ms", TomlFieldType::Integer},
         {"connect_timeout_ms", TomlFieldType::Integer},
         {"request_timeout_ms", TomlFieldType::Integer},
         {"scheduler", TomlFieldType::Table}},
        errs);
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
    c.port           = getInt(t, "port", 502);
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
        rejectUnknownKeys(
            *s, section + ".scheduler",
            {"kind", "default_timeout_ms", "inter_request_gap_ms",
             "max_queue_depth", "max_inflight", "starvation_guard_ms",
             "fifo_within_lane", "circuit_breaker_threshold",
             "circuit_breaker_open_ms"},
            errs);
        rejectWrongTypes(
            *s, section + ".scheduler",
            {{"kind", TomlFieldType::String},
             {"default_timeout_ms", TomlFieldType::Integer},
             {"inter_request_gap_ms", TomlFieldType::Integer},
             {"max_queue_depth", TomlFieldType::Integer},
             {"max_inflight", TomlFieldType::Integer},
             {"starvation_guard_ms", TomlFieldType::Integer},
             {"fifo_within_lane", TomlFieldType::Boolean},
             {"circuit_breaker_threshold", TomlFieldType::Integer},
             {"circuit_breaker_open_ms", TomlFieldType::Integer}},
            errs);
        auto const schedulerKindName = getStr(*s, "kind", "serial");
        if (auto kind = parseSchedulerKind(schedulerKindName)) {
            c.scheduler.kind = *kind;
        } else {
            errs.push_back({section + ".scheduler", "kind",
                            "must be serial, credit, or priority", -1});
        }
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
        int rangeIndex = 0;
        for (auto&& n : *lr) {
            if (auto wt = n.as_table()) {
                auto const rangeSection =
                    section + ".listen_ranges[" + std::to_string(rangeIndex)
                    + "]";
                rejectUnknownKeys(*wt, rangeSection, {"table", "range"}, errs);
                rejectWrongTypes(
                    *wt, rangeSection,
                    {{"table", TomlFieldType::String},
                     {"range", TomlFieldType::Array}},
                    errs);
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
                else {
                    r.table = core::RegisterTable::HoldingRegister;
                    errs.push_back({rangeSection, "table",
                                    "unknown register table '" + tbl + "'",
                                    -1});
                }
                if (auto arr = (*wt)["range"].as_array(); arr && arr->size() == 2) {
                    auto const start = arr->at(0).value<std::int64_t>();
                    auto const size = arr->at(1).value<std::int64_t>();
                    if (!start || !size) {
                        errs.push_back({rangeSection, "range",
                                        "expected [start, count] integers",
                                        int(arr->source().begin.line)});
                    } else {
                        r.startAddress = narrowInt(*start);
                        r.size = narrowInt(*size);
                    }
                } else {
                    errs.push_back({rangeSection, "range",
                                    "expected [start, count] array", -1});
                }
                c.listenRanges.push_back(r);
            } else {
                errs.push_back(
                    {section, "listen_ranges", "expected array of tables",
                     int(n.source().begin.line)});
            }
            ++rangeIndex;
        }
    }
    return c;
}

CodecConfig parseCodec(toml::table const& t,
                        int                index,
                        ValidationErrors&   errs) {
    auto const section = idx("codec", index);
    rejectUnknownKeys(t, section,
                      {"id", "kind", "map", "script", "arg"}, errs);
    rejectWrongTypes(
        t, section,
        {{"id", TomlFieldType::String}, {"kind", TomlFieldType::String},
         {"map", TomlFieldType::Table}, {"script", TomlFieldType::String},
         {"arg", TomlFieldType::String}},
        errs);
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
            else {
                errs.push_back(
                    {section + ".map", std::string(k.str()),
                     "map values must be string, integer, or boolean",
                     int(v.source().begin.line)});
                continue;
            }
            c.map.emplace(std::string(k.str()), std::move(val));
        }
    }
    return c;
}

PollRangeConfig parsePollRange(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = idx("poll_range", index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "table", "range",
                       "period_ms", "priority"},
                      errs);
    rejectWrongTypes(
        t, section,
        {{"module_id", TomlFieldType::String},
         {"transport", TomlFieldType::String},
         {"table", TomlFieldType::String},
         {"range", TomlFieldType::Array},
         {"period_ms", TomlFieldType::Integer},
         {"priority", TomlFieldType::String}},
        errs);
    PollRangeConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        auto const start = arr->at(0).value<std::int64_t>();
        auto const count = arr->at(1).value<std::int64_t>();
        if (!start || !count) {
            errs.push_back({section, "range",
                            "expected [start, count] integers",
                            int(arr->source().begin.line)});
        } else {
            c.startAddress = narrowInt(*start);
            c.count = narrowInt(*count);
        }
    } else {
        errs.push_back({section, "range",
                        "expected [start, count] array",
                        int(t.source().begin.line)});
    }
    c.periodMs = getInt(t, "period_ms", 0);
    auto const priorityName = getStr(t, "priority", "Normal");
    if (auto priority = parsePriority(priorityName)) {
        c.priority = *priority;
    } else {
        errs.push_back({section, "priority",
                        "must be Low, Normal, High, or Critical", -1});
    }
    return c;
}

core::RegisterWords parseU16Array(toml::array const& arr,
                                  std::string const& section,
                                  std::string const& field,
                                  ValidationErrors& errs) {
    core::RegisterWords out;
    for (auto&& n : arr) {
        auto const value = n.value<std::int64_t>();
        if (!value) {
            errs.push_back({section, field, "expected integer array",
                            int(n.source().begin.line)});
            continue;
        }
        if (*value < 0 || *value > 65535) {
            errs.push_back({section, field,
                            "values must be in [0, 65535]",
                            int(n.source().begin.line)});
            continue;
        }
        out.push_back(static_cast<std::uint16_t>(*value));
    }
    return out;
}

SinkWindowConfig parseSinkWindow(toml::table const& t,
                                   int                index,
                                   ValidationErrors&   errs) {
    auto const section = idx("sink_window", index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "table", "range",
                       "priority", "flush", "initial"},
                      errs);
    rejectWrongTypes(
        t, section,
        {{"module_id", TomlFieldType::String},
         {"transport", TomlFieldType::String},
         {"table", TomlFieldType::String},
         {"range", TomlFieldType::Array},
         {"priority", TomlFieldType::String},
         {"flush", TomlFieldType::Table},
         {"initial", TomlFieldType::Array}},
        errs);
    SinkWindowConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        auto const start = arr->at(0).value<std::int64_t>();
        auto const size = arr->at(1).value<std::int64_t>();
        if (!start || !size) {
            errs.push_back({section, "range",
                            "expected [start, size] integers",
                            int(arr->source().begin.line)});
        } else {
            c.startAddress = narrowInt(*start);
            c.size = narrowInt(*size);
        }
    } else {
        errs.push_back({section, "range",
                        "expected [start, size] array",
                        int(t.source().begin.line)});
    }
    auto const priorityName = getStr(t, "priority", "High");
    if (auto priority = parsePriority(priorityName)) {
        c.priority = *priority;
    } else {
        errs.push_back({section, "priority",
                        "must be Low, Normal, High, or Critical", -1});
    }
    if (auto f = t["flush"].as_table()) {
        rejectUnknownKeys(*f, section + ".flush",
                          {"debounce_ms", "keepalive_ms", "coalesce",
                           "max_retries"},
                          errs);
        rejectWrongTypes(
            *f, section + ".flush",
            {{"debounce_ms", TomlFieldType::Integer},
             {"keepalive_ms", TomlFieldType::Integer},
             {"coalesce", TomlFieldType::Boolean},
             {"max_retries", TomlFieldType::Integer}},
            errs);
        c.flush.debounceMs     = getInt(*f, "debounce_ms", 20);
        c.flush.keepaliveMs    = getInt(*f, "keepalive_ms", 0);
        c.flush.coalesceWrites = getBool(*f, "coalesce").value_or(true);
        c.flush.maxRetries     = getInt(*f, "max_retries", 0);
    }
    if (auto arr = t["initial"].as_array()) {
        c.initial = parseU16Array(*arr, section, "initial", errs);
    }
    return c;
}

HeartbeatConfig parseHeartbeat(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = idx("heartbeat", index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "table", "address",
                       "value", "values", "period_ms", "priority",
                       "incrementer"},
                      errs);
    rejectWrongTypes(
        t, section,
        {{"module_id", TomlFieldType::String},
         {"transport", TomlFieldType::String},
         {"table", TomlFieldType::String},
         {"address", TomlFieldType::Integer},
         {"value", TomlFieldType::Integer},
         {"values", TomlFieldType::Array},
         {"period_ms", TomlFieldType::Integer},
         {"priority", TomlFieldType::String},
         {"incrementer", TomlFieldType::String}},
        errs);
    HeartbeatConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    c.table       = getStr(t, "table", "HR");
    c.address     = getInt(t, "address", 0);
    if (auto arr = t["values"].as_array()) {
        c.values = parseU16Array(*arr, section, "values", errs);
    } else if (auto v = getInt(t, "value")) {
        if (*v < 0 || *v > 65535) {
            errs.push_back({section, "value",
                            "must be in [0, 65535]", -1});
        } else {
            c.values.push_back(static_cast<std::uint16_t>(*v));
        }
    }
    if (c.values.empty()) {
        errs.push_back({section, "values",
                        "heartbeat requires non-empty values",
                        int(t.source().begin.line)});
    }
    c.periodMs    = getInt(t, "period_ms", 0);
    auto const priorityName = getStr(t, "priority", "Low");
    if (auto priority = parsePriority(priorityName)) {
        c.priority = *priority;
    } else {
        errs.push_back({section, "priority",
                        "must be Low, Normal, High, or Critical", -1});
    }
    c.incrementer = getStr(t, "incrementer", "none");
    return c;
}

AckWatchConfig parseAckWatch(toml::table const& t,
                               int                index,
                               ValidationErrors&   errs) {
    auto const section = idx("ack_watch", index);
    rejectUnknownKeys(t, section,
                      {"module_id", "dp", "expected", "timeout_ms"}, errs);
    rejectWrongTypes(
        t, section,
        {{"module_id", TomlFieldType::String},
         {"dp", TomlFieldType::String},
         {"timeout_ms", TomlFieldType::Integer}},
        errs);
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
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "priority",
                       "interruptable", "trigger", "writes"},
                      errs);
    rejectWrongTypes(
        t, section,
        {{"module_id", TomlFieldType::String},
         {"transport", TomlFieldType::String},
         {"priority", TomlFieldType::String},
         {"interruptable", TomlFieldType::Boolean},
         {"trigger", TomlFieldType::String},
         {"writes", TomlFieldType::Array}},
        errs);
    CommandConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    auto const priorityName = getStr(t, "priority", "High");
    if (auto priority = parsePriority(priorityName)) {
        c.priority = *priority;
    } else {
        errs.push_back({section, "priority",
                        "must be Low, Normal, High, or Critical", -1});
    }
    c.interruptable = getBool(t, "interruptable").value_or(false);
    c.trigger       = getStr(t, "trigger", {});
    if (auto arr = t["writes"].as_array()) {
        int writeIndex = 0;
        for (auto&& n : *arr) {
            if (auto wt = n.as_table()) {
                auto const writeSection = section + ".writes["
                                        + std::to_string(writeIndex) + "]";
                rejectUnknownKeys(*wt, writeSection,
                                  {"table", "address", "value"}, errs);
                rejectWrongTypes(
                    *wt, writeSection,
                    {{"table", TomlFieldType::String},
                     {"address", TomlFieldType::Integer},
                     {"value", TomlFieldType::Integer}},
                    errs);
                CommandWriteEntry e;
                e.table   = getStr(*wt, "table", "HR");
                e.address = getInt(*wt, "address", 0);
                int const rawValue = getInt(*wt, "value", 0);
                if (rawValue < 0 || rawValue > 65535) {
                    errs.push_back({writeSection, "value",
                                    "must be in [0, 65535]", -1});
                } else {
                    e.value = static_cast<std::uint16_t>(rawValue);
                }
                c.writes.push_back(e);
            } else {
                errs.push_back({section, "writes",
                                "expected array of tables",
                                int(n.source().begin.line)});
            }
            ++writeIndex;
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
    rejectUnknownKeys(t, section, {"name", "from", "to", "policy"}, errs);
    rejectWrongTypes(
        t, section,
        {{"name", TomlFieldType::String}, {"from", TomlFieldType::String},
         {"to", TomlFieldType::String}, {"policy", TomlFieldType::String}},
        errs);
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
    rejectUnknownKeys(
        t, section,
        {"server", "plc", "offset", "write_start", "write_count",
         "mirror_start", "mirror_count", "mirror_policy",
         "mirror_period_ms"},
        errs);
    rejectWrongTypes(
        t, section,
        {{"server", TomlFieldType::String}, {"plc", TomlFieldType::String},
         {"offset", TomlFieldType::Integer},
         {"write_start", TomlFieldType::Integer},
         {"write_count", TomlFieldType::Integer},
         {"mirror_start", TomlFieldType::Integer},
         {"mirror_count", TomlFieldType::Integer},
         {"mirror_policy", TomlFieldType::String},
         {"mirror_period_ms", TomlFieldType::Integer}},
        errs);
    BridgeConfig c;
    requireStr(t, "server", section, c.server, errs);
    requireStr(t, "plc",    section, c.plc,    errs);
    c.offset         = getInt(t, "offset", 0);
    c.writeStart     = getInt(t, "write_start", 0);
    c.writeCount     = getInt(t, "write_count", 0);
    c.mirrorStart    = getInt(t, "mirror_start", 0);
    c.mirrorCount    = getInt(t, "mirror_count", 0);
    auto const mirrorPolicy = getStr(t, "mirror_policy", {});
    if (c.mirrorCount > 0 && mirrorPolicy.empty()) {
        errs.push_back({section, "mirror_policy",
                        "is required when mirror_count is positive", -1});
    } else if (mirrorPolicy == "AfterPoll") {
        c.mirrorPolicy = BridgeMirrorPolicy::AfterPoll;
    } else if (mirrorPolicy == "Periodic") {
        c.mirrorPolicy = BridgeMirrorPolicy::Periodic;
    } else if (!mirrorPolicy.empty()) {
        errs.push_back({section, "mirror_policy",
                        "must be exactly 'AfterPoll' or 'Periodic'", -1});
    }
    c.mirrorPeriodMs = getInt(t, "mirror_period_ms", 0);
    return c;
}

PluginConfig parsePlugin(toml::table const& t,
                          int                index,
                          ValidationErrors&   errs) {
    auto const section = idx("plugin", index);
    rejectUnknownKeys(t, section, {"dll", "name", "config"}, errs);
    rejectWrongTypes(
        t, section,
        {{"dll", TomlFieldType::String}, {"name", TomlFieldType::String},
         {"config", TomlFieldType::String}},
        errs);
    PluginConfig c;
    requireStr(t, "dll", section, c.dllPath, errs);
    c.name   = getStr(t, "name",   {});
    c.config = getStr(t, "config", {});
    return c;
}

PortRefConfig parsePortRef(toml::table const& t,
                             std::string const& section,
                             ValidationErrors&   errs) {
    rejectUnknownKeys(t, section,
                      {"port", "table", "addr", "bit", "wordOrder", "shift",
                       "mask", "scale", "offset", "codec", "dedupe", "window"},
                      errs);
    rejectWrongTypes(
        t, section,
        {{"port", TomlFieldType::String}, {"table", TomlFieldType::String},
         {"addr", TomlFieldType::Integer}, {"bit", TomlFieldType::Integer},
         {"wordOrder", TomlFieldType::String},
         {"shift", TomlFieldType::Integer},
         {"mask", TomlFieldType::Integer},
         {"scale", TomlFieldType::Number},
         {"offset", TomlFieldType::Number},
         {"codec", TomlFieldType::String},
         {"dedupe", TomlFieldType::String},
         {"window", TomlFieldType::String}},
        errs);
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

    return p;
}

DatapointConfig parseDatapoint(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = idx("datapoint", index);
    rejectUnknownKeys(
        t, section,
        {"id", "kind", "type", "source", "sink", "policy", "ack", "ui",
         "persist", "on_disconnect", "disconnect_value"},
        errs);
    rejectWrongTypes(
        t, section,
        {{"id", TomlFieldType::String}, {"kind", TomlFieldType::String},
         {"type", TomlFieldType::String}, {"source", TomlFieldType::Table},
         {"sink", TomlFieldType::Table}, {"policy", TomlFieldType::String},
         {"ack", TomlFieldType::Table}, {"ui", TomlFieldType::String},
         {"persist", TomlFieldType::String},
         {"on_disconnect", TomlFieldType::String},
         {"disconnect_value", TomlFieldType::Number}},
        errs);
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
        rejectUnknownKeys(*a, section + ".ack",
                          {"dp", "expected", "timeout_ms"}, errs);
        rejectWrongTypes(
            *a, section + ".ack",
            {{"dp", TomlFieldType::String},
             {"timeout_ms", TomlFieldType::Integer}},
            errs);
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
            } else {
                errs.push_back({std::string(key), std::to_string(i),
                                "expected table entry",
                                int(node.source().begin.line)});
            }
            ++i;
        }
    } else if (root[key]) {
        errs.push_back({std::string(key), std::string(key),
                        "expected array of tables",
                        int(root[key].node()->source().begin.line)});
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

bool isKnownTable(std::string const& table) {
    return table == "HR" || table == "HoldingRegisters"
        || table == "IR" || table == "InputRegisters"
        || table == "Coil" || table == "Coils"
        || table == "DI" || table == "DiscreteInputs";
}

bool isWritableTable(std::string const& table) {
    return table == "HR" || table == "HoldingRegisters"
        || table == "Coil" || table == "Coils";
}

int maxReadCount(std::string const& table) {
    return table == "Coil" || table == "Coils"
        || table == "DI" || table == "DiscreteInputs" ? 2000 : 125;
}

int maxWriteCount(std::string const& table) {
    return table == "Coil" || table == "Coils" ? 1968 : 123;
}

bool usesModbusClientLimits(transport::TransportKind kind) {
    return kind == transport::TransportKind::ModbusTcpClient
        || kind == transport::TransportKind::ModbusRtu;
}

void validateRange(std::string const& section, std::string const& field,
                   int start, int count, int maxCount,
                   ValidationErrors& errs) {
    if (start < 0 || start > 65535) {
        errs.push_back({section, field, "start must be in [0, 65535]", -1});
    }
    if (count <= 0) {
        errs.push_back({section, field, "count must be positive", -1});
    } else if (count > maxCount) {
        errs.push_back({section, field,
                        "count exceeds maximum " + std::to_string(maxCount),
                        -1});
    }
    if (start >= 0 && count > 0
        && std::int64_t(start) + count > 65536) {
        errs.push_back({section, field,
                        "range exceeds register address space", -1});
    }
}

void validateValues(ConfigSchema const& schema, ValidationErrors& errs) {
    auto requireNonEmpty = [&](std::string const& value,
                               std::string const& section,
                               std::string const& field) {
        if (value.empty()) {
            errs.push_back({section, field, "must not be empty", -1});
        }
    };
    auto requirePositive = [&](int value, std::string const& section,
                               std::string const& field) {
        if (value <= 0) {
            errs.push_back({section, field, "must be positive", -1});
        }
    };
    auto requireNonNegative = [&](int value, std::string const& section,
                                  std::string const& field) {
        if (value < 0) {
            errs.push_back({section, field, "must be non-negative", -1});
        }
    };

    if (!schema.meta.logLevel.empty()
        && schema.meta.logLevel != "trace"
        && schema.meta.logLevel != "debug"
        && schema.meta.logLevel != "info"
        && schema.meta.logLevel != "warn"
        && schema.meta.logLevel != "error"
        && schema.meta.logLevel != "critical") {
        errs.push_back({"meta", "log_level",
                        "must be trace, debug, info, warn, error, or critical",
                        -1});
    }

    for (size_t i = 0; i < schema.transports.size(); ++i) {
        auto const& transportConfig = schema.transports[i];
        auto const section = idx("transport", int(i));
        requireNonEmpty(transportConfig.id, section, "id");
        if (transportConfig.port < 1 || transportConfig.port > 65535) {
            errs.push_back({section, "port", "must be in [1, 65535]", -1});
        }
        if (transportConfig.listenPort < 1
            || transportConfig.listenPort > 65535) {
            errs.push_back(
                {section, "listen_port", "must be in [1, 65535]", -1});
        }
        if (transportConfig.slaveId < 0 || transportConfig.slaveId > 247) {
            errs.push_back({section, "slave_id", "must be in [0, 247]", -1});
        }
        requirePositive(transportConfig.connectTimeoutMs,
                        section, "connect_timeout_ms");
        requirePositive(transportConfig.requestTimeoutMs,
                        section, "request_timeout_ms");
        requireNonNegative(transportConfig.reconnectIntervalMs,
                           section, "reconnect_interval_ms");
        requirePositive(transportConfig.scheduler.defaultTimeoutMs,
                        section + ".scheduler", "default_timeout_ms");
        requireNonNegative(transportConfig.scheduler.interRequestGapMs,
                           section + ".scheduler", "inter_request_gap_ms");
        requirePositive(transportConfig.scheduler.maxQueueDepth,
                        section + ".scheduler", "max_queue_depth");
        requirePositive(transportConfig.scheduler.maxInflight,
                        section + ".scheduler", "max_inflight");
        requireNonNegative(transportConfig.scheduler.starvationGuardMs,
                           section + ".scheduler", "starvation_guard_ms");
        requirePositive(transportConfig.scheduler.circuitBreakerThreshold,
                        section + ".scheduler",
                        "circuit_breaker_threshold");
        requirePositive(transportConfig.scheduler.circuitBreakerOpenMs,
                        section + ".scheduler", "circuit_breaker_open_ms");

        if (transportConfig.kind
            == transport::TransportKind::ModbusTcpClient
            || transportConfig.kind == transport::TransportKind::S7Client) {
            requireNonEmpty(transportConfig.host, section, "host");
        }
        if (transportConfig.kind
            == transport::TransportKind::ModbusTcpServer) {
            requirePositive(transportConfig.maxClients,
                            section, "max_clients");
            if (transportConfig.listenRanges.empty()) {
                errs.push_back(
                    {section, "listen_ranges",
                     "modbus_tcp_server requires at least one explicit "
                     "listen range", -1});
            }
        }
        if (transportConfig.kind == transport::TransportKind::ModbusRtu) {
            requireNonEmpty(transportConfig.portName, section, "port_name");
            requirePositive(transportConfig.baudRate, section, "baud_rate");
            if (transportConfig.dataBits < 5
                || transportConfig.dataBits > 8) {
                errs.push_back(
                    {section, "data_bits", "must be in [5, 8]", -1});
            }
            if (transportConfig.stopBits != 1
                && transportConfig.stopBits != 2) {
                errs.push_back(
                    {section, "stop_bits", "must be 1 or 2", -1});
            }
            if (transportConfig.parity != "none"
                && transportConfig.parity != "even"
                && transportConfig.parity != "odd") {
                errs.push_back(
                    {section, "parity", "must be none, even, or odd", -1});
            }
        }
        if (transportConfig.kind
            == transport::TransportKind::OpcUaClient) {
            requireNonEmpty(transportConfig.endpointUrl,
                            section, "endpoint_url");
        }
        if (transportConfig.kind == transport::TransportKind::MqttClient
            || transportConfig.kind
                == transport::TransportKind::MqttPahoClient) {
            requireNonEmpty(transportConfig.brokerUri,
                            section, "broker_uri");
            if (transportConfig.qos < 0 || transportConfig.qos > 2) {
                errs.push_back({section, "qos", "must be in [0, 2]", -1});
            }
        }
        for (size_t rangeIndex = 0;
             rangeIndex < transportConfig.listenRanges.size();
             ++rangeIndex) {
            auto const& range = transportConfig.listenRanges[rangeIndex];
            validateRange(section + ".listen_ranges["
                              + std::to_string(rangeIndex) + "]",
                          "range", range.startAddress, range.size,
                          65536, errs);
        }
    }

    for (size_t i = 0; i < schema.pollRanges.size(); ++i) {
        auto const& poll = schema.pollRanges[i];
        auto const section = idx("poll_range", int(i));
        requireNonEmpty(poll.moduleId, section, "module_id");
        if (!isKnownTable(poll.table)) {
            errs.push_back({section, "table",
                            "unknown register table '" + poll.table + "'",
                            -1});
        } else {
            validateRange(section, "range", poll.startAddress, poll.count,
                          65536, errs);
        }
        requirePositive(poll.periodMs, section, "period_ms");
    }

    for (size_t i = 0; i < schema.sinkWindows.size(); ++i) {
        auto const& sink = schema.sinkWindows[i];
        auto const section = idx("sink_window", int(i));
        requireNonEmpty(sink.moduleId, section, "module_id");
        if (!isWritableTable(sink.table)) {
            errs.push_back({section, "table",
                            "table '" + sink.table + "' is not writable", -1});
        } else {
            validateRange(section, "range", sink.startAddress, sink.size,
                          65536, errs);
        }
        requireNonNegative(sink.flush.debounceMs,
                           section + ".flush", "debounce_ms");
        requireNonNegative(sink.flush.keepaliveMs,
                           section + ".flush", "keepalive_ms");
        requireNonNegative(sink.flush.maxRetries,
                           section + ".flush", "max_retries");
        if (sink.flush.maxRetries != 0) {
            errs.push_back(
                {section + ".flush", "max_retries",
                 "scheduler-level retries are not implemented; use 0 "
                 "(failed windows remain dirty and retry on the next tick)",
                 -1});
        }
        if (int(sink.initial.size()) > sink.size) {
            errs.push_back(
                {section, "initial", "contains more values than range size",
                 -1});
        }
    }

    for (size_t i = 0; i < schema.heartbeats.size(); ++i) {
        auto const& heartbeat = schema.heartbeats[i];
        auto const section = idx("heartbeat", int(i));
        requireNonEmpty(heartbeat.moduleId, section, "module_id");
        if (!isWritableTable(heartbeat.table)) {
            errs.push_back({section, "table",
                            "table '" + heartbeat.table
                                + "' is not writable",
                            -1});
        } else if (!heartbeat.values.empty()) {
            validateRange(section, "values", heartbeat.address,
                          int(heartbeat.values.size()), 65536, errs);
        }
        requirePositive(heartbeat.periodMs, section, "period_ms");
        if (heartbeat.incrementer != "none") {
            errs.push_back(
                {section, "incrementer",
                 "only incrementer='none' is currently implemented", -1});
        }
    }

    for (size_t i = 0; i < schema.ackWatches.size(); ++i) {
        auto const& ack = schema.ackWatches[i];
        auto const section = idx("ack_watch", int(i));
        requireNonEmpty(ack.moduleId, section, "module_id");
        requireNonEmpty(ack.dp, section, "dp");
        requirePositive(ack.timeoutMs, section, "timeout_ms");
    }

    for (size_t i = 0; i < schema.commands.size(); ++i) {
        auto const& command = schema.commands[i];
        auto const section = idx("command", int(i));
        requireNonEmpty(command.moduleId, section, "module_id");
        if (!command.trigger.empty()) {
            errs.push_back(
                {section, "trigger",
                 "command triggers are not implemented; invoke the command "
                 "through the runtime API", -1});
        }
        for (size_t writeIndex = 0;
             writeIndex < command.writes.size(); ++writeIndex) {
            auto const& write = command.writes[writeIndex];
            auto const writeSection = section + ".writes["
                                    + std::to_string(writeIndex) + "]";
            if (!isWritableTable(write.table)) {
                errs.push_back(
                    {writeSection, "table", "table is not writable", -1});
            }
            validateRange(writeSection, "address",
                          write.address, 1, 65536, errs);
        }
    }

    for (size_t i = 0; i < schema.datapoints.size(); ++i) {
        auto const& datapoint = schema.datapoints[i];
        auto const section = idx("datapoint", int(i));
        requireNonEmpty(datapoint.id, section, "id");
        if (datapoint.kind != "Status"
            && datapoint.kind != "Command"
            && datapoint.kind != "Bidirectional") {
            errs.push_back({section, "kind",
                            "must be Status, Command, or Bidirectional", -1});
        }
        if (datapoint.onDisconnect != "reset"
            && datapoint.onDisconnect != "hold") {
            errs.push_back({section, "on_disconnect",
                            "must be 'reset' or 'hold'", -1});
        }
        if (!std::isfinite(datapoint.disconnectValue)) {
            errs.push_back({section, "disconnect_value",
                            "must be finite", -1});
        }
        auto validatePort = [&](PortRefConfig const& port,
                                std::string const& portSection,
                                bool source) {
            requireNonEmpty(port.port, portSection, "port");
            if (!isKnownTable(port.table)) {
                errs.push_back({portSection, "table",
                                "unknown register table '" + port.table + "'",
                                -1});
            }
            validateRange(portSection, "addr", port.address,
                          std::max(1, dp::registerCountFor(datapoint.type)),
                          65536, errs);
            if (port.bit < -1 || port.bit > 15) {
                errs.push_back(
                    {portSection, "bit", "must be in [0, 15] when set", -1});
            }
            if (port.bit >= 0 && datapoint.type != dp::ScalarType::Bool) {
                errs.push_back(
                    {portSection, "bit", "bit is only valid for Bool", -1});
            }
            if (port.shift < 0 || port.shift > 63) {
                errs.push_back(
                    {portSection, "shift", "must be in [0, 63]", -1});
            }
            if (!port.wordOrder.empty()
                && port.wordOrder != "ABCD"
                && port.wordOrder != "CDAB"
                && port.wordOrder != "BADC"
                && port.wordOrder != "DCBA") {
                errs.push_back(
                    {portSection, "wordOrder",
                     "must be ABCD, CDAB, BADC, or DCBA", -1});
            }
            if (port.dedupe != "none") {
                errs.push_back(
                    {portSection, "dedupe",
                     "only dedupe='none' is currently implemented", -1});
            }
            if (!std::isfinite(port.scale) || port.scale == 0.0
                || !std::isfinite(port.offset)) {
                errs.push_back(
                    {portSection, "scale",
                     "scale must be finite/non-zero and offset finite", -1});
            }
            if (!source && dp::isMultiRegister(datapoint.type)
                && (port.shift != 0
                    || port.mask != ~std::uint64_t(0))) {
                errs.push_back(
                    {portSection, "transform",
                     "multi-register sinks require full-word writes", -1});
            }
        };
        if (datapoint.hasSource) {
            validatePort(datapoint.source, section + ".source", true);
        }
        if (datapoint.hasSink) {
            validatePort(datapoint.sink, section + ".sink", false);
        }
        if (!datapoint.policy.empty()) {
            errs.push_back(
                {section, "policy",
                 "datapoint policies are not implemented; use [[route]] "
                 "for ContinuousMirror routing", -1});
        }
        if (datapoint.hasAck) {
            errs.push_back(
                {section, "ack",
                 "inline datapoint acknowledgements are not implemented; "
                 "use [[ack_watch]]", -1});
        }
    }

    for (size_t i = 0; i < schema.routes.size(); ++i) {
        auto const& route = schema.routes[i];
        if (route.policy != "ContinuousMirror") {
            errs.push_back(
                {idx("route", int(i)), "policy",
                 "only ContinuousMirror is implemented", -1});
        }
    }

    for (size_t i = 0; i < schema.codecs.size(); ++i) {
        auto const& codec = schema.codecs[i];
        auto const section = idx("codec", int(i));
        requireNonEmpty(codec.id, section, "id");
        if (codec.kind != "enum_u16" && codec.kind != "lua") {
            errs.push_back(
                {section, "kind", "must be enum_u16 or lua", -1});
        }
        if (codec.kind == "enum_u16" && codec.map.empty()) {
            errs.push_back(
                {section, "map", "enum_u16 requires a non-empty map", -1});
        }
        if (codec.kind == "enum_u16") {
            for (auto const& [key, value] : codec.map) {
                (void)value;
                unsigned parsed = 0;
                auto const* begin = key.data();
                auto const* end = begin + key.size();
                auto const result = std::from_chars(begin, end, parsed, 10);
                if (result.ec != std::errc{} || result.ptr != end
                    || parsed > 65535u) {
                    errs.push_back(
                        {section + ".map", key,
                         "enum_u16 keys must be decimal integers in "
                         "[0, 65535]", -1});
                }
            }
        }
        if (codec.kind == "lua") {
            requireNonEmpty(codec.script, section, "script");
        }
    }
}

void validateRefs(ConfigSchema const& s, ValidationErrors& errs) {
    std::set<std::string> transports;
    std::map<std::string, TransportConfig const*> transportById;
    for (auto const& t : s.transports) {
        transports.insert(t.id);
        transportById.emplace(t.id, &t);
    }

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
        auto const& poll = s.pollRanges[i];
        auto const section = idx("poll_range", int(i));
        checkTransportRef(poll.transport, section, "transport");
        auto const transportIt = transportById.find(poll.transport);
        if (transportIt != transportById.end()
            && usesModbusClientLimits(transportIt->second->kind)
            && poll.count > maxReadCount(poll.table)) {
            errs.push_back(
                {section, "range",
                 "count exceeds Modbus read maximum "
                     + std::to_string(maxReadCount(poll.table)),
                 -1});
        }
    }
    for (size_t i = 0; i < s.sinkWindows.size(); ++i) {
        auto const& sw = s.sinkWindows[i];
        auto const sec = idx("sink_window", int(i));
        checkTransportRef(sw.transport, sec, "transport");
        auto const transportIt = transportById.find(sw.transport);
        if (transportIt != transportById.end()
            && usesModbusClientLimits(transportIt->second->kind)
            && sw.size > maxWriteCount(sw.table)) {
            errs.push_back(
                {sec, "range",
                 "size exceeds Modbus write maximum "
                     + std::to_string(maxWriteCount(sw.table)),
                 -1});
        }
    }
    for (size_t i = 0; i < s.heartbeats.size(); ++i) {
        auto const& heartbeat = s.heartbeats[i];
        auto const section = idx("heartbeat", int(i));
        checkTransportRef(heartbeat.transport, section, "transport");
        auto const transportIt = transportById.find(heartbeat.transport);
        if (transportIt != transportById.end()
            && usesModbusClientLimits(transportIt->second->kind)
            && int(heartbeat.values.size())
                > maxWriteCount(heartbeat.table)) {
            errs.push_back(
                {section, "values",
                 "value count exceeds Modbus write maximum "
                     + std::to_string(maxWriteCount(heartbeat.table)),
                 -1});
        }
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
            auto const transportIt = transportById.find(d.source.port);
            int const words = std::max(1, dp::registerCountFor(d.type));
            if (transportIt != transportById.end()
                && transportIt->second->kind
                    == transport::TransportKind::ModbusTcpServer) {
                bool covered = false;
                auto const sourceTable =
                    registerTableFromString(d.source.table);
                for (auto const& range : transportIt->second->listenRanges) {
                    if (!sourceTable || range.table != *sourceTable) {
                        continue;
                    }
                    if (d.source.address >= range.startAddress
                        && std::int64_t(d.source.address) + words
                            <= std::int64_t(range.startAddress)
                                + range.size) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    errs.push_back(
                        {sec + ".source", "addr",
                         "server source range is outside listen_ranges", -1});
                }
            } else if (transportIt != transportById.end()) {
                bool covered = false;
                for (auto const& poll : s.pollRanges) {
                    if (poll.transport != d.source.port
                        || registerTableFromString(poll.table)
                            != registerTableFromString(d.source.table)) {
                        continue;
                    }
                    if (d.source.address >= poll.startAddress
                        && std::int64_t(d.source.address) + words
                            <= std::int64_t(poll.startAddress) + poll.count) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    errs.push_back(
                        {sec + ".source", "addr",
                         "source range is not covered by a poll_range", -1});
                }
            }
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
                    if (d.sink.port != sw->transport
                        || d.sink.table != sw->table) {
                        errs.push_back(
                            {sec + ".sink", "window",
                             "sink port/table must match its sink_window", -1});
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
        auto const serverIt = transportById.find(b.server);
        auto const plcIt = transportById.find(b.plc);
        if (serverIt != transportById.end()
            && serverIt->second->kind
                != transport::TransportKind::ModbusTcpServer) {
            errs.push_back({sec, "server",
                            "must reference a modbus_tcp_server transport", -1});
        }
        if (plcIt != transportById.end()
            && plcIt->second->kind
                != transport::TransportKind::ModbusTcpClient
            && plcIt->second->kind != transport::TransportKind::ModbusRtu
            && plcIt->second->kind != transport::TransportKind::S7Client) {
            errs.push_back({sec, "plc",
                            "must reference a writable PLC client transport",
                            -1});
        }
        if (b.writeCount < 0 || b.mirrorCount < 0) {
            errs.push_back({sec, "count",
                "write_count / mirror_count must be >= 0", -1});
        }
        if (b.mirrorCount > 0) {
            if (b.mirrorPolicy == BridgeMirrorPolicy::Periodic) {
                if (b.mirrorPeriodMs <= 0) {
                    errs.push_back({sec, "mirror_period_ms",
                                    "must be positive for Periodic mirroring",
                                    -1});
                }
            } else if (b.mirrorPeriodMs != 0) {
                errs.push_back({sec, "mirror_period_ms",
                                "is only valid when mirror_policy is 'Periodic'",
                                -1});
            }
        }
        auto validRange = [&](std::int64_t start, std::int64_t count) {
            return start >= 0 && count >= 0 && start + count <= 65536;
        };
        if (!validRange(b.writeStart, b.writeCount)) {
            errs.push_back({sec, "write",
                            "write range must stay within [0, 65536)", -1});
        }
        if (!validRange(b.mirrorStart, b.mirrorCount)) {
            errs.push_back({sec, "mirror",
                            "mirror range must stay within [0, 65536)", -1});
        }
        auto const mappedPlcWriteStart =
            std::int64_t(b.writeStart) - std::int64_t(b.offset);
        auto const mappedServerMirrorStart =
            std::int64_t(b.mirrorStart) + std::int64_t(b.offset);
        if (!validRange(mappedPlcWriteStart, b.writeCount)) {
            errs.push_back({sec, "offset",
                            "write range maps outside PLC address space", -1});
        }
        if (!validRange(mappedServerMirrorStart, b.mirrorCount)) {
            errs.push_back({sec, "offset",
                            "mirror range maps outside server address space",
                            -1});
        }
        if (rangesOverlap(b.writeStart, b.writeCount,
                          mappedServerMirrorStart, b.mirrorCount)) {
            errs.push_back({sec, "range",
                            "server write range overlaps mapped mirror range",
                            -1});
        }
        if (serverIt != transportById.end()
            && serverIt->second->kind
                == transport::TransportKind::ModbusTcpServer) {
            auto covered = [&](std::int64_t start, std::int64_t count) {
                if (count <= 0) return true;
                for (auto const& range : serverIt->second->listenRanges) {
                    if (range.table != core::RegisterTable::HoldingRegister) {
                        continue;
                    }
                    if (start >= range.startAddress
                        && start + count
                            <= std::int64_t(range.startAddress) + range.size) {
                        return true;
                    }
                }
                return false;
            };
            if (!covered(b.writeStart, b.writeCount)) {
                errs.push_back({sec, "write",
                                "write range is outside server HR listen ranges",
                                -1});
            }
            if (!covered(mappedServerMirrorStart, b.mirrorCount)) {
                errs.push_back({sec, "mirror",
                                "mapped mirror range is outside server HR listen ranges",
                                -1});
            }
        }
        // Raw mirror images must come from one complete, successful poll so
        // every word belongs to the same PLC read cycle.
        if (b.mirrorCount > 0 && transports.count(b.plc)) {
            bool coveredByOnePoll = false;
            for (auto const& poll : s.pollRanges) {
                if (poll.transport != b.plc
                    || !isHoldingRegisterTable(poll.table)) {
                    continue;
                }
                if (poll.startAddress <= b.mirrorStart
                    && std::int64_t(poll.startAddress) + poll.count
                        >= std::int64_t(b.mirrorStart) + b.mirrorCount) {
                    coveredByOnePoll = true;
                    break;
                }
            }
            if (!coveredByOnePoll) {
                errs.push_back({sec, "mirror",
                    "mirror range [" + std::to_string(b.mirrorStart) + ","
                        + std::to_string(b.mirrorStart + b.mirrorCount)
                        + ") must be fully covered by one HR poll_range on plc '"
                        + b.plc + "'", -1});
            }
        }
    }

    for (size_t i = 0; i < s.sinkWindows.size(); ++i) {
        auto const& lhs = s.sinkWindows[i];
        for (size_t j = i + 1; j < s.sinkWindows.size(); ++j) {
            auto const& rhs = s.sinkWindows[j];
            if (lhs.transport == rhs.transport
                && lhs.table == rhs.table
                && rangesOverlap(lhs.startAddress, lhs.size,
                                 rhs.startAddress, rhs.size)) {
                errs.push_back(
                    {idx("sink_window", int(j)), "range",
                     "overlaps " + idx("sink_window", int(i)), -1});
            }
        }
    }

    for (size_t i = 0; i < s.bridges.size(); ++i) {
        auto const& a = s.bridges[i];
        for (size_t j = i + 1; j < s.bridges.size(); ++j) {
            auto const& b = s.bridges[j];
            if (a.server == b.server
                && (rangesOverlap(a.writeStart, a.writeCount,
                                  b.writeStart, b.writeCount)
                    || rangesOverlap(a.writeStart, a.writeCount,
                                     std::int64_t(b.mirrorStart) + b.offset,
                                     b.mirrorCount)
                    || rangesOverlap(std::int64_t(a.mirrorStart) + a.offset,
                                     a.mirrorCount, b.writeStart, b.writeCount)
                    || rangesOverlap(std::int64_t(a.mirrorStart) + a.offset,
                                     a.mirrorCount,
                                     std::int64_t(b.mirrorStart) + b.offset,
                                     b.mirrorCount))) {
                errs.push_back({idx("bridge", int(j)), "range",
                                "server address window overlaps "
                                    + idx("bridge", int(i)),
                                -1});
            }
            if (a.plc == b.plc
                && rangesOverlap(std::int64_t(a.writeStart) - a.offset,
                                 a.writeCount,
                                 std::int64_t(b.writeStart) - b.offset,
                                 b.writeCount)) {
                errs.push_back({idx("bridge", int(j)), "write",
                                "mapped PLC write range overlaps "
                                    + idx("bridge", int(i)),
                                -1});
            }
        }
    }
}

} // namespace

// ───────────────────────────────────────────────────────────────────────

std::expected<ConfigSchema, ValidationErrors>
ConfigLoader::loadFromToml(
    std::string const& path,
    std::span<std::string_view const> allowedRootExtensions) {
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

    std::set<std::string_view> knownRootKeys{
        "meta", "transport", "codec", "poll_range", "sink_window",
        "heartbeat", "ack_watch", "command", "datapoint", "route",
        "bridge", "plugin"};
    knownRootKeys.insert(
        allowedRootExtensions.begin(), allowedRootExtensions.end());
    for (auto const& [key, node] : root) {
        if (knownRootKeys.contains(key.str())) continue;
        errs.push_back(
            {"root", std::string(key.str()), "unknown field",
             int(node.source().begin.line)});
    }
    rejectWrongTypes(
        root, "root",
        {{"meta", TomlFieldType::Table},
         {"transport", TomlFieldType::Array},
         {"codec", TomlFieldType::Array},
         {"poll_range", TomlFieldType::Array},
         {"sink_window", TomlFieldType::Array},
         {"heartbeat", TomlFieldType::Array},
         {"ack_watch", TomlFieldType::Array},
         {"command", TomlFieldType::Array},
         {"datapoint", TomlFieldType::Array},
         {"route", TomlFieldType::Array},
         {"bridge", TomlFieldType::Array},
         {"plugin", TomlFieldType::Array}},
        errs);
    for (auto const extension : allowedRootExtensions) {
        auto const node = root[extension];
        if (node && !node.as_table()) {
            errs.push_back(
                {"root", std::string(extension),
                 "expected extension table",
                 int(node.node()->source().begin.line)});
        }
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

    validateValues(schema, errs);
    validateRefs(schema, errs);

    if (!errs.empty()) return std::unexpected(std::move(errs));
    return {};
}

} // namespace core::config
