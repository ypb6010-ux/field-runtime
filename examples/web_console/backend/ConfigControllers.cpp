// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ConfigControllers.h"
#include "Envelope.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include "GatewayAsio.h"

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

DbClientPtr db() { return app().getDbClient(); }

// Read a column value from a JSON body, falling back to a default.
std::string s(Json::Value const& j, char const* k, std::string d = "") {
    return j.isMember(k) && j[k].isString() ? j[k].asString() : d;
}
int i(Json::Value const& j, char const* k, int d = 0) {
    return j.isMember(k) && j[k].isNumeric() ? j[k].asInt() : d;
}
double d(Json::Value const& j, char const* k, double dv = 0) {
    return j.isMember(k) && j[k].isNumeric() ? j[k].asDouble() : dv;
}

bool enabled(Json::Value const& j) {
    auto const& value = j["enabled"];
    return value.isNull() ? true
         : value.isBool() ? value.asBool()
         : value.isIntegral() && (value.asInt64() == 0 || value.asInt64() == 1)
               ? value.asBool()
               : true;
}

bool validId(std::string const& id) {
    return !id.empty() && id.size() <= 128
        && id.find_first_of("/\\?#%") == std::string::npos
        && std::none_of(id.begin(), id.end(), [](unsigned char c) {
               return std::iscntrl(c) || std::isspace(c);
           });
}

bool safeText(std::string const& value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum
        && std::none_of(value.begin(), value.end(), [](unsigned char c) {
               return std::iscntrl(c);
           });
}

std::optional<std::string> rejectUnknownFields(
    Json::Value const& object,
    std::set<std::string> const& allowed,
    std::string const& context) {
    for (auto const& key : object.getMemberNames()) {
        if (!allowed.count(key)) {
            return context + " contains unsupported field: " + key;
        }
    }
    return std::nullopt;
}

std::optional<std::string> validateCommonId(Json::Value const& j) {
    auto const id = s(j, "id");
    if (!validId(id)) {
        return "id must contain 1..128 path-safe non-whitespace characters";
    }
    return std::nullopt;
}

bool intIn(Json::Value const& object, char const* key,
           int fallback, int minimum, int maximum, int& out) {
    auto const& value = object[key];
    if (value.isNull()) {
        out = fallback;
        return true;
    }
    if (!value.isIntegral()) return false;
    auto const parsed = value.asInt64();
    if (parsed < minimum || parsed > maximum) return false;
    out = static_cast<int>(parsed);
    return true;
}

