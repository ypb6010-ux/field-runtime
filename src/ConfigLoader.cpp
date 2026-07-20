// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/config/ConfigLoader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
#include <string_view>

#include <QFile>
#include <QMetaType>
#include <QUrl>
#include <QTextStream>
#include <toml++/toml.hpp>

#include "core/dp/ScalarType.h"
#include "core/sched/SchedulerTypes.h"

namespace core::config {

namespace {

QString qstr(std::string_view s) {
    return QString::fromUtf8(s.data(), int(s.size()));
}

std::optional<QString> getStr(toml::table const& t, std::string_view key) {
    if (auto v = t[key].value<std::string>()) return qstr(*v);
    return std::nullopt;
}

QString getStr(toml::table const& t, std::string_view key, QString const& dflt) {
    return getStr(t, key).value_or(dflt);
}

int narrowInt(int64_t value) {
    if (value > std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    if (value < std::numeric_limits<int>::min())
        return std::numeric_limits<int>::min();
    return int(value);
}

std::optional<int> getInt(toml::table const& t, std::string_view key) {
    if (auto v = t[key].value<int64_t>()) {
        // Keep an out-of-range TOML integer out of implementation-defined
        // narrowing. The schema validation below will reject the saturated
        // sentinel for every bounded numeric field with a useful field name.
        return narrowInt(*v);
    }
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
                 QString const& section, QString& out,
                 ValidationErrors& errs) {
    auto v = getStr(t, key);
    if (!v) {
        errs.push_back({section, qstr(key),
                        QStringLiteral("missing required string field"),
                        int(t.source().begin.line)});
        return;
    }
    out = *v;
}

dp::ScalarType parseScalarType(QString const& s, bool& ok) {
    ok = true;
    static const QHash<QString, dp::ScalarType> map = {
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
    return *it;
}

std::optional<sched::Priority> parsePriority(QString const& s) {
    if (s == "Low")      return sched::Priority::Low;
    if (s == "Normal")   return sched::Priority::Normal;
    if (s == "High")     return sched::Priority::High;
    if (s == "Critical") return sched::Priority::Critical;
    return std::nullopt;
}

std::optional<sched::SchedulerKind> parseSchedulerKind(QString const& s) {
    if (s == "serial")   return sched::SchedulerKind::Serial;
    if (s == "credit")   return sched::SchedulerKind::Credit;
    if (s == "priority") return sched::SchedulerKind::Priority;
    return std::nullopt;
}

transport::TransportKind parseTransportKind(QString const& s, bool& ok) {
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

void rejectUnknownKeys(toml::table const& t, QString const& section,
                       std::initializer_list<std::string_view> allowed,
                       ValidationErrors& errs) {
    std::set<std::string_view> const known(allowed.begin(), allowed.end());
    for (auto const& [key, node] : t) {
        if (known.contains(key.str())) continue;
        errs.push_back({section, qstr(key.str()), QStringLiteral("unknown field"),
                        int(node.source().begin.line)});
    }
}

enum class TomlFieldType { String, Integer, Number, Boolean, Array, Table };

struct TomlFieldSpec {
    std::string_view key;
    TomlFieldType    type;
};

void rejectWrongTypes(toml::table const& t, QString const& section,
                      std::initializer_list<TomlFieldSpec> specs,
                      ValidationErrors& errs) {
    for (auto const& spec : specs) {
        auto const node = t[spec.key];
        if (!node) continue;
        bool ok = false;
        QString expected;
        switch (spec.type) {
            case TomlFieldType::String:
                ok = node.as_string() != nullptr; expected = QStringLiteral("string"); break;
            case TomlFieldType::Integer:
                ok = node.as_integer() != nullptr; expected = QStringLiteral("integer"); break;
            case TomlFieldType::Number:
                ok = node.as_integer() != nullptr || node.as_floating_point() != nullptr;
                expected = QStringLiteral("number"); break;
            case TomlFieldType::Boolean:
                ok = node.as_boolean() != nullptr; expected = QStringLiteral("boolean"); break;
            case TomlFieldType::Array:
                ok = node.as_array() != nullptr; expected = QStringLiteral("array"); break;
            case TomlFieldType::Table:
                ok = node.as_table() != nullptr; expected = QStringLiteral("table"); break;
        }
        if (!ok) {
            errs.push_back({section, qstr(spec.key),
                QStringLiteral("expected %1").arg(expected),
                int(node.node()->source().begin.line)});
        }
    }
}

// ─── Section parsers ────────────────────────────────────────────────────

MetaConfig parseMeta(toml::table const& root, ValidationErrors& errs) {
    MetaConfig m;
    if (auto t = root["meta"].as_table()) {
        rejectUnknownKeys(*t, QStringLiteral("meta"),
                          {"project", "version", "generated", "log_level"}, errs);
        rejectWrongTypes(*t, QStringLiteral("meta"),
            {{"project", TomlFieldType::String}, {"version", TomlFieldType::String},
             {"generated", TomlFieldType::String}, {"log_level", TomlFieldType::String}},
            errs);
        m.project   = getStr(*t, "project",   {});
        m.version   = getStr(*t, "version",   {});
        m.generated = getStr(*t, "generated", {});
        m.logLevel  = getStr(*t, "log_level", {});
    } else if (root["meta"]) {
        errs.push_back({QStringLiteral("meta"), QStringLiteral("meta"),
                        QStringLiteral("expected table"),
                        int(root.source().begin.line)});
    }
    return m;
}

TransportConfig parseTransport(toml::table const& t,
                                int                index,
                                ValidationErrors&   errs) {
    auto const section = QStringLiteral("transport[%1]").arg(index);
    rejectUnknownKeys(t, section,
        {"id", "kind", "host", "port", "slave_id", "listen_address",
         "listen_port", "max_clients", "port_name", "baud_rate", "data_bits",
         "stop_bits", "parity", "endpoint_url", "security_policy", "username",
         "password", "backend", "node_id_template", "client_id", "broker_uri",
         "topic_prefix", "topic_template", "qos", "clean_session", "rack", "slot",
         "reconnect_interval_ms", "connect_timeout_ms", "request_timeout_ms",
         "scheduler", "listen_ranges"}, errs);
    rejectWrongTypes(t, section,
        {{"id", TomlFieldType::String}, {"kind", TomlFieldType::String},
         {"host", TomlFieldType::String}, {"port", TomlFieldType::Integer},
         {"slave_id", TomlFieldType::Integer},
         {"listen_address", TomlFieldType::String},
         {"listen_port", TomlFieldType::Integer},
         {"max_clients", TomlFieldType::Integer},
         {"port_name", TomlFieldType::String}, {"baud_rate", TomlFieldType::Integer},
         {"data_bits", TomlFieldType::Integer}, {"stop_bits", TomlFieldType::Integer},
         {"parity", TomlFieldType::String}, {"endpoint_url", TomlFieldType::String},
         {"security_policy", TomlFieldType::String}, {"username", TomlFieldType::String},
         {"password", TomlFieldType::String}, {"backend", TomlFieldType::String},
         {"node_id_template", TomlFieldType::String}, {"client_id", TomlFieldType::String},
         {"broker_uri", TomlFieldType::String}, {"topic_prefix", TomlFieldType::String},
         {"topic_template", TomlFieldType::String}, {"qos", TomlFieldType::Integer},
         {"clean_session", TomlFieldType::Boolean}, {"rack", TomlFieldType::Integer},
         {"slot", TomlFieldType::Integer},
         {"reconnect_interval_ms", TomlFieldType::Integer},
         {"connect_timeout_ms", TomlFieldType::Integer},
         {"request_timeout_ms", TomlFieldType::Integer},
         {"scheduler", TomlFieldType::Table}, {"listen_ranges", TomlFieldType::Array}},
        errs);
    TransportConfig c;
    requireStr(t, "id",   section, c.id,   errs);

    QString kindStr;
    requireStr(t, "kind", section, kindStr, errs);
    bool ok = true;
    c.kind = parseTransportKind(kindStr, ok);
    if (!ok && !kindStr.isEmpty()) {
        errs.push_back({section, "kind",
                        QStringLiteral("unknown transport kind '%1'").arg(kindStr),
                        int(t.source().begin.line)});
    }
    auto rejectUnless = [&](std::string_view field, bool appliesToKind) {
        if (t[field] && !appliesToKind) {
            errs.push_back({section, qstr(field),
                QStringLiteral("field is not applicable to transport kind '%1'")
                    .arg(kindStr), int(t.source().begin.line)});
        }
    };
    bool const tcpClient = c.kind == transport::TransportKind::ModbusTcpClient;
    bool const tcpServer = c.kind == transport::TransportKind::ModbusTcpServer;
    bool const rtu       = c.kind == transport::TransportKind::ModbusRtu;
    bool const opc       = c.kind == transport::TransportKind::OpcUaClient;
    bool const mqtt      = c.kind == transport::TransportKind::MqttClient
                        || c.kind == transport::TransportKind::MqttPahoClient;
    bool const s7        = c.kind == transport::TransportKind::S7Client;
    rejectUnless("host", tcpClient || s7);
    rejectUnless("port", tcpClient || s7);
    rejectUnless("slave_id", tcpClient || tcpServer || rtu);
    rejectUnless("listen_address", tcpServer);
    rejectUnless("listen_port", tcpServer);
    rejectUnless("listen_ranges", tcpServer);
    rejectUnless("port_name", rtu);
    rejectUnless("baud_rate", rtu);
    rejectUnless("data_bits", rtu);
    rejectUnless("stop_bits", rtu);
    rejectUnless("parity", rtu);
    rejectUnless("endpoint_url", opc);
    rejectUnless("security_policy", opc);
    rejectUnless("backend", opc);
    rejectUnless("node_id_template", opc);
    rejectUnless("username", opc || mqtt);
    rejectUnless("password", opc || mqtt);
    rejectUnless("client_id", mqtt);
    rejectUnless("broker_uri", mqtt);
    rejectUnless("topic_prefix", mqtt);
    rejectUnless("topic_template", mqtt);
    rejectUnless("qos", mqtt);
    rejectUnless("clean_session", mqtt);
    rejectUnless("rack", s7);
    rejectUnless("slot", s7);

    c.host           = getStr(t, "host", {});
    c.port           = getInt(t, "port",
                              c.kind == transport::TransportKind::S7Client
                                  ? 102 : 502);
    c.slaveId        = getInt(t, "slave_id", 1);
    c.listenAddress  = getStr(t, "listen_address", QStringLiteral("0.0.0.0"));
    c.listenPort     = getInt(t, "listen_port", 502);
    if (t["max_clients"]) {
        errs.push_back({section, "max_clients",
            QStringLiteral("unsupported: Qt Modbus TCP server does not expose a client limit"),
            int(t.source().begin.line)});
    }
    // Modbus RTU
    c.portName       = getStr(t, "port_name",  {});
    c.baudRate       = getInt(t, "baud_rate",  9600);
    c.dataBits       = getInt(t, "data_bits",  8);
    c.stopBits       = getInt(t, "stop_bits",  1);
    c.parity         = getStr(t, "parity",     QStringLiteral("none"));
    // OPC UA
    c.endpointUrl    = getStr(t, "endpoint_url",    {});
    c.securityPolicy = getStr(t, "security_policy", QStringLiteral("None"));
    c.username       = getStr(t, "username",        {});
    c.password       = getStr(t, "password",        {});
    c.opcuaBackend   = getStr(t, "backend",         QStringLiteral("open62541"));
    c.nodeIdTemplate = getStr(t, "node_id_template", QStringLiteral("ns=2;s=Var_%1"));
    // MQTT
    c.clientId       = getStr(t, "client_id",     {});
    c.brokerUri      = getStr(t, "broker_uri",    {});
    c.topicPrefix    = getStr(t, "topic_prefix",  {});
    c.topicTemplate  = getStr(t, "topic_template", QStringLiteral("reg/%1"));
    c.qos            = getInt(t, "qos",           1);
    c.cleanSession   = getBool(t, "clean_session").value_or(true);
    // S7
    c.rack           = getInt(t, "rack", 0);
    c.slot           = getInt(t, "slot", 1);
    // Common
    c.reconnectIntervalMs = getInt(t, "reconnect_interval_ms", 15000);
    c.connectTimeoutMs    = getInt(t, "connect_timeout_ms",    3000);
    c.requestTimeoutMs    = getInt(t, "request_timeout_ms",    1000);
    if (t["request_timeout_ms"]
        && c.kind == transport::TransportKind::ModbusTcpServer) {
        errs.push_back({section, "request_timeout_ms",
            QStringLiteral("not applicable to a Modbus TCP server"),
            int(t.source().begin.line)});
    }
    if (t["request_timeout_ms"]
        && c.kind == transport::TransportKind::MqttClient) {
        errs.push_back({section, "request_timeout_ms",
            QStringLiteral("Qt MQTT publish is asynchronous and has no request timeout"),
            int(t.source().begin.line)});
    }

    if (auto s = t["scheduler"].as_table()) {
        rejectUnknownKeys(*s, section + QStringLiteral(".scheduler"),
            {"kind", "default_timeout_ms", "inter_request_gap_ms",
             "max_queue_depth", "max_inflight", "starvation_guard_ms",
             "fifo_within_lane", "circuit_breaker_threshold",
             "circuit_breaker_open_ms"}, errs);
        rejectWrongTypes(*s, section + QStringLiteral(".scheduler"),
            {{"kind", TomlFieldType::String},
             {"default_timeout_ms", TomlFieldType::Integer},
             {"inter_request_gap_ms", TomlFieldType::Integer},
             {"max_queue_depth", TomlFieldType::Integer},
             {"max_inflight", TomlFieldType::Integer},
             {"starvation_guard_ms", TomlFieldType::Integer},
             {"fifo_within_lane", TomlFieldType::Boolean},
             {"circuit_breaker_threshold", TomlFieldType::Integer},
             {"circuit_breaker_open_ms", TomlFieldType::Integer}}, errs);
        QString const schedulerKind = getStr(*s, "kind", "serial");
        if (auto parsed = parseSchedulerKind(schedulerKind)) {
            c.scheduler.kind = *parsed;
        } else {
            errs.push_back({section + ".scheduler", "kind",
                QStringLiteral("unknown scheduler kind '%1'").arg(schedulerKind),
                int(s->source().begin.line)});
        }
        if ((*s)["default_timeout_ms"]) {
            errs.push_back({section + ".scheduler", "default_timeout_ms",
                QStringLiteral("unsupported: use transport.request_timeout_ms"),
                int(s->source().begin.line)});
        }
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
                rejectUnknownKeys(*wt, section + QStringLiteral(".listen_ranges"),
                                  {"table", "range"}, errs);
                rejectWrongTypes(*wt, section + QStringLiteral(".listen_ranges"),
                    {{"table", TomlFieldType::String},
                     {"range", TomlFieldType::Array}}, errs);
                transport::WatchRange r;
                QString const tbl = getStr(*wt, "table", QStringLiteral("HR"));
                if (tbl == "HR" || tbl == "HoldingRegisters")
                    r.table = QModbusDataUnit::HoldingRegisters;
                else if (tbl == "IR" || tbl == "InputRegisters")
                    r.table = QModbusDataUnit::InputRegisters;
                else if (tbl == "Coil" || tbl == "Coils")
                    r.table = QModbusDataUnit::Coils;
                else if (tbl == "DI" || tbl == "DiscreteInputs")
                    r.table = QModbusDataUnit::DiscreteInputs;
                else {
                    r.table = QModbusDataUnit::HoldingRegisters;
                    errs.push_back({section + ".listen_ranges", "table",
                        QStringLiteral("unknown register table '%1'").arg(tbl),
                        int(wt->source().begin.line)});
                }
                if (auto arr = (*wt)["range"].as_array(); arr && arr->size() == 2) {
                    auto const start = arr->at(0).value<int64_t>();
                    auto const size  = arr->at(1).value<int64_t>();
                    if (!start || !size) {
                        errs.push_back({section + QStringLiteral(".listen_ranges"),
                            "range", QStringLiteral("elements must be integers"),
                            int(arr->source().begin.line)});
                    } else {
                        r.startAddress = narrowInt(*start);
                        r.size         = narrowInt(*size);
                    }
                } else if ((*wt)["range"]) {
                    errs.push_back({section + QStringLiteral(".listen_ranges"),
                        "range", QStringLiteral("expected [start, size] array"),
                        int(wt->source().begin.line)});
                }
                c.listenRanges.append(r);
            } else {
                errs.push_back({section + QStringLiteral(".listen_ranges"),
                                QStringLiteral("item"),
                                QStringLiteral("expected table"),
                                int(n.source().begin.line)});
            }
        }
    }
    return c;
}

CodecConfig parseCodec(toml::table const& t,
                        int                index,
                        ValidationErrors&   errs) {
    auto const section = QStringLiteral("codec[%1]").arg(index);
    rejectUnknownKeys(t, section, {"id", "kind", "script", "arg", "map"}, errs);
    rejectWrongTypes(t, section,
        {{"id", TomlFieldType::String}, {"kind", TomlFieldType::String},
         {"script", TomlFieldType::String}, {"arg", TomlFieldType::String},
         {"map", TomlFieldType::Table}}, errs);
    CodecConfig c;
    requireStr(t, "id",   section, c.id,   errs);
    requireStr(t, "kind", section, c.kind, errs);
    c.script = getStr(t, "script", {});
    c.arg    = getStr(t, "arg", {});
    if (auto m = t["map"].as_table()) {
        for (auto&& [k, v] : *m) {
            auto key = qstr(k.str());
            QVariant val;
            if (auto sv = v.value<std::string>())   val = qstr(*sv);
            else if (auto iv = v.value<int64_t>())  val = QVariant(qint64(*iv));
            else if (auto bv = v.value<bool>())     val = *bv;
            c.map.insert(key, val);
        }
    }
    return c;
}

PollRangeConfig parsePollRange(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = QStringLiteral("poll_range[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "table", "range", "period_ms",
                       "priority"}, errs);
    rejectWrongTypes(t, section,
        {{"module_id", TomlFieldType::String}, {"transport", TomlFieldType::String},
         {"table", TomlFieldType::String}, {"range", TomlFieldType::Array},
         {"period_ms", TomlFieldType::Integer}, {"priority", TomlFieldType::String}},
        errs);
    PollRangeConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        auto const start = arr->at(0).value<int64_t>();
        auto const count = arr->at(1).value<int64_t>();
        if (!start || !count) {
            errs.push_back({section, "range", QStringLiteral("elements must be integers"),
                            int(arr->source().begin.line)});
        } else {
            c.startAddress = narrowInt(*start);
            c.count        = narrowInt(*count);
        }
    } else {
        errs.push_back({section, "range",
                        QStringLiteral("expected [start, count] array"),
                        int(t.source().begin.line)});
    }
    c.periodMs = getInt(t, "period_ms", 0);
    if (c.periodMs <= 0) {
        errs.push_back({section, "period_ms",
                        QStringLiteral("period_ms must be > 0"),
                        int(t.source().begin.line)});
    }
    QString const priority = getStr(t, "priority", "Normal");
    if (auto parsed = parsePriority(priority)) c.priority = *parsed;
    else errs.push_back({section, "priority",
        QStringLiteral("unknown priority '%1'").arg(priority),
        int(t.source().begin.line)});
    return c;
}

QList<quint16> parseU16Array(toml::array const& arr, QString const& section,
                             QString const& field, ValidationErrors& errs) {
    QList<quint16> out;
    int index = 0;
    for (auto&& n : arr) {
        if (auto i = n.value<int64_t>(); i && *i >= 0 && *i <= 65535) {
            out.append(quint16(*i));
        } else {
            errs.push_back({section, field,
                QStringLiteral("element %1 must be an integer in [0, 65535]")
                    .arg(index),
                int(n.source().begin.line)});
        }
        ++index;
    }
    return out;
}

SinkWindowConfig parseSinkWindow(toml::table const& t,
                                   int                index,
                                   ValidationErrors&   errs) {
    auto const section = QStringLiteral("sink_window[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "table", "range", "priority",
                       "flush", "initial"}, errs);
    rejectWrongTypes(t, section,
        {{"module_id", TomlFieldType::String}, {"transport", TomlFieldType::String},
         {"table", TomlFieldType::String}, {"range", TomlFieldType::Array},
         {"priority", TomlFieldType::String}, {"flush", TomlFieldType::Table},
         {"initial", TomlFieldType::Array}}, errs);
    SinkWindowConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        auto const start = arr->at(0).value<int64_t>();
        auto const size  = arr->at(1).value<int64_t>();
        if (!start || !size) {
            errs.push_back({section, "range", QStringLiteral("elements must be integers"),
                            int(arr->source().begin.line)});
        } else {
            c.startAddress = narrowInt(*start);
            c.size         = narrowInt(*size);
        }
    } else {
        errs.push_back({section, "range",
                        QStringLiteral("expected [start, size] array"),
                        int(t.source().begin.line)});
    }
    if (c.size <= 0) {
        errs.push_back({section, "range",
                        QStringLiteral("size must be > 0"),
                        int(t.source().begin.line)});
    }
    QString const priority = getStr(t, "priority", "High");
    if (auto parsed = parsePriority(priority)) c.priority = *parsed;
    else errs.push_back({section, "priority",
        QStringLiteral("unknown priority '%1'").arg(priority),
        int(t.source().begin.line)});
    if (auto f = t["flush"].as_table()) {
        rejectUnknownKeys(*f, section + QStringLiteral(".flush"),
                          {"debounce_ms", "keepalive_ms", "coalesce",
                           "max_retries"}, errs);
        rejectWrongTypes(*f, section + QStringLiteral(".flush"),
            {{"debounce_ms", TomlFieldType::Integer},
             {"keepalive_ms", TomlFieldType::Integer},
             {"coalesce", TomlFieldType::Boolean},
             {"max_retries", TomlFieldType::Integer}}, errs);
        c.flush.debounceMs     = getInt(*f, "debounce_ms", 20);
        c.flush.keepaliveMs    = getInt(*f, "keepalive_ms", 0);
        if ((*f)["coalesce"]) {
            errs.push_back({section + ".flush", "coalesce",
                QStringLiteral("unsupported: SinkWindow already merges staged values into one snapshot and allows only one async flush in flight"),
                int(f->source().begin.line)});
        }
        if ((*f)["max_retries"]) {
            errs.push_back({section + ".flush", "max_retries",
                QStringLiteral("unsupported: SinkWindow retries failed flushes until success"),
                int(f->source().begin.line)});
        }
    }
    if (auto arr = t["initial"].as_array()) {
        c.initial = parseU16Array(*arr, section, "initial", errs);
    }
    return c;
}

