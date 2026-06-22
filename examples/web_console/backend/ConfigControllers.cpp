// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ConfigControllers.h"
#include "Envelope.h"

#include <chrono>
#include <functional>
#include <string>
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
    return j.isMember(k) && !j[k].isNull() ? (j[k].isString() ? j[k].asString() : j[k].asString()) : d;
}
int i(Json::Value const& j, char const* k, int d = 0) {
    return j.isMember(k) && j[k].isNumeric() ? j[k].asInt() : d;
}
double d(Json::Value const& j, char const* k, double dv = 0) {
    return j.isMember(k) && j[k].isNumeric() ? j[k].asDouble() : dv;
}

// 从 URI(opc.tcp://host:port/path、tcp://host:port)解析 host/port。
void parseUri(std::string uri, std::string& host, int& port, int defPort) {
    auto p = uri.find("://");
    if (p != std::string::npos) uri = uri.substr(p + 3);
    auto slash = uri.find('/');
    if (slash != std::string::npos) uri = uri.substr(0, slash);
    auto colon = uri.rfind(':');
    if (colon != std::string::npos) { host = uri.substr(0, colon); try { port = std::stoi(uri.substr(colon + 1)); } catch (...) {} }
    else { host = uri; port = defPort; }
}

// 按 kind 从 params_json 取出探测目标 host:port。
bool targetFor(std::string const& kind, Json::Value const& p, std::string& host, int& port) {
    if (kind == "modbus_tcp_client") { host = p.get("host", "").asString(); port = p.get("port", 502).asInt(); }
    else if (kind == "s7_client")    { host = p.get("host", "").asString(); port = 102; }
    else if (kind == "opc_ua_client") parseUri(p.get("endpoint_url", "").asString(), host, port, 4840);
    else if (kind == "mqtt_client")   parseUri(p.get("broker_uri", "").asString(), host, port, 1883);
    else return false;
    return !host.empty() && port > 0;
}

// 异步 TCP 连接探测,run_for 超时绑定;不阻塞 io 线程到 OS 默认超时。
bool tcpProbe(std::string const& host, int port, int timeoutMs, int& latencyMs, std::string& err) {
    gateway_asio::io_context io;
    gateway_error_code ec;
    gateway_asio::ip::tcp::resolver res(io);
    auto eps = res.resolve(host, std::to_string(port), ec);
    if (ec) { err = "解析地址失败: " + ec.message(); return false; }
    gateway_asio::ip::tcp::socket sock(io);
    auto const t0 = std::chrono::steady_clock::now();
    bool done = false, good = false;
    std::string e2;
    gateway_asio::async_connect(sock, eps,
        [&](gateway_error_code e, auto const&) { done = true; good = !e; if (e) e2 = e.message(); });
    io.run_for(std::chrono::milliseconds(timeoutMs));
    latencyMs = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    gateway_error_code ig; sock.close(ig);
    if (!done) { err = "连接超时"; return false; }
    if (!good) { err = e2; return false; }
    return true;
}