std::optional<std::string> validateTransport(Json::Value const& j,
                                             bool requireId) {
    if (requireId) {
        if (auto error = validateCommonId(j)) return error;
    }
    static std::set<std::string> const kinds{
        "modbus_tcp_client", "opc_ua_client", "s7_client"};
    if (j.isMember("name")
        && (!j["name"].isString() || j["name"].asString().size() > 256)) {
        return "name must be a string of at most 256 characters";
    }
    auto const kind = s(j, "kind");
    if (!kinds.count(kind)) return "unsupported transport kind";
    auto const& params = j["params_json"];
    if (!params.isObject()) return "params_json must be an object";
    auto const& scheduler = j["scheduler_json"];
    if (!scheduler.isNull() && !scheduler.isObject()) {
        return "scheduler_json must be an object";
    }

    int unused = 0;
    static std::set<std::string> const timeoutFields{
        "reconnect_interval_ms", "connect_timeout_ms",
        "request_timeout_ms"};
    if (kind == "modbus_tcp_client") {
        auto const host = s(params, "host");
        if (!safeText(host, 255)
            || std::any_of(host.begin(), host.end(), [](unsigned char c) {
                   return std::isspace(c);
               })) {
            return "Modbus host must contain 1..255 non-whitespace characters";
        }
        auto allowed = timeoutFields;
        allowed.insert({"host", "port", "slave_id"});
        if (auto error = rejectUnknownFields(params, allowed, "params_json")) {
            return error;
        }
        if (!intIn(params, "port", 502, 1, 65535, unused)) {
            return "Modbus port must be in 1..65535";
        }
        if (!intIn(params, "slave_id", 1, 0, 247, unused)) {
            return "Modbus slave_id must be in 0..247";
        }
    } else if (kind == "s7_client") {
        auto const host = s(params, "host");
        if (!safeText(host, 255)
            || std::any_of(host.begin(), host.end(), [](unsigned char c) {
                   return std::isspace(c);
               })) {
            return "S7 host must contain 1..255 non-whitespace characters";
        }
        auto allowed = timeoutFields;
        allowed.insert({"host", "rack", "slot", "db"});
        if (auto error = rejectUnknownFields(params, allowed, "params_json")) {
            return error;
        }
        if (!intIn(params, "rack", 0, 0, 7, unused)
            || !intIn(params, "slot", 1, 0, 31, unused)
            || !intIn(params, "db", 1, 0, 65535, unused)) {
            return "S7 rack/slot/db is outside the supported range";
        }
    } else {
        auto const endpoint = s(params, "endpoint_url");
        if (!safeText(endpoint, 2048)
            || std::any_of(
                endpoint.begin(), endpoint.end(), [](unsigned char c) {
                    return std::isspace(c);
                })
            || endpoint.rfind("opc.tcp://", 0) != 0) {
            return "OPC UA endpoint_url must start with opc.tcp://";
        }
        auto const nodeTemplate = s(params, "node_id_template");
        if (!safeText(nodeTemplate, 1024)) {
            return "OPC UA node_id_template must contain 1..1024 characters";
        }
        auto allowed = timeoutFields;
        allowed.insert({"endpoint_url", "node_id_template"});
        if (auto error = rejectUnknownFields(params, allowed, "params_json")) {
            return error;
        }
    }

    for (auto const& bound :
         std::vector<std::tuple<char const*, int, int, int>>{
             {"reconnect_interval_ms", 15000, 0, 3600000},
             {"connect_timeout_ms", 3000, 1, 300000},
             {"request_timeout_ms", 1000, 1, 300000}}) {
        if (!intIn(
                params,
                std::get<0>(bound),
                std::get<1>(bound),
                std::get<2>(bound),
                std::get<3>(bound),
                unused)) {
            return std::string(std::get<0>(bound))
                 + " is outside the supported range";
        }
    }

    if (scheduler.isObject()) {
        static std::set<std::string> const schedulerFields{
            "kind", "max_inflight", "max_queue_depth",
            "inter_request_gap_ms", "starvation_guard_ms"};
        if (auto error = rejectUnknownFields(
                scheduler,
                schedulerFields,
                "scheduler_json")) {
            return error;
        }
        auto const schedulerKind = s(
            scheduler,
            "kind",
            kind == "modbus_tcp_client" ? "serial" : "credit");
        if (schedulerKind != "serial" && schedulerKind != "credit"
            && schedulerKind != "priority") {
            return "scheduler kind must be serial, credit, or priority";
        }
        for (auto const& bound :
             std::vector<std::tuple<char const*, int, int, int>>{
                 {"max_inflight", schedulerKind == "serial" ? 1 : 4, 1, 1024},
                 {"max_queue_depth", 256, 1, 100000},
                 {"inter_request_gap_ms", 0, 0, 60000},
                 {"starvation_guard_ms", 5000, 0, 3600000}}) {
            if (!intIn(
                    scheduler,
                    std::get<0>(bound),
                    std::get<1>(bound),
                    std::get<2>(bound),
                    std::get<3>(bound),
                    unused)) {
                return std::string(std::get<0>(bound))
                     + " is outside the supported range";
            }
        }
        if (schedulerKind == "serial"
            && scheduler.get("max_inflight", 1).asInt() != 1) {
            return "serial scheduler requires max_inflight=1";
        }
    }
    return std::nullopt;
}

int registerCount(std::string const& type) {
    if (type == "U32" || type == "S32" || type == "F32") return 2;
    if (type == "U64" || type == "S64" || type == "F64") return 4;
    return 1;
}

