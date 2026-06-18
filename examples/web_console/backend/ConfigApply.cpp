// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ConfigApply.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

bool is32(std::string const& t) {
    static std::set<std::string> const s{"U32", "S32", "F32", "U64", "S64", "F64"};
    return s.count(t) > 0;
}

std::string wordOrder(std::string const& wo) {
    if (wo == "ABCD" || wo == "CDAB" || wo == "BADC" || wo == "DCBA") return wo;
    if (wo == "lo_hi") return "CDAB";
    return "ABCD";  // hi_lo / default
}

void writeFile(std::string const& path, std::string const& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

// Build a gateway TOML from the enabled DB config rows.
std::string buildRuntimeToml(DbClientPtr const& db) {
    std::ostringstream o;
    o << "[meta]\nproject = \"web_console_runtime\"\nlog_level = \"warn\"\n\n";

    for (auto const& r : db->execSqlSync("SELECT id,kind,params_json FROM codecs")) {
        auto const id = r["id"].as<std::string>();
        auto const kind = r["kind"].as<std::string>();
        if (kind != "enum_u16") continue;
        auto p = parseJsonOr(r["params_json"].as<std::string>());
        auto const& map = p.isMember("map") ? p["map"] : p;
        o << "[[codec]]\nid = \"" << id << "\"\nkind = \"enum_u16\"\nmap = { ";
        bool first = true;
        for (auto const& k : map.getMemberNames()) {
            if (!first) o << ", ";
            first = false;
            o << "\"" << k << "\" = \"" << map[k].asString() << "\"";
        }
        o << " }\n\n";
    }

    for (auto const& r : db->execSqlSync(
             "SELECT id,kind,params_json FROM transports WHERE enabled=1")) {
        auto const id = r["id"].as<std::string>();
        auto const kind = r["kind"].as<std::string>();
        auto p = parseJsonOr(r["params_json"].as<std::string>());
        o << "[[transport]]\nid = \"" << id << "\"\nkind = \"" << kind << "\"\n";
        if (kind == "modbus_tcp_client") {
            o << "host = \"" << p.get("host", "127.0.0.1").asString() << "\"\n"
              << "port = " << p.get("port", 502).asInt() << "\n"
              << "slave_id = " << p.get("slave_id", 1).asInt() << "\n";
        } else if (kind == "s7_client") {
            o << "host = \"" << p.get("host", "127.0.0.1").asString() << "\"\n"
              << "rack = " << p.get("rack", 0).asInt() << "\n"
              << "slot = " << p.get("slot", 1).asInt() << "\n"
              << "db = " << p.get("db", 1).asInt() << "\n";
        } else if (kind == "opc_ua_client") {
            o << "endpoint_url = \"" << p.get("endpoint_url", "opc.tcp://127.0.0.1:4840").asString() << "\"\n"
              << "node_id_template = \"" << p.get("node_id_template", "ns=2;s=Sim_%1").asString() << "\"\n"
              << "connect_timeout_ms = 4000\n";
        }
        o << "request_timeout_ms = 1000\n[transport.scheduler]\nkind = \"credit\"\nmax_inflight = "
          << (kind == "opc_ua_client" ? 4 : 1) << "\n\n";
    }

    for (auto const& r : db->execSqlSync(
             "SELECT id,transport_id,reg_table,start,count,period_ms FROM poll_ranges WHERE enabled=1")) {
        auto const start = r["start"].as<int>();
        o << "[[poll_range]]\nmodule_id = \"poll_" << r["id"].as<std::string>() << "\"\n"
          << "transport = \"" << r["transport_id"].as<std::string>() << "\"\n"
          << "table = \"" << r["reg_table"].as<std::string>() << "\"\n"
          << "range = [" << start << ", " << (start + r["count"].as<int>()) << "]\n"
          << "period_ms = " << r["period_ms"].as<int>() << "\n\n";
    }

    for (auto const& r : db->execSqlSync(
             "SELECT id,transport_id,reg_table,addr,type,word_order,scale,codec_id,kind "
             "FROM datapoints WHERE enabled=1")) {
        auto const type = r["type"].as<std::string>();
        o << "[[datapoint]]\nid = \"" << r["id"].as<std::string>() << "\"\n"
          << "kind = \"" << r["kind"].as<std::string>() << "\"\ntype = \"" << type << "\"\n"
          << "[datapoint.source]\nport = \"" << r["transport_id"].as<std::string>() << "\"\n"
          << "table = \"" << r["reg_table"].as<std::string>() << "\"\n"
          << "addr = " << r["addr"].as<int>() << "\n";
        double const scale = r["scale"].isNull() ? 1.0 : r["scale"].as<double>();
        if (scale != 1.0) o << "scale = " << scale << "\n";
        if (!r["codec_id"].isNull() && !r["codec_id"].as<std::string>().empty())
            o << "codec = \"" << r["codec_id"].as<std::string>() << "\"\n";
        if (is32(type)) o << "wordOrder = \"" << wordOrder(r["word_order"].as<std::string>()) << "\"\n";
        o << "\n";
    }
    return o.str();
}

void recordVersion(DbClientPtr const& db, std::string const& toml, std::string const& note) {
    db->execSqlSync("UPDATE config_versions SET status='superseded' WHERE status='active'");
    db->execSqlSync(
        "INSERT INTO config_versions(status,snapshot_json,note,applied_at) "
        "VALUES('active',$1,$2,strftime('%s','now'))",
        toml, note);
}

} // namespace

void registerConfigApply(RuntimeHost& rt, std::string genPath) {
    // POST /api/v1/config/validate
    app().registerHandler("/api/v1/config/validate",
        [genPath](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto toml = buildRuntimeToml(app().getDbClient());
                std::string const tmp = genPath + ".validate";
                writeFile(tmp, toml);
                std::string err;
                bool ok = RuntimeHost::validate(tmp, err);
                Json::Value d; d["valid"] = ok; if (!ok) d["error"] = err;
                cb(wc::ok(d));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});

    // POST /api/v1/config/apply
    app().registerHandler("/api/v1/config/apply",
        [&rt, genPath](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto db = app().getDbClient();
                auto toml = buildRuntimeToml(db);
                writeFile(genPath, toml);
                if (!rt.reload(genPath)) { cb(fail(3001, "apply failed: config invalid", k400BadRequest)); return; }
                recordVersion(db, toml, "apply");
                Json::Value d; d["applied"] = true;
                cb(wc::ok(d));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});

    // GET /api/v1/config/versions
    app().registerHandler("/api/v1/config/versions",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto r = app().getDbClient()->execSqlSync(
                    "SELECT version,status,note,applied_at,created_at FROM config_versions "
                    "ORDER BY version DESC LIMIT 50");
                cb(wc::ok(resultToArray(r)));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    // POST /api/v1/config/versions/{v}/rollback
    app().registerHandler("/api/v1/config/versions/{v}/rollback",
        [&rt, genPath](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string v) {
            try {
                auto db = app().getDbClient();
                auto r = db->execSqlSync(
                    "SELECT snapshot_json FROM config_versions WHERE version=$1", v);
                if (r.empty()) { cb(fail(1404, "version not found", k404NotFound)); return; }
                auto toml = r[0]["snapshot_json"].as<std::string>();
                writeFile(genPath, toml);
                if (!rt.reload(genPath)) { cb(fail(3001, "rollback failed: config invalid")); return; }
                recordVersion(db, toml, "rollback from v" + v);
                cb(wc::ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});
}

} // namespace wc