HeartbeatConfig parseHeartbeat(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = QStringLiteral("heartbeat[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "table", "address", "values",
                       "value", "period_ms", "priority", "incrementer"}, errs);
    rejectWrongTypes(t, section,
        {{"module_id", TomlFieldType::String}, {"transport", TomlFieldType::String},
         {"table", TomlFieldType::String}, {"address", TomlFieldType::Integer},
         {"values", TomlFieldType::Array}, {"value", TomlFieldType::Integer},
         {"period_ms", TomlFieldType::Integer}, {"priority", TomlFieldType::String},
         {"incrementer", TomlFieldType::String}}, errs);
    HeartbeatConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    c.table       = getStr(t, "table", QStringLiteral("HR"));
    c.address     = getInt(t, "address", 0);
    if (auto arr = t["values"].as_array()) {
        c.values = parseU16Array(*arr, section, "values", errs);
    } else if (auto v = getInt(t, "value")) {
        if (*v >= 0 && *v <= 65535) c.values.append(quint16(*v));
        else errs.push_back({section, "value",
            QStringLiteral("must be in [0, 65535]"),
            int(t.source().begin.line)});
    }
    if (c.values.isEmpty()) {
        errs.push_back({section, "values",
                        QStringLiteral("heartbeat requires non-empty values"),
                        int(t.source().begin.line)});
    }
    c.periodMs    = getInt(t, "period_ms", 0);
    if (c.periodMs <= 0) {
        errs.push_back({section, "period_ms",
                        QStringLiteral("period_ms must be > 0"),
                        int(t.source().begin.line)});
    }
    QString const priority = getStr(t, "priority", "Low");
    if (auto parsed = parsePriority(priority)) c.priority = *parsed;
    else errs.push_back({section, "priority",
        QStringLiteral("unknown priority '%1'").arg(priority),
        int(t.source().begin.line)});
    c.incrementer = getStr(t, "incrementer", QStringLiteral("none"));
    return c;
}