std::optional<std::string> validateDatapoint(Json::Value const& j,
                                             bool requireId) {
    if (requireId) {
        if (auto error = validateCommonId(j)) return error;
    }
    if (!validId(s(j, "transport_id"))) return "transport_id is required";
    if (s(j, "kind", "Status") != "Status") {
        return "the console currently supports source-only Status datapoints";
    }
    static std::set<std::string> const tables{"HR", "IR"};
    if (!tables.count(s(j, "reg_table", "HR"))) {
        return "reg_table must be HR or IR";
    }
    static std::set<std::string> const types{
        "U16", "S16", "U32", "S32", "U64", "S64",
        "F32", "F64", "EnumU16"};
    auto const type = s(j, "type", "U16");
    if (!types.count(type)) {
        return "unsupported source datapoint type";
    }
    int address = 0;
    if (!intIn(j, "addr", 0, 0, 65535, address)
        || address + registerCount(type) > 65536) {
        return "datapoint address range exceeds 0..65535";
    }
    auto const scale = d(j, "scale", 1.0);
    if (!std::isfinite(scale) || scale == 0.0) {
        return "scale must be finite and non-zero";
    }
    if (registerCount(type) > 1) {
        static std::set<std::string> const orders{
            "hi_lo", "lo_hi", "ABCD", "CDAB", "BADC", "DCBA"};
        if (!orders.count(s(j, "word_order", "hi_lo"))) {
            return "multi-register datapoints require a valid word_order";
        }
    }
    if (type == "EnumU16" && !validId(s(j, "codec_id"))) {
        return "EnumU16 requires codec_id";
    }
    return std::nullopt;
}

std::optional<std::string> validatePoll(Json::Value const& j,
                                       bool requireId) {
    if (requireId) {
        if (auto error = validateCommonId(j)) return error;
    }
    if (!validId(s(j, "transport_id"))) return "transport_id is required";
    auto const table = s(j, "reg_table", "HR");
    if (table != "HR" && table != "IR") return "reg_table must be HR or IR";
    int start = 0;
    int count = 0;
    int period = 0;
    if (!intIn(j, "start", 0, 0, 65535, start)
        || !intIn(j, "count", 1, 1, 125, count)
        || start + count > 65536) {
        return "poll range must stay within 0..65535 and contain 1..125 registers";
    }
    if (!intIn(j, "period_ms", 1000, 1, 86400000, period)) {
        return "period_ms must be in 1..86400000";
    }
    return std::nullopt;
}

std::optional<std::string> validateCodec(Json::Value const& j,
                                        bool requireId) {
    if (requireId) {
        if (auto error = validateCommonId(j)) return error;
    }
    if (s(j, "kind") != "enum_u16") {
        return "the console currently supports enum_u16 codecs only";
    }
    auto const& params = j["params_json"];
    if (!params.isObject()) return "params_json must be an object";
    auto const& map = params.isMember("map") ? params["map"] : params;
    if (!map.isObject() || map.empty()) return "enum_u16 map must not be empty";
    for (auto const& key : map.getMemberNames()) {
        unsigned value = 0;
        auto const parsed = std::from_chars(
            key.data(), key.data() + key.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()
            || value > 65535 || !map[key].isString()) {
            return "enum_u16 map keys must be decimal 0..65535 and values strings";
        }
    }
    return std::nullopt;
}

bool invalidEnabled(Json::Value const& j) {
    auto const& value = j["enabled"];
    return !value.isNull() && !value.isBool()
        && !(value.isIntegral()
             && (value.asInt64() == 0 || value.asInt64() == 1));
}

// 从 URI(opc.tcp://host:port/path、tcp://host:port)解析 host/port。
void parseUri(std::string uri, std::string& host, int& port, int defPort) {
    host.clear();
    port = defPort;
    auto p = uri.find("://");
    if (p != std::string::npos) uri = uri.substr(p + 3);
    auto slash = uri.find('/');
    if (slash != std::string::npos) uri = uri.substr(0, slash);
    std::string portText;
    if (!uri.empty() && uri.front() == '[') {
        auto const close = uri.find(']');
        if (close == std::string::npos) return;
        host = uri.substr(1, close - 1);
        if (close + 1 < uri.size()) {
            if (uri[close + 1] != ':') {
                host.clear();
                return;
            }
            portText = uri.substr(close + 2);
        }
    } else {
        auto const firstColon = uri.find(':');
        auto const lastColon = uri.rfind(':');
        if (firstColon != std::string::npos && firstColon == lastColon) {
            host = uri.substr(0, firstColon);
            portText = uri.substr(firstColon + 1);
        } else {
            host = uri;
        }
    }
    if (!portText.empty()) {
        int parsed = 0;
        auto const result = std::from_chars(
            portText.data(), portText.data() + portText.size(), parsed);
        if (result.ec != std::errc{}
            || result.ptr != portText.data() + portText.size()) {
            host.clear();
            return;
        }
        port = parsed;
    }
}

