#include "core/config/ConfigLoader.h"

#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include <QFile>
#include <QTextStream>
#include <toml++/toml.hpp>

#include "core/dp/ScalarType.h"
#include "core/dp/WordOrder.h"
#include "core/sched/SchedulerTypes.h"

namespace core::config {

namespace {

QString tomlValueLine(toml::node const& n) {
    auto const& src = n.source();
    return src.begin.line > 0 ? QString::number(int(src.begin.line)) : QStringLiteral("?");
}

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

dp::WordOrder parseWordOrder(QString const& s, bool& ok) {
    ok = true;
    if (s.isEmpty() || s == "ABCD") return dp::WordOrder::ABCD;
    if (s == "CDAB") return dp::WordOrder::CDAB;
    if (s == "BADC") return dp::WordOrder::BADC;
    if (s == "DCBA") return dp::WordOrder::DCBA;
    ok = false;
    return dp::WordOrder::ABCD;
}

sched::Priority parsePriority(QString const& s) {
    if (s == "Low")      return sched::Priority::Low;
    if (s == "High")     return sched::Priority::High;
    if (s == "Critical") return sched::Priority::Critical;
    return sched::Priority::Normal;
}

sched::SchedulerKind parseSchedulerKind(QString const& s) {
    if (s == "credit")   return sched::SchedulerKind::Credit;
    if (s == "priority") return sched::SchedulerKind::Priority;
    return sched::SchedulerKind::Serial;
}

transport::TransportKind parseTransportKind(QString const& s, bool& ok) {
    ok = true;
    if (s == "modbus_tcp_client") return transport::TransportKind::ModbusTcpClient;
    if (s == "modbus_tcp_server") return transport::TransportKind::ModbusTcpServer;
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
    }
    return m;
}

TransportConfig parseTransport(toml::table const& t,
                                int                index,
                                ValidationErrors&   errs) {
    auto const section = QStringLiteral("transport[%1]").arg(index);
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

    c.host           = getStr(t, "host", {});
    c.port           = quint16(getInt(t, "port", 502));
    c.slaveId        = getInt(t, "slave_id", 1);
    c.listenAddress  = getStr(t, "listen_address", QStringLiteral("0.0.0.0"));
    c.listenPort     = getInt(t, "listen_port", 502);
    c.maxClients     = getInt(t, "max_clients", 1);
    c.reconnectIntervalMs = getInt(t, "reconnect_interval_ms", 15000);
    c.connectTimeoutMs    = getInt(t, "connect_timeout_ms",    3000);

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
                QString const tbl = getStr(*wt, "table", QStringLiteral("HR"));
                if (tbl == "HR" || tbl == "HoldingRegisters")
                    r.table = QModbusDataUnit::HoldingRegisters;
                else if (tbl == "IR" || tbl == "InputRegisters")
                    r.table = QModbusDataUnit::InputRegisters;
                else if (tbl == "Coil" || tbl == "Coils")
                    r.table = QModbusDataUnit::Coils;
                else if (tbl == "DI" || tbl == "DiscreteInputs")
                    r.table = QModbusDataUnit::DiscreteInputs;
                else
                    r.table = QModbusDataUnit::HoldingRegisters;
                if (auto arr = (*wt)["range"].as_array(); arr && arr->size() == 2) {
                    r.startAddress = int(arr->at(0).value<int64_t>().value_or(0));
                    r.size         = int(arr->at(1).value<int64_t>().value_or(0));
                }
                c.listenRanges.append(r);
            }
        }
    }
    return c;
}

CodecConfig parseCodec(toml::table const& t,
                        int                index,
                        ValidationErrors&   errs) {
    auto const section = QStringLiteral("codec[%1]").arg(index);
    CodecConfig c;
    requireStr(t, "id",   section, c.id,   errs);
    requireStr(t, "kind", section, c.kind, errs);
    c.script = getStr(t, "script", {});
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
    PollRangeConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        c.startAddress = int(arr->at(0).value<int64_t>().value_or(0));
        c.count        = int(arr->at(1).value<int64_t>().value_or(0));
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
    c.priority = parsePriority(getStr(t, "priority", "Normal"));
    return c;
}

QList<quint16> parseU16Array(toml::array const& arr) {
    QList<quint16> out;
    for (auto&& n : arr) {
        if (auto i = n.value<int64_t>()) out.append(quint16(*i));
    }
    return out;
}