AckWatchConfig parseAckWatch(toml::table const& t,
                               int                index,
                               ValidationErrors&   errs) {
    auto const section = QStringLiteral("ack_watch[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"module_id", "dp", "timeout_ms", "expected"}, errs);
    rejectWrongTypes(t, section,
        {{"module_id", TomlFieldType::String}, {"dp", TomlFieldType::String},
         {"timeout_ms", TomlFieldType::Integer}}, errs);
    AckWatchConfig c;
    requireStr(t, "module_id", section, c.moduleId, errs);
    requireStr(t, "dp",        section, c.dp,       errs);
    c.timeoutMs = getInt(t, "timeout_ms", 3000);
    if (auto v = t["expected"]; v) {
        if (auto i = v.value<int64_t>())          c.expected = qint64(*i);
        else if (auto b = v.value<bool>())        c.expected = *b;
        else if (auto s = v.value<std::string>()) c.expected = qstr(*s);
        else if (auto d = v.value<double>())      c.expected = *d;
    }
    return c;
}

CommandConfig parseCommand(toml::table const& t,
                             int                index,
                             ValidationErrors&   errs) {
    auto const section = QStringLiteral("command[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"module_id", "transport", "priority", "interruptable",
                       "trigger", "writes"}, errs);
    rejectWrongTypes(t, section,
        {{"module_id", TomlFieldType::String}, {"transport", TomlFieldType::String},
         {"priority", TomlFieldType::String}, {"interruptable", TomlFieldType::Boolean},
         {"writes", TomlFieldType::Array}}, errs);
    CommandConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    QString const priority = getStr(t, "priority", "High");
    if (auto parsed = parsePriority(priority)) c.priority = *parsed;
    else errs.push_back({section, "priority",
        QStringLiteral("unknown priority '%1'").arg(priority),
        int(t.source().begin.line)});
    c.interruptable = getBool(t, "interruptable").value_or(false);
    if (t["trigger"]) {
        errs.push_back({section, "trigger",
            QStringLiteral("automatic command triggers are not implemented; invoke the Command module explicitly"),
            int(t.source().begin.line)});
    }
    if (auto arr = t["writes"].as_array()) {
        int wi = 0;
        for (auto&& n : *arr) {
            if (auto wt = n.as_table()) {
                auto const writeSection = section
                    + QStringLiteral(".writes[%1]").arg(wi);
                rejectUnknownKeys(*wt, writeSection,
                                  {"table", "address", "value"}, errs);
                rejectWrongTypes(*wt, writeSection,
                    {{"table", TomlFieldType::String},
                     {"address", TomlFieldType::Integer},
                     {"value", TomlFieldType::Integer}}, errs);
                CommandWriteEntry e;
                e.table   = getStr(*wt, "table", QStringLiteral("HR"));
                e.address = getInt(*wt, "address", 0);
                int const value = getInt(*wt, "value", 0);
                if (value < 0 || value > 65535) {
                    errs.push_back({section + QStringLiteral(".writes[%1]").arg(wi),
                        "value", QStringLiteral("must be in [0, 65535]"),
                        int(wt->source().begin.line)});
                } else {
                    e.value = quint16(value);
                }
                c.writes.append(e);
            } else {
                errs.push_back({section + QStringLiteral(".writes[%1]").arg(wi),
                                QStringLiteral("item"),
                                QStringLiteral("expected table"),
                                int(n.source().begin.line)});
            }
            ++wi;
        }
    }
    if (c.writes.isEmpty()) {
        errs.push_back({section, "writes",
                        QStringLiteral("command requires non-empty writes"),
                        int(t.source().begin.line)});
    }
    return c;
}

RouteConfig parseRoute(toml::table const& t,
                        int                index,
                        ValidationErrors&   errs) {
    auto const section = QStringLiteral("route[%1]").arg(index);
    rejectUnknownKeys(t, section, {"name", "from", "to", "policy"}, errs);
    rejectWrongTypes(t, section,
        {{"name", TomlFieldType::String}, {"from", TomlFieldType::String},
         {"to", TomlFieldType::String}, {"policy", TomlFieldType::String}}, errs);
    RouteConfig c;
    c.name   = getStr(t, "name", {});
    requireStr(t, "from", section, c.from, errs);
    requireStr(t, "to",   section, c.to,   errs);
    c.policy = getStr(t, "policy", QStringLiteral("ContinuousMirror"));
    return c;
}

BridgeConfig parseBridge(toml::table const& t,
                          int                index,
                          ValidationErrors&   errs) {
    auto const section = QStringLiteral("bridge[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"server", "plc", "offset", "write_start", "write_count",
                       "mirror_start", "mirror_count", "mirror_policy",
                       "mirror_period_ms"}, errs);
    rejectWrongTypes(t, section,
        {{"server", TomlFieldType::String}, {"plc", TomlFieldType::String},
         {"offset", TomlFieldType::Integer}, {"write_start", TomlFieldType::Integer},
         {"write_count", TomlFieldType::Integer},
         {"mirror_start", TomlFieldType::Integer},
         {"mirror_count", TomlFieldType::Integer},
         {"mirror_policy", TomlFieldType::String},
         {"mirror_period_ms", TomlFieldType::Integer}}, errs);
    BridgeConfig c;
    requireStr(t, "server", section, c.server, errs);
    requireStr(t, "plc",    section, c.plc,    errs);
    c.offset         = getInt(t, "offset", 0);
    c.writeStart     = getInt(t, "write_start", 0);
    c.writeCount     = getInt(t, "write_count", 0);
    c.mirrorStart    = getInt(t, "mirror_start", 0);
    c.mirrorCount    = getInt(t, "mirror_count", 0);
    QString const mirrorPolicy = getStr(t, "mirror_policy", {});
    if (c.mirrorCount > 0 && mirrorPolicy.isEmpty()) {
        errs.push_back({section, "mirror_policy",
            QStringLiteral("is required when mirror_count is positive"), -1});
    } else if (mirrorPolicy == QStringLiteral("AfterPoll")) {
        c.mirrorPolicy = BridgeMirrorPolicy::AfterPoll;
    } else if (mirrorPolicy == QStringLiteral("Periodic")) {
        c.mirrorPolicy = BridgeMirrorPolicy::Periodic;
    } else if (!mirrorPolicy.isEmpty()) {
        errs.push_back({section, "mirror_policy",
            QStringLiteral("must be exactly 'AfterPoll' or 'Periodic'"), -1});
    }
    c.mirrorPeriodMs = getInt(t, "mirror_period_ms", 0);
    return c;
}

PluginConfig parsePlugin(toml::table const& t,
                          int                index,
                          ValidationErrors&   errs) {
    auto const section = QStringLiteral("plugin[%1]").arg(index);
    rejectUnknownKeys(t, section, {"dll", "name", "config"}, errs);
    rejectWrongTypes(t, section,
        {{"dll", TomlFieldType::String}, {"name", TomlFieldType::String}}, errs);
    PluginConfig c;
    requireStr(t, "dll", section, c.dllPath, errs);
    c.name   = getStr(t, "name",   {});
    if (t["config"]) {
        errs.push_back({section, "config",
            QStringLiteral("plugin config payloads are not implemented by the Plugin interface"),
            int(t.source().begin.line)});
    }
    return c;
}