// 按 kind 从 params_json 取出探测目标 host:port。
bool targetFor(std::string const& kind, Json::Value const& p, std::string& host, int& port) {
    if (kind == "modbus_tcp_client") { host = p.get("host", "").asString(); port = p.get("port", 502).asInt(); }
    else if (kind == "s7_client")    { host = p.get("host", "").asString(); port = 102; }
    else if (kind == "opc_ua_client") parseUri(p.get("endpoint_url", "").asString(), host, port, 4840);
    else return false;
    return !host.empty() && port > 0 && port <= 65535;
}

using ProbeDone = std::function<void(bool, int, std::string)>;

class TcpProbeOperation final
    : public std::enable_shared_from_this<TcpProbeOperation> {
public:
    TcpProbeOperation(gateway_asio::io_context& io,
                      std::string host,
                      int port,
                      int timeoutMs,
                      ProbeDone done)
        : m_resolver(io)
        , m_socket(io)
        , m_timer(io)
        , m_host(std::move(host))
        , m_port(port)
        , m_timeoutMs(timeoutMs)
        , m_done(std::move(done)) {}

    void start() {
        m_startedAt = std::chrono::steady_clock::now();
        auto self = shared_from_this();
        m_timer.expires_after(std::chrono::milliseconds(m_timeoutMs));
        m_timer.async_wait([self](gateway_error_code const& error) {
            if (!error) self->finish(false, "连接超时");
        });
        m_resolver.async_resolve(
            m_host,
            std::to_string(m_port),
            [self](gateway_error_code const& error,
                   gateway_asio::ip::tcp::resolver::results_type endpoints) {
                if (error) {
                    self->finish(false, "解析地址失败: " + error.message());
                    return;
                }
                gateway_asio::async_connect(
                    self->m_socket,
                    endpoints,
                    [self](gateway_error_code const& connectError,
                           gateway_asio::ip::tcp::endpoint const&) {
                        self->finish(
                            !connectError,
                            connectError ? connectError.message()
                                         : std::string{});
                    });
            });
    }

private:
    void finish(bool ok, std::string error) {
        if (m_completed) return;
        m_completed = true;
        gateway_error_code ignored;
        try {
            m_timer.cancel();
        } catch (...) {
        }
        m_resolver.cancel();
        m_socket.close(ignored);
        auto const latency = int(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_startedAt)
                .count());
        auto done = std::move(m_done);
        if (done) done(ok, latency, std::move(error));
    }

    gateway_asio::ip::tcp::resolver m_resolver;
    gateway_asio::ip::tcp::socket m_socket;
    gateway_asio::steady_timer m_timer;
    std::string m_host;
    int m_port = 0;
    int m_timeoutMs = 2000;
    ProbeDone m_done;
    std::chrono::steady_clock::time_point m_startedAt;
    bool m_completed = false;
};

class TcpProbeService {
public:
    TcpProbeService()
        : m_guard(gateway_asio::make_work_guard(m_io))
        , m_thread([this] { m_io.run(); }) {}

    ~TcpProbeService() {
        m_guard.reset();
        m_io.stop();
        if (m_thread.joinable()) m_thread.join();
    }