SinkWindowConfig parseSinkWindow(toml::table const& t,
                                   int                index,
                                   ValidationErrors&   errs) {
    auto const section = QStringLiteral("sink_window[%1]").arg(index);
    SinkWindowConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    requireStr(t, "table",     section, c.table,     errs);
    if (auto arr = t["range"].as_array(); arr && arr->size() == 2) {
        c.startAddress = int(arr->at(0).value<int64_t>().value_or(0));
        c.size         = int(arr->at(1).value<int64_t>().value_or(0));
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
    auto const section = QStringLiteral("heartbeat[%1]").arg(index);
    HeartbeatConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    c.table       = getStr(t, "table", QStringLiteral("HR"));
    c.address     = getInt(t, "address", 0);
    if (auto arr = t["values"].as_array()) {
        c.values = parseU16Array(*arr);
    } else if (auto v = getInt(t, "value")) {
        c.values.append(quint16(*v));
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
    c.priority    = parsePriority(getStr(t, "priority", "Low"));
    c.incrementer = getStr(t, "incrementer", QStringLiteral("none"));
    return c;
}

AckWatchConfig parseAckWatch(toml::table const& t,
                               int                index,
                               ValidationErrors&   errs) {
    auto const section = QStringLiteral("ack_watch[%1]").arg(index);
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
    CommandConfig c;
    requireStr(t, "module_id", section, c.moduleId,  errs);
    requireStr(t, "transport", section, c.transport, errs);
    c.priority      = parsePriority(getStr(t, "priority", "High"));
    c.interruptable = getBool(t, "interruptable").value_or(false);
    c.trigger       = getStr(t, "trigger", {});
    if (auto arr = t["writes"].as_array()) {
        int wi = 0;
        for (auto&& n : *arr) {
            if (auto wt = n.as_table()) {
                CommandWriteEntry e;
                e.table   = getStr(*wt, "table", QStringLiteral("HR"));
                e.address = getInt(*wt, "address", 0);
                e.value   = quint16(getInt(*wt, "value", 0));
                c.writes.append(e);
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
    RouteConfig c;
    c.name   = getStr(t, "name", {});
    requireStr(t, "from", section, c.from, errs);
    requireStr(t, "to",   section, c.to,   errs);
    c.policy = getStr(t, "policy", QStringLiteral("ContinuousMirror"));
    return c;
}

PortRefConfig parsePortRef(toml::table const& t,
                             QString const&     section,
                             ValidationErrors&   errs) {
    PortRefConfig p;
    p.port    = getStr(t, "port",    {});
    p.table   = getStr(t, "table",   {});
    p.address = getInt(t, "addr",    0);
    p.bit     = getInt(t, "bit",     -1);
    p.wordOrder = getStr(t, "wordOrder", {});
    p.shift   = getInt(t, "shift",   0);
    if (auto m = getInt(t, "mask")) p.mask = quint64(*m);
    p.scale   = getDouble(t, "scale", 1.0);
    p.offset  = getDouble(t, "offset", 0.0);
    p.codec   = getStr(t, "codec", {});
    p.dedupe  = getStr(t, "dedupe", "none");
    p.window  = getStr(t, "window", {});

    Q_UNUSED(section); Q_UNUSED(errs);
    return p;
}

DatapointConfig parseDatapoint(toml::table const& t,
                                 int                index,
                                 ValidationErrors&   errs) {
    auto const section = QStringLiteral("datapoint[%1]").arg(index);
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
            }
            ++i;
        }
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

void validateRefs(ConfigSchema const& s, ValidationErrors& errs) {
    std::set<QString> transports;
    for (auto const& t : s.transports) transports.insert(t.id);

    std::set<QString> sinkWindowIds;
    for (auto const& sw : s.sinkWindows) sinkWindowIds.insert(sw.moduleId);

    std::set<QString> datapointIds;
    for (auto const& d : s.datapoints) datapointIds.insert(d.id);

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
        checkTransportRef(s.pollRanges[i].transport,
                          QStringLiteral("poll_range[%1]").arg(i), "transport");
    }
    for (int i = 0; i < s.sinkWindows.size(); ++i) {
        checkTransportRef(s.sinkWindows[i].transport,
                          QStringLiteral("sink_window[%1]").arg(i), "transport");
    }
    for (int i = 0; i < s.heartbeats.size(); ++i) {
        checkTransportRef(s.heartbeats[i].transport,
                          QStringLiteral("heartbeat[%1]").arg(i), "transport");
    }
    for (int i = 0; i < s.commands.size(); ++i) {
        checkTransportRef(s.commands[i].transport,
                          QStringLiteral("command[%1]").arg(i), "transport");
    }

    for (int i = 0; i < s.datapoints.size(); ++i) {
        auto const& d  = s.datapoints[i];
        auto const sec = QStringLiteral("datapoint[%1]").arg(i);
        if (d.hasSource) checkPortRef(d.source, sec + ".source");
        if (d.hasSink) {
            // Sink either references a SinkWindow by id (preferred for
            // staged batch writes) and / or names a transport `port` for
            // direct writes. When both are set, `window` wins; `port` is
            // expected to match the SinkWindow's underlying transport and
            // is purely informational.
            if (!d.sink.window.isEmpty() && !sinkWindowIds.count(d.sink.window)) {
                errs.push_back({sec + ".sink", "window",
                                QStringLiteral("references unknown sink_window '%1'")
                                    .arg(d.sink.window),
                                -1});
            }
            checkPortRef(d.sink, sec + ".sink");
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