PortRefConfig parsePortRef(toml::table const& t,
                             QString const&     section,
                             ValidationErrors&   errs) {
    PortRefConfig p;
    rejectUnknownKeys(t, section,
                      {"port", "table", "addr", "bit", "wordOrder", "shift",
                       "mask", "scale", "offset", "codec", "dedupe", "window"},
                      errs);
    rejectWrongTypes(t, section,
        {{"port", TomlFieldType::String}, {"table", TomlFieldType::String},
         {"addr", TomlFieldType::Integer}, {"bit", TomlFieldType::Integer},
         {"wordOrder", TomlFieldType::String}, {"shift", TomlFieldType::Integer},
         {"mask", TomlFieldType::Integer}, {"scale", TomlFieldType::Number},
         {"offset", TomlFieldType::Number}, {"codec", TomlFieldType::String},
         {"dedupe", TomlFieldType::Boolean}, {"window", TomlFieldType::String}}, errs);
    p.port    = getStr(t, "port",    {});
    p.table   = getStr(t, "table",   {});
    p.address = getInt(t, "addr",    0);
    p.bit     = getInt(t, "bit",     -1);
    p.wordOrder = getStr(t, "wordOrder", {});
    p.shift   = getInt(t, "shift",   0);
    if (auto m = t["mask"].value<int64_t>()) {
        if (*m < 0) {
            errs.push_back({section, "mask",
                            QStringLiteral("mask must be >= 0"),
                            int(t.source().begin.line)});
        } else {
            p.mask = quint64(*m);
        }
    }
    p.scale   = getDouble(t, "scale", 1.0);
    p.offset  = getDouble(t, "offset", 0.0);
    p.codec   = getStr(t, "codec", {});
    if (t["dedupe"]) {
        errs.push_back({section, "dedupe",
            QStringLiteral("port-level dedupe is not implemented"),
            int(t.source().begin.line)});
    }
    p.window  = getStr(t, "window", {});

    return p;
}