// ── Generic list / get / delete (parameterless or single id bind) ───────────
void registerListGetDelete(std::string const& table, std::string const& idCol) {
    std::string const base = "/api/v1/" + table;

    app().registerHandler(base,
        [table](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try { cb(ok(resultToArray(db()->execSqlSync("SELECT * FROM " + table)))); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

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
                db()->execSqlSync("DELETE FROM " + table + " WHERE " + idCol + "=$1", id);
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
        }, {Put});
}

// ── /transports/kinds : per-protocol parameter JSON Schema (drives the
//    frontend's dynamic form). ──
HttpResponsePtr kindsResponse() {
    auto field = [](char const* type, char const* label, Json::Value def) {
        Json::Value f; f["type"] = type; f["label"] = label; f["default"] = def; return f;
    };
    Json::Value kinds(Json::arrayValue);

    auto kind = [&](char const* id, char const* label, Json::Value params) {
        Json::Value k; k["id"] = id; k["label"] = label; k["params"] = params; kinds.append(k);
    };

    { Json::Value p; p["host"] = field("string","Host",Json::Value("127.0.0.1"));
      p["port"] = field("int","Port",Json::Value(502));
      p["slave_id"] = field("int","Slave ID",Json::Value(1));
      kind("modbus_tcp_client","Modbus TCP", p); }
    { Json::Value p; p["endpoint_url"] = field("string","Endpoint",Json::Value("opc.tcp://127.0.0.1:4840"));
      p["node_id_template"] = field("string","Node template",Json::Value("ns=2;s=Sim_%1"));
      kind("opc_ua_client","OPC UA", p); }
    { Json::Value p; p["broker_uri"] = field("string","Broker",Json::Value("tcp://127.0.0.1:1883"));
      p["topic_template"] = field("string","Topic template",Json::Value("reg/%1"));
      p["qos"] = field("int","QoS",Json::Value(0));
      kind("mqtt_client","MQTT", p); }
    { Json::Value p; p["host"] = field("string","Host",Json::Value("127.0.0.1"));
      p["rack"] = field("int","Rack",Json::Value(0));
      p["slot"] = field("int","Slot",Json::Value(1));
      p["db"] = field("int","DB number",Json::Value(1));
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
            std::string const kind = (*j)["kind"].asString();
            Json::Value const& params = (*j)["params_json"];
            std::string host; int port = 0;
            Json::Value out;
            if (!targetFor(kind, params, host, port)) {
                out["ok"] = false; out["message"] = "无法从该协议参数解析连接地址";
                cb(ok(out)); return;
            }
            int latency = 0; std::string err;
            bool const good = tcpProbe(host, port, 2000, latency, err);
            out["ok"] = good;
            out["latencyMs"] = latency;
            out["message"] = good ? ("连接成功 " + host + ":" + std::to_string(port)) : err;
            if (!good) out["errorType"] = "connect";
            cb(ok(out));
        }, {Post});

    registerListGetDelete("transports", "id");
    registerListGetDelete("codecs", "id");
    registerListGetDelete("datapoints", "id");
    registerListGetDelete("poll_ranges", "id");

    // ── transports create/update ──
    post("/api/v1/transports", [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync(
            "INSERT INTO transports(id,name,kind,enabled,params_json,scheduler_json) "
            "VALUES($1,$2,$3,$4,$5,$6)",
            s(j,"id"), s(j,"name"), s(j,"kind"), i(j,"enabled",1),
            jsonCol(j["params_json"]), jsonCol(j["scheduler_json"]));
        cb(ok());
    });
    put("/api/v1/transports/{id}", [](std::string const& id, Json::Value const& j,
                                      std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync(
            "UPDATE transports SET name=$1,kind=$2,enabled=$3,params_json=$4,scheduler_json=$5,"
            "updated_at=strftime('%s','now') WHERE id=$6",
            s(j,"name"), s(j,"kind"), i(j,"enabled",1),
            jsonCol(j["params_json"]), jsonCol(j["scheduler_json"]), id);
        cb(ok());
    });

    // ── codecs create/update ──
    post("/api/v1/codecs", [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync("INSERT INTO codecs(id,kind,params_json,script_path) VALUES($1,$2,$3,$4)",
            s(j,"id"), s(j,"kind"), jsonCol(j["params_json"]), s(j,"script_path"));
        cb(ok());
    });
    put("/api/v1/codecs/{id}", [](std::string const& id, Json::Value const& j,
                                  std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync("UPDATE codecs SET kind=$1,params_json=$2,script_path=$3 WHERE id=$4",
            s(j,"kind"), jsonCol(j["params_json"]), s(j,"script_path"), id);
        cb(ok());
    });

    // ── datapoints create/update ──
    auto dpInsert = [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync(
            "INSERT INTO datapoints(id,transport_id,reg_table,addr,type,word_order,scale,codec_id,kind,enabled) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
            s(j,"id"), s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"addr"),
            s(j,"type","U16"), s(j,"word_order","hi_lo"), d(j,"scale",1.0),
            s(j,"codec_id"), s(j,"kind","Status"), i(j,"enabled",1));
        cb(ok());
    };
    post("/api/v1/datapoints", dpInsert);
    put("/api/v1/datapoints/{id}", [](std::string const& id, Json::Value const& j,
                                      std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync(
            "UPDATE datapoints SET transport_id=$1,reg_table=$2,addr=$3,type=$4,word_order=$5,"
            "scale=$6,codec_id=$7,kind=$8,enabled=$9 WHERE id=$10",
            s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"addr"), s(j,"type","U16"),
            s(j,"word_order","hi_lo"), d(j,"scale",1.0), s(j,"codec_id"),
            s(j,"kind","Status"), i(j,"enabled",1), id);
        cb(ok());
    });

    // ── poll_ranges create/update ──
    post("/api/v1/poll_ranges", [](Json::Value const& j, std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync(
            "INSERT INTO poll_ranges(id,transport_id,reg_table,start,count,period_ms,enabled) "
            "VALUES($1,$2,$3,$4,$5,$6,$7)",
            s(j,"id"), s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"start"),
            i(j,"count",1), i(j,"period_ms",1000), i(j,"enabled",1));
        cb(ok());
    });
    put("/api/v1/poll_ranges/{id}", [](std::string const& id, Json::Value const& j,
                                       std::function<void(HttpResponsePtr const&)>&& cb) {
        db()->execSqlSync(
            "UPDATE poll_ranges SET transport_id=$1,reg_table=$2,start=$3,count=$4,period_ms=$5,"
            "enabled=$6 WHERE id=$7",
            s(j,"transport_id"), s(j,"reg_table","HR"), i(j,"start"), i(j,"count",1),
            i(j,"period_ms",1000), i(j,"enabled",1), id);
        cb(ok());
    });
}

} // namespace wc