    bool probe(std::string host,
               int port,
               int timeoutMs,
               ProbeDone done) {
        auto const previous =
            m_active.fetch_add(1, std::memory_order_acq_rel);
        if (previous >= kMaxActiveProbes) {
            m_active.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        try {
            gateway_asio::post(
                m_io,
                [this, host = std::move(host), port, timeoutMs,
                 done = std::move(done)]() mutable {
                    std::shared_ptr<ProbeDone> completion;
                    try {
                        completion = std::make_shared<ProbeDone>(
                            std::move(done));
                        std::make_shared<TcpProbeOperation>(
                            m_io,
                            std::move(host),
                            port,
                            timeoutMs,
                            [this, completion](
                                bool ok,
                                int latency,
                                std::string error) mutable {
                                m_active.fetch_sub(
                                    1,
                                    std::memory_order_acq_rel);
                                if (*completion) {
                                    (*completion)(
                                        ok,
                                        latency,
                                        std::move(error));
                                }
                            })
                            ->start();
                    } catch (std::exception const& error) {
                        m_active.fetch_sub(1, std::memory_order_acq_rel);
                        auto& callback =
                            completion ? *completion : done;
                        if (callback) {
                            callback(
                                false,
                                0,
                                "无法启动连接探测: "
                                    + std::string(error.what()));
                        }
                    } catch (...) {
                        m_active.fetch_sub(1, std::memory_order_acq_rel);
                        auto& callback =
                            completion ? *completion : done;
                        if (callback) {
                            callback(false, 0, "无法启动连接探测");
                        }
                    }
                });
        } catch (...) {
            m_active.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

private:
    gateway_asio::io_context m_io;
    gateway_asio::executor_work_guard<
        gateway_asio::io_context::executor_type> m_guard;
    std::thread m_thread;
    static constexpr std::size_t kMaxActiveProbes = 64;
    std::atomic_size_t m_active{0};
};

TcpProbeService& probeService() {
    static TcpProbeService service;
    return service;
}

// ── Generic list / get / delete (parameterless or single id bind) ───────────
void registerListGetDelete(std::string const& table,
                           std::string const& idCol,
                           bool withDelete = true) {
    std::string const base = "/api/v1/" + table;

    app().registerHandler(base,
        [table](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try { cb(ok(resultToArray(db()->execSqlSync("SELECT * FROM " + table)))); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    if (!withDelete) return;
    app().registerHandler(base + "/{id}",
        [table, idCol](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb,
                       std::string id) {
            try {
                auto r = db()->execSqlSync("SELECT * FROM " + table + " WHERE " + idCol + "=$1", id);
                if (r.empty()) { cb(fail(1404, "not found", k404NotFound)); return; }
                cb(ok(rowToJson(r[0])));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    app().registerHandler(base + "/{id}",
        [table, idCol](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb,
                       std::string id) {
            try {
                auto result = db()->execSqlSync(
                    "DELETE FROM " + table + " WHERE " + idCol + "=$1", id);
                if (result.affectedRows() == 0) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Delete});
}

template <typename Fn>
void post(std::string const& path, Fn&& fn) {
    app().registerHandler(path,
        [fn = std::forward<Fn>(fn)](HttpRequestPtr const& req,
                                    std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            try { fn(*j, std::move(cb)); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
            catch (std::exception const& e) { cb(fail(1001, e.what())); }
        }, {Post});
}

template <typename Fn>
void put(std::string const& path, Fn&& fn) {
    app().registerHandler(path,
        [fn = std::forward<Fn>(fn)](HttpRequestPtr const& req,
                                    std::function<void(HttpResponsePtr const&)>&& cb,
                                    std::string id) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            try { fn(id, *j, std::move(cb)); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
            catch (std::exception const& e) { cb(fail(1001, e.what())); }
        }, {Put});
}

// ── /transports/kinds : per-protocol parameter JSON Schema (drives the
//    frontend's dynamic form). ──
HttpResponsePtr kindsResponse() {
    auto field = [](char const* type,
                    char const* label,
                    Json::Value def,
                    bool required = true,
                    std::optional<int> minimum = std::nullopt,
                    std::optional<int> maximum = std::nullopt,
                    bool advanced = false) {
        Json::Value f;
        f["type"] = type;
        f["label"] = label;
        f["default"] = std::move(def);
        f["required"] = required;
        f["advanced"] = advanced;
        if (minimum) f["minimum"] = *minimum;
        if (maximum) f["maximum"] = *maximum;
        return f;
    };
    Json::Value kinds(Json::arrayValue);

    auto kind = [&](char const* id, char const* label, Json::Value params) {
        Json::Value k; k["id"] = id; k["label"] = label; k["params"] = params; kinds.append(k);
    };

    auto addTimeouts = [&](Json::Value& params) {
        params["connect_timeout_ms"] = field(
            "int", "连接超时", 3000, true, 1, 300000, true);
        params["request_timeout_ms"] = field(
            "int", "请求超时", 1000, true, 1, 300000, true);
        params["reconnect_interval_ms"] = field(
            "int", "重连间隔", 15000, true, 0, 3600000, true);
    };

    { Json::Value p; p["host"] = field("string","Host",Json::Value("127.0.0.1"));
      p["port"] = field("int","Port",Json::Value(502), true, 1, 65535);
      p["slave_id"] = field("int","Slave ID",Json::Value(1), true, 0, 247);
      addTimeouts(p);
      kind("modbus_tcp_client","Modbus TCP", p); }
    { Json::Value p; p["endpoint_url"] = field("string","Endpoint",Json::Value("opc.tcp://127.0.0.1:4840"));
      p["node_id_template"] = field("string","Node template",Json::Value("ns=2;s=Sim_%1"));
      addTimeouts(p);
      kind("opc_ua_client","OPC UA", p); }
    { Json::Value p; p["host"] = field("string","Host",Json::Value("127.0.0.1"));
      p["rack"] = field("int","Rack",Json::Value(0), true, 0, 7);
      p["slot"] = field("int","Slot",Json::Value(1), true, 0, 31);
      p["db"] = field("int","DB number",Json::Value(1), true, 0, 65535);
      addTimeouts(p);
      kind("s7_client","Siemens S7", p); }

    return ok(kinds);
}

} // namespace

void registerConfigControllers() {
    // GET /transports/kinds must be registered before the generic /{id} routes
    // would otherwise shadow it (drogon matches by specificity, but be explicit).
    app().registerHandler("/api/v1/transports/kinds",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            cb(kindsResponse());
        }, {Get});

    // POST /transports/{id}/test —— 用 body 的 {kind, params_json} 做 TCP 连接探测
    app().registerHandler("/api/v1/transports/{id}/test",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb, std::string) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            if (auto error = validateTransport(*j, false)) {
                cb(fail(1001, *error));
                return;
            }
            std::string const kind = (*j)["kind"].asString();
            Json::Value const& params = (*j)["params_json"];
            std::string host; int port = 0;
            Json::Value out;
            if (!targetFor(kind, params, host, port)) {
                out["ok"] = false; out["message"] = "无法从该协议参数解析连接地址";
                cb(ok(out)); return;
            }
            if (!probeService().probe(
                host,
                port,
                std::clamp(
                    params.get("connect_timeout_ms", 2000).asInt(),
                    100,
                    10000),
                [cb = std::move(cb), host = std::move(host), port](
                    bool good, int latency, std::string error) mutable {
                    Json::Value result;
                    result["ok"] = good;
                    result["latencyMs"] = latency;
                    result["message"] = good
                        ? ("连接成功 " + host + ":" + std::to_string(port))
                        : error;
                    if (!good) result["errorType"] = "connect";
                    cb(ok(result));
                })) {
                cb(fail(
                    1003,
                    "too many connection probes are already running",
                    k429TooManyRequests));
            }
        }, {Post});

    registerListGetDelete("transports", "id", false);
    registerListGetDelete("codecs", "id", false);
    registerListGetDelete("datapoints", "id", false);
    registerListGetDelete("poll_ranges", "id");

    app().registerHandler(
        "/api/v1/transports/{id}",
        [](HttpRequestPtr const&,
           std::function<void(HttpResponsePtr const&)>&& cb,
           std::string id) {
            try {
                std::set<std::string> sourceDatapoints;
                for (auto const& row : db()->execSqlSync(
                         "SELECT id FROM datapoints WHERE transport_id=$1",
                         id)) {
                    sourceDatapoints.insert(row["id"].as<std::string>());
                }
                for (auto const& row : db()->execSqlSync(
                         "SELECT id,source_json,dest_json "
                         "FROM conversion_rules")) {
                    auto source = parseJsonOr(
                        row["source_json"].as<std::string>());
                    auto destination = parseJsonOr(
                        row["dest_json"].as<std::string>());
                    if (s(destination, "transport") == id
                        || sourceDatapoints.count(s(source, "dp")) > 0) {
                        cb(fail(
                            1002,
                            "transport is referenced by conversion rule "
                                + row["id"].as<std::string>(),
                            k409Conflict));
                        return;
                    }
                }
                auto result = db()->execSqlSync(
                    "DELETE FROM transports WHERE id=$1",
                    id);
                if (result.affectedRows() == 0) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                cb(ok());
            } catch (DrogonDbException const& error) {
                cb(fail(
                    4000,
                    error.base().what(),
                    k500InternalServerError));
            }
        },
        {Delete});

    app().registerHandler(
        "/api/v1/datapoints/{id}",
        [](HttpRequestPtr const&,
           std::function<void(HttpResponsePtr const&)>&& cb,
           std::string id) {
            try {
                for (auto const& row : db()->execSqlSync(
                         "SELECT id,source_json FROM conversion_rules")) {
                    auto source = parseJsonOr(
                        row["source_json"].as<std::string>());
                    if (s(source, "dp") == id) {
                        cb(fail(
                            1002,
                            "datapoint is referenced by conversion rule "
                                + row["id"].as<std::string>(),
                            k409Conflict));
                        return;
                    }
                }
                auto result = db()->execSqlSync(
                    "DELETE FROM datapoints WHERE id=$1",
                    id);
                if (result.affectedRows() == 0) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                cb(ok());
            } catch (DrogonDbException const& error) {
                cb(fail(
                    4000,
                    error.base().what(),
                    k500InternalServerError));
            }
        },
        {Delete});

    app().registerHandler(
        "/api/v1/codecs/{id}",
        [](HttpRequestPtr const&,
           std::function<void(HttpResponsePtr const&)>&& cb,
           std::string id) {
            try {
                auto references = db()->execSqlSync(
                    "SELECT id FROM datapoints WHERE codec_id=$1 LIMIT 1",
                    id);
                if (!references.empty()) {
                    cb(fail(
                        1002,
                        "codec is referenced by datapoint "
                            + references[0]["id"].as<std::string>(),
                        k409Conflict));
                    return;
                }
                auto result = db()->execSqlSync(
                    "DELETE FROM codecs WHERE id=$1",
                    id);
                if (result.affectedRows() == 0) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                cb(ok());
            } catch (DrogonDbException const& error) {
                cb(fail(
                    4000,
                    error.base().what(),
                    k500InternalServerError));
            }
        },
        {Delete});

    // ── transports create/update ──
    post("/api/v1/transports", [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        if (invalidEnabled(j)) { cb(fail(1001, "enabled must be boolean")); return; }
        if (auto error = validateTransport(j, true)) { cb(fail(1001, *error)); return; }
        db()->execSqlSync(
            "INSERT INTO transports(id,name,kind,enabled,params_json,scheduler_json) "
            "VALUES($1,$2,$3,$4,$5,$6)",
            s(j,"id"), s(j,"name"), s(j,"kind"), enabled(j) ? 1 : 0,
            jsonCol(j["params_json"]), jsonCol(j["scheduler_json"]));
        cb(ok());
    });
    put("/api/v1/transports/{id}", [](std::string const& id, Json::Value const& j,
                                      std::function<void(HttpResponsePtr const&)>&& cb) {
        if (invalidEnabled(j)) { cb(fail(1001, "enabled must be boolean")); return; }
        if (auto error = validateTransport(j, false)) { cb(fail(1001, *error)); return; }
        auto result = db()->execSqlSync(
            "UPDATE transports SET name=$1,kind=$2,enabled=$3,params_json=$4,scheduler_json=$5,"
            "updated_at=strftime('%s','now') WHERE id=$6",
            s(j,"name"), s(j,"kind"), enabled(j) ? 1 : 0,
            jsonCol(j["params_json"]), jsonCol(j["scheduler_json"]), id);
        if (result.affectedRows() == 0) { cb(fail(1404, "not found", k404NotFound)); return; }
        cb(ok());
    });

    // ── codecs create/update ──
    post("/api/v1/codecs", [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        if (auto error = validateCodec(j, true)) { cb(fail(1001, *error)); return; }
        db()->execSqlSync("INSERT INTO codecs(id,kind,params_json,script_path) VALUES($1,$2,$3,$4)",
            s(j,"id"), s(j,"kind"), jsonCol(j["params_json"]), s(j,"script_path"));
        cb(ok());
    });
    put("/api/v1/codecs/{id}", [](std::string const& id, Json::Value const& j,
                                  std::function<void(HttpResponsePtr const&)>&& cb) {
        if (auto error = validateCodec(j, false)) { cb(fail(1001, *error)); return; }
        auto result = db()->execSqlSync("UPDATE codecs SET kind=$1,params_json=$2,script_path=$3 WHERE id=$4",
            s(j,"kind"), jsonCol(j["params_json"]), s(j,"script_path"), id);
        if (result.affectedRows() == 0) { cb(fail(1404, "not found", k404NotFound)); return; }
        cb(ok());
    });

    // ── datapoints create/update ──
    auto dpInsert = [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        if (invalidEnabled(j)) { cb(fail(1001, "enabled must be boolean")); return; }
        if (auto error = validateDatapoint(j, true)) { cb(fail(1001, *error)); return; }
        db()->execSqlSync(
            "INSERT INTO datapoints(id,transport_id,reg_table,addr,type,word_order,scale,codec_id,kind,enabled) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
            s(j,"id"), s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"addr"),
            s(j,"type","U16"), s(j,"word_order","hi_lo"), d(j,"scale",1.0),
            s(j,"codec_id"), "Status", enabled(j) ? 1 : 0);
        cb(ok());
    };
    post("/api/v1/datapoints", dpInsert);
    put("/api/v1/datapoints/{id}", [](std::string const& id, Json::Value const& j,
                                      std::function<void(HttpResponsePtr const&)>&& cb) {
        if (invalidEnabled(j)) { cb(fail(1001, "enabled must be boolean")); return; }
        if (auto error = validateDatapoint(j, false)) { cb(fail(1001, *error)); return; }
        auto result = db()->execSqlSync(
            "UPDATE datapoints SET transport_id=$1,reg_table=$2,addr=$3,type=$4,word_order=$5,"
            "scale=$6,codec_id=$7,kind=$8,enabled=$9 WHERE id=$10",
            s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"addr"), s(j,"type","U16"),
            s(j,"word_order","hi_lo"), d(j,"scale",1.0), s(j,"codec_id"),
            "Status", enabled(j) ? 1 : 0, id);
        if (result.affectedRows() == 0) { cb(fail(1404, "not found", k404NotFound)); return; }
        cb(ok());
    });

    // ── poll_ranges create/update ──
    post("/api/v1/poll_ranges", [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        if (invalidEnabled(j)) { cb(fail(1001, "enabled must be boolean")); return; }
        if (auto error = validatePoll(j, true)) { cb(fail(1001, *error)); return; }
        db()->execSqlSync(
            "INSERT INTO poll_ranges(id,transport_id,reg_table,start,count,period_ms,enabled) "
            "VALUES($1,$2,$3,$4,$5,$6,$7)",
            s(j,"id"), s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"start"),
            i(j,"count",1), i(j,"period_ms",1000), enabled(j) ? 1 : 0);
        cb(ok());
    });
    put("/api/v1/poll_ranges/{id}", [](std::string const& id, Json::Value const& j,
                                       std::function<void(HttpResponsePtr const&)>&& cb) {
        if (invalidEnabled(j)) { cb(fail(1001, "enabled must be boolean")); return; }
        if (auto error = validatePoll(j, false)) { cb(fail(1001, *error)); return; }
        auto result = db()->execSqlSync(
            "UPDATE poll_ranges SET transport_id=$1,reg_table=$2,start=$3,count=$4,period_ms=$5,"
            "enabled=$6 WHERE id=$7",
            s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"start"), i(j,"count",1),
            i(j,"period_ms",1000), enabled(j) ? 1 : 0, id);
        if (result.affectedRows() == 0) { cb(fail(1404, "not found", k404NotFound)); return; }
        cb(ok());
    });
}

} // namespace wc