DatapointConfig parseDatapoint(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = QStringLiteral("datapoint[%1]").arg(index);
    rejectUnknownKeys(t, section,
                      {"id", "kind", "type", "source", "sink", "policy", "ui",
                       "persist", "ack"}, errs);
    rejectWrongTypes(t, section,
        {{"id", TomlFieldType::String}, {"kind", TomlFieldType::String},
         {"type", TomlFieldType::String}, {"source", TomlFieldType::Table},
         {"sink", TomlFieldType::Table}, {"policy", TomlFieldType::String},
         {"ui", TomlFieldType::String}, {"persist", TomlFieldType::String},
         {"ack", TomlFieldType::Table}}, errs);
    DatapointConfig c;
    requireStr(t, "id",   section, c.id,   errs);
    c.kind = getStr(t, "kind", "Status");
    QString typeStr = getStr(t, "type", "U16");
    bool typeOk = true;
    c.type = parseScalarType(typeStr, typeOk);
    if (!typeOk) {
        errs.push_back({section, "type",
                        QStringLiteral("unknown ScalarType '%1'").arg(typeStr),
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

    if (auto a = t["ack"].as_table()) {
        rejectUnknownKeys(*a, section + QStringLiteral(".ack"),
                          {"dp", "timeout_ms", "expected"}, errs);
        rejectWrongTypes(*a, section + QStringLiteral(".ack"),
            {{"dp", TomlFieldType::String},
             {"timeout_ms", TomlFieldType::Integer}}, errs);
        c.ack.dp        = getStr(*a, "dp", {});
        c.ack.timeoutMs = getInt(*a, "timeout_ms", 3000);
        // expected stays as QVariant via direct extraction
        if (auto v = (*a)["expected"]; v) {
            if (auto i = v.value<int64_t>())       c.ack.expected = qint64(*i);
            else if (auto b = v.value<bool>())     c.ack.expected = *b;
            else if (auto s = v.value<std::string>()) c.ack.expected = qstr(*s);
            else if (auto d = v.value<double>())   c.ack.expected = *d;
        }
        c.hasAck = true;
    }
    return c;
}

template <class Section, class Fn>
void parseArray(toml::table const& root, std::string_view key,
                 QList<Section>& out, Fn parseOne, ValidationErrors& errs) {
    if (auto arr = root[key].as_array()) {
        int i = 0;
        for (auto&& node : *arr) {
            if (auto t = node.as_table()) {
                out.append(parseOne(*t, i, errs));
            } else {
                errs.push_back({qstr(key) + QStringLiteral("[%1]").arg(i),
                                QStringLiteral("item"),
                                QStringLiteral("expected table"),
                                int(node.source().begin.line)});
            }
            ++i;
        }
    } else if (root[key]) {
        errs.push_back({qstr(key), qstr(key), QStringLiteral("expected array"),
                        int(root.source().begin.line)});
    }
}

// ─── Validation ────────────────────────────────────────────────────────

void checkUnique(QList<QString> const& ids, QString const& section,
                  QString const& field, ValidationErrors& errs) {
    std::set<QString> seen;
    for (auto const& id : ids) {
        if (!seen.insert(id).second) {
            errs.push_back({section, field,
                            QStringLiteral("duplicate id '%1'").arg(id), -1});
        }
    }
}

// Built-in codec IDs that ConfigLoader::validate accepts without a [[codec]]
// declaration — these are registered at CodecRegistry::loadBuiltins time.
bool isBuiltinCodecId(QString const& id) {
    static const std::set<QString> ids = {
        QStringLiteral("builtin.bool"),
        QStringLiteral("builtin.u16"),
        QStringLiteral("builtin.s16"),
        QStringLiteral("builtin.u32"),
        QStringLiteral("builtin.s32"),
        QStringLiteral("builtin.f32"),
        QStringLiteral("builtin.u64"),
        QStringLiteral("builtin.s64"),
        QStringLiteral("builtin.f64"),
        QStringLiteral("builtin.enumu16"),
    };
    return ids.contains(id);
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
        case dp::ScalarType::String:  return 64;
    }
    return 64;
}

bool isKnownTable(QString const& table) {
    return table == QStringLiteral("HR")
        || table == QStringLiteral("HoldingRegisters")
        || table == QStringLiteral("IR")
        || table == QStringLiteral("InputRegisters")
        || table == QStringLiteral("Coil")
        || table == QStringLiteral("Coils")
        || table == QStringLiteral("DI")
        || table == QStringLiteral("DiscreteInputs");
}

QString canonicalTable(QString const& table) {
    if (table == QStringLiteral("HoldingRegisters")) return QStringLiteral("HR");
    if (table == QStringLiteral("InputRegisters"))   return QStringLiteral("IR");
    if (table == QStringLiteral("Coils"))            return QStringLiteral("Coil");
    if (table == QStringLiteral("DiscreteInputs"))   return QStringLiteral("DI");
    return table;
}

QString canonicalTable(QModbusDataUnit::RegisterType table) {
    switch (table) {
        case QModbusDataUnit::HoldingRegisters: return QStringLiteral("HR");
        case QModbusDataUnit::InputRegisters:   return QStringLiteral("IR");
        case QModbusDataUnit::Coils:            return QStringLiteral("Coil");
        case QModbusDataUnit::DiscreteInputs:   return QStringLiteral("DI");
        default:                                return {};
    }
}

bool isWritableTable(QString const& table) {
    return table == QStringLiteral("HR")
        || table == QStringLiteral("HoldingRegisters")
        || table == QStringLiteral("Coil")
        || table == QStringLiteral("Coils");
}

int maxReadCount(QString const& table) {
    return (table == QStringLiteral("Coil")
         || table == QStringLiteral("Coils")
         || table == QStringLiteral("DI")
         || table == QStringLiteral("DiscreteInputs")) ? 2000 : 125;
}

int maxWriteCount(QString const& table) {
    return (table == QStringLiteral("Coil")
         || table == QStringLiteral("Coils")) ? 1968 : 123;
}

bool usesModbusClientRequestLimits(transport::TransportKind kind) {
    return kind == transport::TransportKind::ModbusTcpClient
        || kind == transport::TransportKind::ModbusRtu;
}

bool rangesOverlap(qint64 aStart, qint64 aCount,
                   qint64 bStart, qint64 bCount) {
    return aCount > 0 && bCount > 0
        && aStart < bStart + bCount
        && bStart < aStart + aCount;
}

void validateRange(QString const& section, QString const& field,
                   int start, int count, int maxCount,
                   ValidationErrors& errs) {
    if (start < 0 || start > 65535) {
        errs.push_back({section, field,
            QStringLiteral("start address must be in [0, 65535]"), -1});
    }
    if (count <= 0 || count > maxCount) {
        errs.push_back({section, field,
            QStringLiteral("count must be in [1, %1]").arg(maxCount), -1});
    }
    if (start >= 0 && count > 0
     && qint64(start) + qint64(count) > 65536) {
        errs.push_back({section, field,
            QStringLiteral("range exceeds address 65535"), -1});
    }
}

void validateValues(ConfigSchema const& s, ValidationErrors& errs) {
    auto requireId = [&](QString const& value, QString const& section,
                         QString const& field) {
        if (value.trimmed().isEmpty()) {
            errs.push_back({section, field,
                            QStringLiteral("must not be empty"), -1});
        }
    };
    auto requirePositive = [&](int value, QString const& section,
                               QString const& field) {
        if (value <= 0) {
            errs.push_back({section, field,
                            QStringLiteral("must be > 0"), -1});
        }
    };
    auto requireNonNegative = [&](int value, QString const& section,
                                  QString const& field) {
        if (value < 0) {
            errs.push_back({section, field,
                            QStringLiteral("must be >= 0"), -1});
        }
    };
    auto validateScheduler = [&](sched::SchedulerConfig const& cfg,
                                 QString const& section) {
        requireNonNegative(cfg.interRequestGapMs, section, "inter_request_gap_ms");
        requirePositive(cfg.maxQueueDepth, section, "max_queue_depth");
        requirePositive(cfg.maxInflight, section, "max_inflight");
        requireNonNegative(cfg.starvationGuardMs, section, "starvation_guard_ms");
        requireNonNegative(cfg.circuitBreakerThreshold, section,
                           "circuit_breaker_threshold");
        requirePositive(cfg.circuitBreakerOpenMs, section,
                        "circuit_breaker_open_ms");
    };

    if (!s.meta.logLevel.isEmpty()) {
        QString const level = s.meta.logLevel.trimmed().toLower();
        if (level != QStringLiteral("trace")
         && level != QStringLiteral("debug")
         && level != QStringLiteral("info")
         && level != QStringLiteral("warn")
         && level != QStringLiteral("warning")
         && level != QStringLiteral("error")
         && level != QStringLiteral("crit")
         && level != QStringLiteral("critical")) {
            errs.push_back({QStringLiteral("meta"), QStringLiteral("log_level"),
                QStringLiteral("unknown log level '%1'").arg(s.meta.logLevel), -1});
        }
    }

    for (int i = 0; i < s.codecs.size(); ++i) {
        auto const& c = s.codecs[i];
        auto const sec = QStringLiteral("codec[%1]").arg(i);
        requireId(c.id, sec, "id");
        if (isBuiltinCodecId(c.id)) {
            errs.push_back({sec, "id",
                QStringLiteral("'%1' is reserved by a built-in codec").arg(c.id), -1});
        }
        if (c.kind == QStringLiteral("enum_u16")) {
            if (c.map.isEmpty()) {
                errs.push_back({sec, "map",
                                QStringLiteral("enum_u16 requires a non-empty map"), -1});
            }
            for (auto it = c.map.constBegin(); it != c.map.constEnd(); ++it) {
                bool ok = false;
                uint const raw = it.key().toUInt(&ok);
                if (!ok || raw > 65535) {
                    errs.push_back({sec, "map",
                        QStringLiteral("key '%1' must be an integer in [0, 65535]")
                            .arg(it.key()), -1});
                }
                if (it.value().metaType().id() != QMetaType::QString) {
                    errs.push_back({sec, "map",
                        QStringLiteral("value for key '%1' must be a string")
                            .arg(it.key()), -1});
                }
            }
        } else if (c.kind == QStringLiteral("lua")) {
            requireId(c.script, sec, "script");
        } else {
            errs.push_back({sec, "kind",
                QStringLiteral("unknown codec kind '%1'").arg(c.kind), -1});
        }
    }

    for (int i = 0; i < s.transports.size(); ++i) {
        auto const& t = s.transports[i];
        auto const sec = QStringLiteral("transport[%1]").arg(i);
        requireId(t.id, sec, "id");
        requirePositive(t.connectTimeoutMs, sec, "connect_timeout_ms");
        requirePositive(t.requestTimeoutMs, sec, "request_timeout_ms");
        requireNonNegative(t.reconnectIntervalMs, sec, "reconnect_interval_ms");
        validateScheduler(t.scheduler, sec + ".scheduler");

        switch (t.kind) {
            case transport::TransportKind::ModbusTcpClient:
                requireId(t.host, sec, "host");
                if (t.port < 1 || t.port > 65535) {
                    errs.push_back({sec, "port",
                        QStringLiteral("must be in [1, 65535]"), -1});
                }
                break;
            case transport::TransportKind::ModbusTcpServer:
                requireId(t.listenAddress, sec, "listen_address");
                if (t.listenPort < 1 || t.listenPort > 65535) {
                    errs.push_back({sec, "listen_port",
                        QStringLiteral("must be in [1, 65535]"), -1});
                }
                if (t.listenRanges.isEmpty()) {
                    errs.push_back({sec, "listen_ranges",
                        QStringLiteral("requires at least one register range"), -1});
                }
                break;
            case transport::TransportKind::ModbusRtu:
                requireId(t.portName, sec, "port_name");
                requirePositive(t.baudRate, sec, "baud_rate");
                if (t.dataBits < 5 || t.dataBits > 8) {
                    errs.push_back({sec, "data_bits",
                        QStringLiteral("must be in [5, 8]"), -1});
                }
                if (t.stopBits != 1 && t.stopBits != 2) {
                    errs.push_back({sec, "stop_bits",
                        QStringLiteral("must be 1 or 2"), -1});
                }
                if (t.parity != QStringLiteral("none")
                 && t.parity != QStringLiteral("even")
                 && t.parity != QStringLiteral("odd")) {
                    errs.push_back({sec, "parity",
                        QStringLiteral("must be none, even, or odd"), -1});
                }
                break;
            case transport::TransportKind::OpcUaClient:
#ifndef CORE_HAS_OPCUA
                errs.push_back({sec, "kind",
                    QStringLiteral("OPC UA support is disabled in this build"), -1});
#endif
                requireId(t.endpointUrl, sec, "endpoint_url");
                {
                    QUrl const url(t.endpointUrl);
                    if (!url.isValid() || url.scheme() != QStringLiteral("opc.tcp")
                        || url.host().isEmpty()) {
                        errs.push_back({sec, "endpoint_url",
                            QStringLiteral("must be a valid opc.tcp:// URL with a host"), -1});
                    }
                }
                if (t.securityPolicy != QStringLiteral("None")) {
                    errs.push_back({sec, "security_policy",
                        QStringLiteral("only None is currently implemented"), -1});
                }
                if (!t.nodeIdTemplate.contains(QStringLiteral("%1"))) {
                    errs.push_back({sec, "node_id_template",
                        QStringLiteral("must contain %1 for the register address"), -1});
                }
                break;
            case transport::TransportKind::MqttClient:
#ifndef CORE_HAS_MQTT_QT
                errs.push_back({sec, "kind",
                    QStringLiteral("Qt MQTT support is disabled in this build"), -1});
#endif
                requireId(t.brokerUri, sec, "broker_uri");
                {
                    QUrl const url(t.brokerUri);
                    if (!url.isValid() || url.host().isEmpty()
                        || (url.scheme() != QStringLiteral("tcp")
                            && url.scheme() != QStringLiteral("mqtt"))) {
                        errs.push_back({sec, "broker_uri",
                            QStringLiteral("Qt MQTT requires tcp:// or mqtt:// with a host"), -1});
                    }
                }
                if (!t.topicTemplate.contains(QStringLiteral("%1"))) {
                    errs.push_back({sec, "topic_template",
                        QStringLiteral("must contain %1 for the register address"), -1});
                }
                if (t.qos < 0 || t.qos > 2) {
                    errs.push_back({sec, "qos",
                        QStringLiteral("must be in [0, 2]"), -1});
                }
                break;
            case transport::TransportKind::MqttPahoClient:
#ifndef CORE_HAS_MQTT_PAHO
                errs.push_back({sec, "kind",
                    QStringLiteral("Paho MQTT support is disabled in this build"), -1});
#endif
                requireId(t.brokerUri, sec, "broker_uri");
                {
                    QUrl const url(t.brokerUri);
                    auto const scheme = url.scheme();
                    if (!url.isValid() || url.host().isEmpty()
                        || (scheme != QStringLiteral("tcp")
                            && scheme != QStringLiteral("ssl")
                            && scheme != QStringLiteral("ws")
                            && scheme != QStringLiteral("wss"))) {
                        errs.push_back({sec, "broker_uri",
                            QStringLiteral("Paho MQTT requires tcp/ssl/ws/wss URL with a host"), -1});
                    }
                }
                if (!t.topicTemplate.contains(QStringLiteral("%1"))) {
                    errs.push_back({sec, "topic_template",
                        QStringLiteral("must contain %1 for the register address"), -1});
                }
                if (t.qos < 0 || t.qos > 2) {
                    errs.push_back({sec, "qos",
                        QStringLiteral("must be in [0, 2]"), -1});
                }
                break;
            case transport::TransportKind::S7Client:
                requireId(t.host, sec, "host");
                if (t.port < 1 || t.port > 65535) {
                    errs.push_back({sec, "port",
                        QStringLiteral("must be in [1, 65535]"), -1});
                }
                errs.push_back({sec, "kind",
                    QStringLiteral("s7_client is not implemented in this build"), -1});
                break;
        }

        if (t.kind == transport::TransportKind::ModbusTcpClient
         || t.kind == transport::TransportKind::ModbusTcpServer
         || t.kind == transport::TransportKind::ModbusRtu) {
            if (t.slaveId < 0 || t.slaveId > 247) {
                errs.push_back({sec, "slave_id",
                    QStringLiteral("must be in [0, 247]"), -1});
            }
        }
        for (int j = 0; j < t.listenRanges.size(); ++j) {
            auto const& r = t.listenRanges[j];
            validateRange(sec + QStringLiteral(".listen_ranges[%1]").arg(j),
                          "range", r.startAddress, r.size, 65536, errs);
        }
    }

    for (int i = 0; i < s.pollRanges.size(); ++i) {
        auto const& p = s.pollRanges[i];
        auto const sec = QStringLiteral("poll_range[%1]").arg(i);
        requireId(p.moduleId, sec, "module_id");
        if (!isKnownTable(p.table)) {
            errs.push_back({sec, "table",
                QStringLiteral("unknown register table '%1'").arg(p.table), -1});
        } else {
            // Protocol-specific PDU limits are checked after the transport
            // reference has been resolved. OPC UA/MQTT and a Modbus server's
            // local map are not constrained by Modbus client read limits.
            validateRange(sec, "range", p.startAddress, p.count, 65536, errs);
        }
    }

    for (int i = 0; i < s.sinkWindows.size(); ++i) {
        auto const& w = s.sinkWindows[i];
        auto const sec = QStringLiteral("sink_window[%1]").arg(i);
        requireId(w.moduleId, sec, "module_id");
        if (!isWritableTable(w.table)) {
            errs.push_back({sec, "table",
                QStringLiteral("table '%1' is not writable").arg(w.table), -1});
        } else {
            validateRange(sec, "range", w.startAddress, w.size, 65536, errs);
        }
        requireNonNegative(w.flush.debounceMs, sec + ".flush", "debounce_ms");
        requireNonNegative(w.flush.keepaliveMs, sec + ".flush", "keepalive_ms");
        if (w.initial.size() > w.size) {
            errs.push_back({sec, "initial",
                QStringLiteral("contains more values than the sink window"), -1});
        }
    }

    for (int i = 0; i < s.heartbeats.size(); ++i) {
        auto const& h = s.heartbeats[i];
        auto const sec = QStringLiteral("heartbeat[%1]").arg(i);
        requireId(h.moduleId, sec, "module_id");
        if (!isWritableTable(h.table)) {
            errs.push_back({sec, "table",
                QStringLiteral("table '%1' is not writable").arg(h.table), -1});
        } else if (!h.values.isEmpty()) {
            validateRange(sec, "values", h.address, h.values.size(), 65536, errs);
        }
        if (h.incrementer != QStringLiteral("none")) {
            errs.push_back({sec, "incrementer",
                QStringLiteral("incrementing heartbeat payloads are not implemented; use none"),
                -1});
        }
    }

    for (int i = 0; i < s.ackWatches.size(); ++i) {
        auto const& a = s.ackWatches[i];
        auto const sec = QStringLiteral("ack_watch[%1]").arg(i);
        requireId(a.moduleId, sec, "module_id");
        requirePositive(a.timeoutMs, sec, "timeout_ms");
        if (!a.expected.isValid()) {
            errs.push_back({sec, "expected",
                            QStringLiteral("is required"), -1});
        }
    }

    for (int i = 0; i < s.commands.size(); ++i) {
        auto const& c = s.commands[i];
        auto const sec = QStringLiteral("command[%1]").arg(i);
        requireId(c.moduleId, sec, "module_id");
        for (int j = 0; j < c.writes.size(); ++j) {
            auto const& w = c.writes[j];
            auto const writeSec = sec + QStringLiteral(".writes[%1]").arg(j);
            if (!isWritableTable(w.table)) {
                errs.push_back({writeSec, "table",
                    QStringLiteral("table '%1' is not writable").arg(w.table), -1});
            }
            validateRange(writeSec, "address", w.address, 1, 1, errs);
        }
    }

    for (int i = 0; i < s.datapoints.size(); ++i) {
        auto const& d = s.datapoints[i];
        auto const sec = QStringLiteral("datapoint[%1]").arg(i);
        requireId(d.id, sec, "id");
        if (d.kind != QStringLiteral("Status")
         && d.kind != QStringLiteral("Command")
         && d.kind != QStringLiteral("Bidirectional")) {
            errs.push_back({sec, "kind",
                QStringLiteral("unknown datapoint kind '%1'").arg(d.kind), -1});
        }
        if (d.type == dp::ScalarType::String) {
            errs.push_back({sec, "type",
                QStringLiteral("String datapoints require an explicit register length, which is not supported yet"),
                -1});
        }
        if (!d.policy.isEmpty()
         && d.policy != QStringLiteral("ContinuousMirror")) {
            errs.push_back({sec, "policy",
                QStringLiteral("policy '%1' is not implemented; use ContinuousMirror")
                    .arg(d.policy), -1});
        }
        auto validatePortRef = [&](PortRefConfig const& p,
                                   QString const& portSec, bool source) {
            if (source && p.port.trimmed().isEmpty()) {
                errs.push_back({portSec, "port",
                                QStringLiteral("must not be empty"), -1});
            }
            if (!p.table.isEmpty() && !isKnownTable(p.table)) {
                errs.push_back({portSec, "table",
                    QStringLiteral("unknown register table '%1'").arg(p.table), -1});
            }
            if (source && p.table.isEmpty()) {
                errs.push_back({portSec, "table",
                                QStringLiteral("must not be empty"), -1});
            }
            int const count = std::max(1, dp::registerCountFor(d.type));
            validateRange(portSec, "addr", p.address, count, 65536, errs);
            if (p.bit < -1 || p.bit > 15) {
                errs.push_back({portSec, "bit",
                                QStringLiteral("must be in [0, 15] when set"), -1});
            }
            if (p.bit >= 0 && d.type != dp::ScalarType::Bool) {
                errs.push_back({portSec, "bit",
                    QStringLiteral("bit is only supported for Bool datapoints"), -1});
            }
            if (p.shift < 0 || p.shift > 63) {
                errs.push_back({portSec, "shift",
                    QStringLiteral("must be in [0, 63]"), -1});
            }
            if (d.type != dp::ScalarType::Bool && p.mask == 0) {
                errs.push_back({portSec, "mask",
                    QStringLiteral("must not be zero"), -1});
            }
            if (!p.wordOrder.isEmpty()
             && p.wordOrder != QStringLiteral("ABCD")
             && p.wordOrder != QStringLiteral("CDAB")
             && p.wordOrder != QStringLiteral("BADC")
             && p.wordOrder != QStringLiteral("DCBA")) {
                errs.push_back({portSec, "wordOrder",
                    QStringLiteral("must be ABCD, CDAB, BADC, or DCBA"), -1});
            }
            if (!std::isfinite(p.scale) || !std::isfinite(p.offset)
                || p.scale == 0.0) {
                errs.push_back({portSec, "scale",
                    QStringLiteral("scale must be finite and non-zero; offset must be finite"), -1});
            }
            if (d.type == dp::ScalarType::Bool) {
                if (p.shift != 0 || p.mask != ~quint64(0)
                    || p.scale != 1.0 || p.offset != 0.0
                    || !p.wordOrder.isEmpty()) {
                    errs.push_back({portSec, "transform",
                        QStringLiteral("Bool uses only bit; mask/shift/wordOrder/scale/offset are not applicable"),
                        -1});
                }
            }
            if (d.type == dp::ScalarType::F32 || d.type == dp::ScalarType::F64) {
                if (p.shift != 0 || p.mask != ~quint64(0)) {
                    errs.push_back({portSec, "transform",
                        QStringLiteral("floating-point codecs do not apply mask/shift"), -1});
                }
            }
            if (!source && dp::isMultiRegister(d.type)
                && (p.shift != 0 || p.mask != ~quint64(0))) {
                errs.push_back({portSec, "transform",
                    QStringLiteral("multi-register sinks require full-word writes; mask/shift are not supported"),
                    -1});
            }
        };
        if (d.hasSource) validatePortRef(d.source, sec + ".source", true);
        if (d.hasSink)   validatePortRef(d.sink,   sec + ".sink", false);
        if (d.hasAck) {
            errs.push_back({sec + ".ack", "ack",
                QStringLiteral("datapoint ack policies are not implemented; use a standalone [[ack_watch]]"),
                -1});
            requirePositive(d.ack.timeoutMs, sec + ".ack", "timeout_ms");
            if (d.ack.dp.trimmed().isEmpty()) {
                errs.push_back({sec + ".ack", "dp",
                                QStringLiteral("must not be empty"), -1});
            }
            if (!d.ack.expected.isValid()) {
                errs.push_back({sec + ".ack", "expected",
                                QStringLiteral("is required"), -1});
            }
        }
    }

    for (int i = 0; i < s.bridges.size(); ++i) {
        auto const& b = s.bridges[i];
        auto const sec = QStringLiteral("bridge[%1]").arg(i);
        if (b.mirrorCount > 0) {
            if (b.mirrorPolicy == BridgeMirrorPolicy::Periodic) {
                requirePositive(b.mirrorPeriodMs, sec, "mirror_period_ms");
            } else if (b.mirrorPeriodMs != 0) {
                errs.push_back({sec, "mirror_period_ms",
                    QStringLiteral("is only valid when mirror_policy is 'Periodic'"), -1});
            }
        }
        if (b.writeCount > 0) {
            validateRange(sec, "write", b.writeStart, b.writeCount, 123, errs);
            qint64 const plcStart = qint64(b.writeStart) - qint64(b.offset);
            if (plcStart < 0 || plcStart + b.writeCount > 65536) {
                errs.push_back({sec, "offset",
                    QStringLiteral("write range maps outside PLC address space"), -1});
            }
        }
        if (b.mirrorCount > 0) {
            validateRange(sec, "mirror", b.mirrorStart, b.mirrorCount, 65536, errs);
            qint64 const serverStart = qint64(b.mirrorStart) + qint64(b.offset);
            if (serverStart < 0 || serverStart + b.mirrorCount > 65536) {
                errs.push_back({sec, "offset",
                    QStringLiteral("mirror range maps outside server address space"), -1});
            }
        }
    }

    for (int i = 0; i < s.plugins.size(); ++i) {
        auto const& p = s.plugins[i];
        requireId(p.dllPath, QStringLiteral("plugin[%1]").arg(i), "dll");
    }
}

void validateRefs(ConfigSchema const& s, ValidationErrors& errs) {
    std::set<QString> transports;
    std::map<QString, TransportConfig const*> transportById;
    for (auto const& t : s.transports) {
        transports.insert(t.id);
        transportById.emplace(t.id, &t);
    }

    std::set<QString> sinkWindowIds;
    std::map<QString, SinkWindowConfig const*> sinkWindowById;
    for (auto const& sw : s.sinkWindows) {
        sinkWindowIds.insert(sw.moduleId);
        sinkWindowById.emplace(sw.moduleId, &sw);
    }

    std::set<QString> codecIds;
    for (auto const& c : s.codecs) codecIds.insert(c.id);

    std::set<QString> datapointIds;
    std::map<QString, DatapointConfig const*> datapointById;
    for (auto const& d : s.datapoints) {
        datapointIds.insert(d.id);
        datapointById.emplace(d.id, &d);
    }

    auto checkTransportRef = [&](QString const& tid,
                                  QString const& section,
                                  QString const& field) {
        if (!tid.isEmpty() && !transports.count(tid)) {
            errs.push_back({section, field,
                            QStringLiteral("references unknown transport '%1'").arg(tid),
                            -1});
        }
    };

    auto checkPortRef = [&](PortRefConfig const& p, QString const& section) {
        if (!p.port.isEmpty() && !transports.count(p.port)) {
            errs.push_back({section, "port",
                            QStringLiteral("references unknown transport '%1'").arg(p.port),
                            -1});
        }
    };

    for (int i = 0; i < s.pollRanges.size(); ++i) {
        auto const& poll = s.pollRanges[i];
        auto const sec = QStringLiteral("poll_range[%1]").arg(i);
        checkTransportRef(poll.transport, sec, "transport");
        auto const transportIt = transportById.find(poll.transport);
        if (transportIt != transportById.end()
            && usesModbusClientRequestLimits(transportIt->second->kind)
            && poll.count > maxReadCount(poll.table)) {
            errs.push_back({sec, "range",
                QStringLiteral("count %1 exceeds Modbus read maximum (%2) for table '%3'")
                    .arg(poll.count).arg(maxReadCount(poll.table)).arg(poll.table), -1});
        }
    }
    for (int i = 0; i < s.sinkWindows.size(); ++i) {
        auto const& sw = s.sinkWindows[i];
        auto const sec = QStringLiteral("sink_window[%1]").arg(i);
        checkTransportRef(sw.transport, sec, "transport");
        auto const transportIt = transportById.find(sw.transport);
        if (transportIt != transportById.end()
            && usesModbusClientRequestLimits(transportIt->second->kind)
            && sw.size > maxWriteCount(sw.table)) {
            errs.push_back({sec, "range",
                QStringLiteral("size %1 exceeds Modbus write maximum (%2) for table '%3'")
                    .arg(sw.size).arg(maxWriteCount(sw.table)).arg(sw.table), -1});
        }
    }
    for (int i = 0; i < s.heartbeats.size(); ++i) {
        auto const& heartbeat = s.heartbeats[i];
        auto const sec = QStringLiteral("heartbeat[%1]").arg(i);
        checkTransportRef(heartbeat.transport, sec, "transport");
        auto const transportIt = transportById.find(heartbeat.transport);
        if (transportIt != transportById.end()
            && usesModbusClientRequestLimits(transportIt->second->kind)
            && heartbeat.values.size() > maxWriteCount(heartbeat.table)) {
            errs.push_back({sec, "values",
                QStringLiteral("value count %1 exceeds Modbus write maximum (%2) for table '%3'")
                    .arg(heartbeat.values.size())
                    .arg(maxWriteCount(heartbeat.table)).arg(heartbeat.table), -1});
        }
    }
    for (int i = 0; i < s.commands.size(); ++i) {
        checkTransportRef(s.commands[i].transport,
                          QStringLiteral("command[%1]").arg(i), "transport");
    }

    // Independent writers must not own the same register. In particular, a
    // SinkWindow rewrites its complete snapshot on keepalive/reconnect and
    // would silently undo a heartbeat, command, or another window's update.
    for (int i = 0; i < s.sinkWindows.size(); ++i) {
        auto const& a = s.sinkWindows[i];
        for (int j = i + 1; j < s.sinkWindows.size(); ++j) {
            auto const& b = s.sinkWindows[j];
            if (a.transport == b.transport
                && canonicalTable(a.table) == canonicalTable(b.table)
                && rangesOverlap(a.startAddress, a.size,
                                 b.startAddress, b.size)) {
                errs.push_back({QStringLiteral("sink_window[%1]").arg(j), "range",
                    QStringLiteral("overlaps sink_window[%1] on transport '%2'")
                        .arg(i).arg(a.transport), -1});
            }
        }
    }
    for (int i = 0; i < s.heartbeats.size(); ++i) {
        auto const& heartbeat = s.heartbeats[i];
        auto const sec = QStringLiteral("heartbeat[%1]").arg(i);
        for (int j = 0; j < s.sinkWindows.size(); ++j) {
            auto const& sink = s.sinkWindows[j];
            if (heartbeat.transport == sink.transport
                && canonicalTable(heartbeat.table) == canonicalTable(sink.table)
                && rangesOverlap(heartbeat.address, heartbeat.values.size(),
                                 sink.startAddress, sink.size)) {
                errs.push_back({sec, "address",
                    QStringLiteral("overlaps sink_window[%1] on transport '%2'")
                        .arg(j).arg(sink.transport), -1});
            }
        }
        for (int j = 0; j < i; ++j) {
            auto const& other = s.heartbeats[j];
            if (heartbeat.transport == other.transport
                && canonicalTable(heartbeat.table) == canonicalTable(other.table)
                && rangesOverlap(heartbeat.address, heartbeat.values.size(),
                                 other.address, other.values.size())) {
                errs.push_back({sec, "address",
                    QStringLiteral("overlaps heartbeat[%1] on transport '%2'")
                        .arg(j).arg(heartbeat.transport), -1});
            }
        }
    }
    for (int i = 0; i < s.commands.size(); ++i) {
        auto const& command = s.commands[i];
        for (int w = 0; w < command.writes.size(); ++w) {
            auto const& write = command.writes[w];
            auto const sec = QStringLiteral("command[%1].writes[%2]").arg(i).arg(w);
            for (int j = 0; j < s.sinkWindows.size(); ++j) {
                auto const& sink = s.sinkWindows[j];
                if (command.transport == sink.transport
                    && canonicalTable(write.table) == canonicalTable(sink.table)
                    && rangesOverlap(write.address, 1,
                                     sink.startAddress, sink.size)) {
                    errs.push_back({sec, "address",
                        QStringLiteral("overlaps sink_window[%1] on transport '%2'")
                            .arg(j).arg(sink.transport), -1});
                }
            }
            for (int j = 0; j < s.heartbeats.size(); ++j) {
                auto const& heartbeat = s.heartbeats[j];
                if (command.transport == heartbeat.transport
                    && canonicalTable(write.table) == canonicalTable(heartbeat.table)
                    && rangesOverlap(write.address, 1,
                                     heartbeat.address, heartbeat.values.size())) {
                    errs.push_back({sec, "address",
                        QStringLiteral("overlaps heartbeat[%1] on transport '%2'")
                            .arg(j).arg(heartbeat.transport), -1});
                }
            }
        }
    }

    // Codec ref + kind / sink / source consistency.
    auto checkCodecRef = [&](QString const& id, QString const& section) {
        if (id.isEmpty() || codecIds.count(id) || isBuiltinCodecId(id)) return;
        errs.push_back({section, "codec",
            QStringLiteral("references unknown codec '%1'").arg(id), -1});
    };

    for (int i = 0; i < s.datapoints.size(); ++i) {
        auto const& d  = s.datapoints[i];
        auto const sec = QStringLiteral("datapoint[%1]").arg(i);
        if (d.hasSource) {
            checkPortRef(d.source, sec + ".source");
            checkCodecRef(d.source.codec, sec + ".source");

            int const words = std::max(1, dp::registerCountFor(d.type));
            bool polled = false;
            for (auto const& poll : s.pollRanges) {
                if (poll.transport != d.source.port
                    || canonicalTable(poll.table)
                        != canonicalTable(d.source.table)) {
                    continue;
                }
                if (d.source.address >= poll.startAddress
                    && qint64(d.source.address) + words
                        <= qint64(poll.startAddress) + poll.count) {
                    polled = true;
                    break;
                }
            }
            bool serverRouteDriven = false;
            if (auto transportIt = transportById.find(d.source.port);
                transportIt != transportById.end()
                && transportIt->second->kind
                    == transport::TransportKind::ModbusTcpServer) {
                serverRouteDriven = std::any_of(
                    s.routes.cbegin(), s.routes.cend(),
                    [&](RouteConfig const& route) { return route.from == d.id; });
            }
            if (!polled && !serverRouteDriven) {
                errs.push_back({sec + ".source", "range",
                    QStringLiteral("is not fully covered by a poll_range and is not a Modbus-server route source"),
                    -1});
            }
        }
        if (d.hasSink) {
            if (!d.sink.window.isEmpty() && !sinkWindowIds.count(d.sink.window)) {
                errs.push_back({sec + ".sink", "window",
                                QStringLiteral("references unknown sink_window '%1'")
                                    .arg(d.sink.window),
                                -1});
            }
            checkPortRef(d.sink, sec + ".sink");
            checkCodecRef(d.sink.codec, sec + ".sink");

            // sink.addr must fall within the referenced sink_window.
            if (!d.sink.window.isEmpty()) {
                auto it = sinkWindowById.find(d.sink.window);
                if (it != sinkWindowById.end()) {
                    auto const* sw = it->second;
                    int const lo = sw->startAddress;
                    int const hi = sw->startAddress + sw->size;
                    int const words = std::max(1, dp::registerCountFor(d.type));
                    if (d.sink.address < lo || d.sink.address + words > hi) {
                        errs.push_back({sec + ".sink", "addr",
                            QStringLiteral("range at %1 outside sink_window '%2' [%3,%4)")
                                .arg(d.sink.address).arg(d.sink.window)
                                .arg(lo).arg(hi), -1});
                    }
                    if (!d.sink.port.isEmpty()
                        && d.sink.port != sw->transport) {
                        errs.push_back({sec + ".sink", "port",
                            QStringLiteral("must match sink_window transport '%1'")
                                .arg(sw->transport), -1});
                    }
                    if (!d.sink.table.isEmpty()
                        && canonicalTable(d.sink.table)
                            != canonicalTable(sw->table)) {
                        errs.push_back({sec + ".sink", "table",
                            QStringLiteral("must match sink_window table '%1'")
                                .arg(sw->table), -1});
                    }
                }
            } else {
                int const words = std::max(1, dp::registerCountFor(d.type));
                int matches = 0;
                for (auto const& sw : s.sinkWindows) {
                    if (d.sink.port != sw.transport
                        || canonicalTable(d.sink.table)
                            != canonicalTable(sw.table)) {
                        continue;
                    }
                    if (d.sink.address >= sw.startAddress
                        && qint64(d.sink.address) + words
                            <= qint64(sw.startAddress) + sw.size) {
                        ++matches;
                    }
                }
                if (matches == 0) {
                    errs.push_back({sec + ".sink", "window",
                        QStringLiteral("must name a sink_window or be fully covered by one on the same transport/table"),
                        -1});
                }
            }
        }
        if (dp::isMultiRegister(d.type) && d.hasSource && d.source.wordOrder.isEmpty()) {
            errs.push_back({sec + ".source", "wordOrder",
                            QStringLiteral("type=%1 requires wordOrder "
                                            "(ABCD/CDAB/BADC/DCBA)")
                                .arg(QString::fromUtf8(dp::scalarTypeName(d.type))),
                            -1});
        }
        if (d.type == dp::ScalarType::Bool && d.hasSource && d.source.bit < 0) {
            errs.push_back({sec + ".source", "bit",
                            QStringLiteral("type=Bool requires bit (0..15)"),
                            -1});
        }

        // EnumU16 requires an explicit codec (no builtin enum codec exists).
        if (d.type == dp::ScalarType::EnumU16 && d.hasSource
         && d.source.codec.isEmpty()) {
            errs.push_back({sec + ".source", "codec",
                QStringLiteral("type=EnumU16 requires an explicit codec"), -1});
        }

        // Kind ↔ source/sink consistency.
        if (d.kind == "Status" && !d.hasSource) {
            errs.push_back({sec, "kind",
                QStringLiteral("kind=Status requires source"), -1});
        }
        if (d.kind == "Command" && !d.hasSink) {
            errs.push_back({sec, "kind",
                QStringLiteral("kind=Command requires sink"), -1});
        }
        if (d.kind == "Bidirectional" && (!d.hasSource || !d.hasSink)) {
            errs.push_back({sec, "kind",
                QStringLiteral("kind=Bidirectional requires both source and sink"),
                -1});
        }

        // Mask must fit in the type's bit width.
        auto checkMask = [&](PortRefConfig const& p, QString const& port_sec) {
            int const width = typeBitWidth(d.type);
            quint64 const limit = (width >= 64)
                ? ~quint64(0)
                : ((quint64(1) << width) - 1);
            if (p.mask != ~quint64(0) && (p.mask & ~limit) != 0) {
                errs.push_back({port_sec, "mask",
                    QStringLiteral("mask 0x%1 exceeds type=%2 bit width (%3)")
                        .arg(p.mask, 0, 16)
                        .arg(QString::fromUtf8(dp::scalarTypeName(d.type)))
                        .arg(width), -1});
            }
            if (d.type != dp::ScalarType::Bool
                && d.type != dp::ScalarType::F32
                && d.type != dp::ScalarType::F64) {
                quint64 const effectiveMask = p.mask == ~quint64(0) ? limit : p.mask;
                if (p.shift < 0 || p.shift >= width
                    || effectiveMask > (limit >> p.shift)) {
                    errs.push_back({port_sec, "shift",
                        QStringLiteral("mask shifted by %1 exceeds type=%2 bit width (%3)")
                            .arg(p.shift)
                            .arg(QString::fromUtf8(dp::scalarTypeName(d.type)))
                            .arg(width), -1});
                }
            }
        };
        if (d.hasSource) checkMask(d.source, sec + ".source");
        if (d.hasSink)   checkMask(d.sink,   sec + ".sink");
    }

    for (int i = 0; i < s.ackWatches.size(); ++i) {
        auto const& a   = s.ackWatches[i];
        auto const sec  = QStringLiteral("ack_watch[%1]").arg(i);
        if (!a.dp.isEmpty() && !datapointIds.count(a.dp)) {
            errs.push_back({sec, "dp",
                QStringLiteral("references unknown datapoint '%1'").arg(a.dp), -1});
        }
    }

    for (int i = 0; i < s.routes.size(); ++i) {
        auto const& r   = s.routes[i];
        auto const sec  = QStringLiteral("route[%1]").arg(i);
        if (!r.from.isEmpty() && !datapointIds.count(r.from)) {
            errs.push_back({sec, "from",
                QStringLiteral("references unknown datapoint '%1'").arg(r.from), -1});
        }
        if (!r.to.isEmpty() && !datapointIds.count(r.to)) {
            errs.push_back({sec, "to",
                QStringLiteral("references unknown datapoint '%1'").arg(r.to), -1});
        }
        if (auto it = datapointById.find(r.from);
            it != datapointById.end() && !it->second->hasSource) {
            errs.push_back({sec, "from",
                QStringLiteral("source datapoint '%1' has no source port").arg(r.from),
                -1});
        }
        if (auto it = datapointById.find(r.from);
            it != datapointById.end() && it->second->hasSource) {
            auto transportIt = transportById.find(it->second->source.port);
            if (transportIt != transportById.end()
                && transportIt->second->kind
                    != transport::TransportKind::ModbusTcpServer) {
                errs.push_back({sec, "from",
                    QStringLiteral("source datapoint must use a modbus_tcp_server transport"),
                    -1});
            } else if (transportIt != transportById.end()) {
                auto const& source = it->second->source;
                int const words = std::max(
                    1, dp::registerCountFor(it->second->type));
                bool covered = false;
                for (auto const& range : transportIt->second->listenRanges) {
                    if (canonicalTable(range.table)
                        != canonicalTable(source.table)) {
                        continue;
                    }
                    if (source.address >= range.startAddress
                        && qint64(source.address) + words
                            <= qint64(range.startAddress) + range.size) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    errs.push_back({sec, "from",
                        QStringLiteral("source datapoint range is outside the server listen ranges"),
                        -1});
                }
            }
        }
        if (auto it = datapointById.find(r.to);
            it != datapointById.end() && !it->second->hasSink) {
            errs.push_back({sec, "to",
                QStringLiteral("target datapoint '%1' has no sink port").arg(r.to),
                -1});
        }
        if (r.policy != QStringLiteral("ContinuousMirror")) {
            errs.push_back({sec, "policy",
                QStringLiteral("route policy '%1' is not implemented; use ContinuousMirror")
                    .arg(r.policy), -1});
        }
    }

    for (int i = 0; i < s.bridges.size(); ++i) {
        auto const& b   = s.bridges[i];
        auto const sec  = QStringLiteral("bridge[%1]").arg(i);
        checkTransportRef(b.server, sec, "server");
        checkTransportRef(b.plc,    sec, "plc");
        auto serverIt = transportById.find(b.server);
        auto plcIt = transportById.find(b.plc);
        if (serverIt != transportById.end()
            && serverIt->second->kind != transport::TransportKind::ModbusTcpServer) {
            errs.push_back({sec, "server",
                QStringLiteral("must reference a modbus_tcp_server transport"), -1});
        }
        if (plcIt != transportById.end()
            && !usesModbusClientRequestLimits(plcIt->second->kind)) {
            errs.push_back({sec, "plc",
                QStringLiteral("must reference a modbus_tcp_client or modbus_rtu transport"), -1});
        }
        if (b.writeCount < 0 || b.mirrorCount < 0) {
            errs.push_back({sec, "count",
                QStringLiteral("write_count / mirror_count must be >= 0"), -1});
        }
        if (serverIt != transportById.end()
            && serverIt->second->kind == transport::TransportKind::ModbusTcpServer) {
            auto covered = [&](int start, int count) {
                if (count <= 0) return true;
                for (auto const& range : serverIt->second->listenRanges) {
                    if (range.table != QModbusDataUnit::HoldingRegisters) continue;
                    if (start >= range.startAddress
                        && qint64(start) + count
                            <= qint64(range.startAddress) + range.size) {
                        return true;
                    }
                }
                return false;
            };
            if (!covered(b.writeStart, b.writeCount)) {
                errs.push_back({sec, "write",
                    QStringLiteral("write range is outside the server HR listen ranges"), -1});
            }
            if (!covered(b.mirrorStart + b.offset, b.mirrorCount)) {
                errs.push_back({sec, "mirror",
                    QStringLiteral("mapped mirror range is outside the server HR listen ranges"), -1});
            }
        }
        qint64 const mappedMirrorStart = qint64(b.mirrorStart) + b.offset;
        if (rangesOverlap(b.writeStart, b.writeCount,
                          mappedMirrorStart, b.mirrorCount)) {
            errs.push_back({sec, "range",
                QStringLiteral("server write range overlaps the mapped mirror range"),
                -1});
        }
        qint64 const mappedPlcWriteStart = qint64(b.writeStart) - b.offset;
        for (int j = 0; j < s.sinkWindows.size(); ++j) {
            auto const& sink = s.sinkWindows[j];
            if (sink.transport == b.plc
                && canonicalTable(sink.table) == QStringLiteral("HR")
                && rangesOverlap(mappedPlcWriteStart, b.writeCount,
                                 sink.startAddress, sink.size)) {
                errs.push_back({sec, "write",
                    QStringLiteral("mapped PLC write range overlaps sink_window[%1]")
                        .arg(j), -1});
            }
        }
        for (int j = 0; j < s.heartbeats.size(); ++j) {
            auto const& heartbeat = s.heartbeats[j];
            if (heartbeat.transport == b.plc
                && canonicalTable(heartbeat.table) == QStringLiteral("HR")
                && rangesOverlap(mappedPlcWriteStart, b.writeCount,
                                 heartbeat.address, heartbeat.values.size())) {
                errs.push_back({sec, "write",
                    QStringLiteral("mapped PLC write range overlaps heartbeat[%1]")
                        .arg(j), -1});
            }
        }
        for (int j = 0; j < s.commands.size(); ++j) {
            auto const& command = s.commands[j];
            if (command.transport != b.plc) continue;
            for (int w = 0; w < command.writes.size(); ++w) {
                auto const& write = command.writes[w];
                if (canonicalTable(write.table) == QStringLiteral("HR")
                    && rangesOverlap(mappedPlcWriteStart, b.writeCount,
                                     write.address, 1)) {
                    errs.push_back({sec, "write",
                        QStringLiteral("mapped PLC write range overlaps command[%1].writes[%2]")
                            .arg(j).arg(w), -1});
                }
            }
        }
        // Raw mirror snapshots come from one successful PollRange result. A
        // single range must cover the entire mirror window so every register
        // belongs to the same PLC read cycle; datapoint presence is irrelevant.
        if (b.mirrorCount > 0 && transports.count(b.plc)) {
            bool coveredByOnePoll = false;
            for (auto const& poll : s.pollRanges) {
                if (poll.transport != b.plc
                    || canonicalTable(poll.table) != QStringLiteral("HR")) continue;
                if (poll.startAddress <= b.mirrorStart
                    && qint64(poll.startAddress) + poll.count
                        >= qint64(b.mirrorStart) + b.mirrorCount) {
                    coveredByOnePoll = true;
                    break;
                }
            }
            if (!coveredByOnePoll) {
                errs.push_back({sec, "mirror",
                    QStringLiteral("mirror range [%1,%2) must be fully covered by one HR "
                                   "poll_range on plc '%3'")
                        .arg(b.mirrorStart).arg(b.mirrorStart + b.mirrorCount).arg(b.plc), -1});
            }
        }
    }

    // Two bridges sharing one server must not publish into overlapping server
    // address windows. Otherwise the last mirror wins, or a mirrored status
    // window is also interpreted as an operator command by another bridge.
    for (int i = 0; i < s.bridges.size(); ++i) {
        auto const& a = s.bridges[i];
        for (int j = i + 1; j < s.bridges.size(); ++j) {
            auto const& b = s.bridges[j];
            struct Window { qint64 start; qint64 count; };
            Window const aw{a.writeStart, a.writeCount};
            Window const am{qint64(a.mirrorStart) + a.offset, a.mirrorCount};
            Window const bw{b.writeStart, b.writeCount};
            Window const bm{qint64(b.mirrorStart) + b.offset, b.mirrorCount};
            auto const overlaps = [](Window x, Window y) {
                return rangesOverlap(x.start, x.count, y.start, y.count);
            };
            if (a.server == b.server
                && (overlaps(aw, bw) || overlaps(aw, bm)
                    || overlaps(am, bw) || overlaps(am, bm))) {
                errs.push_back({QStringLiteral("bridge[%1]").arg(j), "range",
                    QStringLiteral("server address window overlaps bridge[%1]")
                        .arg(i), -1});
            }
            qint64 const aPlcStart = qint64(a.writeStart) - a.offset;
            qint64 const bPlcStart = qint64(b.writeStart) - b.offset;
            if (a.plc == b.plc
                && rangesOverlap(aPlcStart, a.writeCount,
                                 bPlcStart, b.writeCount)) {
                errs.push_back({QStringLiteral("bridge[%1]").arg(j), "write",
                    QStringLiteral("mapped PLC write range overlaps bridge[%1]")
                        .arg(i), -1});
            }
        }
    }
}

} // namespace

// ───────────────────────────────────────────────────────────────────────

std::expected<ConfigSchema, ValidationErrors>
ConfigLoader::loadFromToml(QString const& path) {
    ConfigSchema schema;
    ValidationErrors errs;

    toml::table root;
    try {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            errs.push_back({"meta", "path",
                            QStringLiteral("cannot open '%1'").arg(path), -1});
            return std::unexpected(std::move(errs));
        }
        QByteArray bytes = f.readAll();
        std::string_view sv(bytes.constData(), size_t(bytes.size()));
        root = toml::parse(sv);
    } catch (toml::parse_error const& e) {
        errs.push_back({"meta", "toml",
                        QString::fromUtf8(e.description().data(),
                                          int(e.description().size())),
                        int(e.source().begin.line)});
        return std::unexpected(std::move(errs));
    }

    rejectUnknownKeys(root, QStringLiteral("root"),
                      {"meta", "transport", "codec", "poll_range",
                       "sink_window", "heartbeat", "ack_watch", "command",
                       "datapoint", "route", "bridge", "plugin"}, errs);

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
    if (vErrs.has_value()) {
        // success
    } else {
        for (auto const& e : vErrs.error()) errs.append(e);
    }

    if (!errs.isEmpty()) return std::unexpected(std::move(errs));
    return schema;
}

std::expected<void, ValidationErrors>
ConfigLoader::validate(ConfigSchema const& schema) {
    ValidationErrors errs;

    validateValues(schema, errs);

    QList<QString> tIds;
    for (auto const& t : schema.transports) tIds.append(t.id);
    checkUnique(tIds, "transport", "id", errs);

    // Module IDs are unique across all module-bearing sections.
    QList<QString> mIds;
    for (auto const& m : schema.pollRanges)  mIds.append(m.moduleId);
    for (auto const& m : schema.sinkWindows) mIds.append(m.moduleId);
    for (auto const& m : schema.heartbeats)  mIds.append(m.moduleId);
    for (auto const& m : schema.ackWatches)  mIds.append(m.moduleId);
    for (auto const& m : schema.commands)    mIds.append(m.moduleId);
    for (int i = 0; i < schema.bridges.size(); ++i) {
        if (schema.bridges[i].writeCount > 0) {
            mIds.append(QStringLiteral("bridge.fwd.%1.%2")
                            .arg(schema.bridges[i].server).arg(i));
        }
    }
    checkUnique(mIds, "module", "module_id", errs);

    QList<QString> dIds;
    for (auto const& d : schema.datapoints) dIds.append(d.id);
    checkUnique(dIds, "datapoint", "id", errs);

    QList<QString> cIds;
    for (auto const& c : schema.codecs) cIds.append(c.id);
    checkUnique(cIds, "codec", "id", errs);

    validateRefs(schema, errs);

    if (!errs.isEmpty()) return std::unexpected(std::move(errs));
    return {};
}

} // namespace core::config
