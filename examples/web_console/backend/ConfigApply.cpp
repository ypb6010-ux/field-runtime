// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "ConfigApply.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <set>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

std::mutex g_applyMutex;

bool is32(std::string const& t) {
    static std::set<std::string> const s{"U32", "S32", "F32", "U64", "S64", "F64"};
    return s.count(t) > 0;
}

std::string wordOrder(std::string const& wo) {
    if (wo == "ABCD" || wo == "CDAB" || wo == "BADC" || wo == "DCBA") return wo;
    if (wo == "lo_hi") return "CDAB";
    return "ABCD";  // hi_lo / default
}

std::optional<std::string> readFile(std::string const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

Json::Value parseObject(std::string const& json, std::string const& context) {
    auto value = parseJsonOr(json, Json::Value(Json::nullValue));
    if (!value.isObject()) {
        throw std::runtime_error(context + " must contain a JSON object");
    }
    return value;
}

std::string tomlQuoted(std::string const& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char const c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"':  out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\t': out << "\\t"; break;
            case '\n': out << "\\n"; break;
            case '\f': out << "\\f"; break;
            case '\r': out << "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out << "\\u00" << std::hex << std::setw(2)
                        << std::setfill('0') << int(c) << std::dec;
                } else {
                    out << char(c);
                }
                break;
        }
    }
    out << '"';
    return out.str();
}

bool replaceFile(std::string const& path, std::string const& content,
                 std::string& error) {
    error.clear();
    std::filesystem::path const target(path);
    if (auto const parent = target.parent_path(); !parent.empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError) {
            error = "cannot create runtime config directory: "
                  + directoryError.message();
            return false;
        }
    }
    auto temp = target;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot open temporary config file";
            return false;
        }
        file.write(content.data(), std::streamsize(content.size()));
        file.flush();
        if (!file) {
            error = "cannot write temporary config file";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace runtime config (win32="
              + std::to_string(GetLastError()) + ")";
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        error = "cannot replace runtime config: " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
#endif
    return true;
}

bool applyRuntimeConfig(RuntimeHost& runtime, std::string const& path,
                        std::string const& content, std::string& error) {
    auto const previous = readFile(path);
    if (!replaceFile(path, content, error)) return false;
    bool const applied =
        runtime.running() ? runtime.reload(path) : runtime.start(path);
    if (applied) return true;

    if (previous.has_value()) {
        std::string restoreError;
        if (!replaceFile(path, *previous, restoreError)) {
            error = "runtime rejected config; failed to restore file: "
                  + restoreError;
            return false;
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            error = "runtime rejected config; failed to remove new file: "
                  + ec.message();
            return false;
        }
    }
    error = "runtime rejected config; previous runtime and file were retained";
    return false;
}

// Build a gateway TOML from the enabled DB config rows.
std::string buildRuntimeToml(DbClientPtr const& db) {
    std::ostringstream o;
    o << "[meta]\nproject = \"web_console_runtime\"\nlog_level = \"warn\"\n\n";

    for (auto const& r : db->execSqlSync("SELECT id,kind,params_json FROM codecs")) {
        auto const id = r["id"].as<std::string>();
        auto const kind = r["kind"].as<std::string>();
        if (kind != "enum_u16") continue;
        auto p = parseObject(
            r["params_json"].as<std::string>(),
            "codec " + id + " params_json");
        auto const& map = p.isMember("map") ? p["map"] : p;
        if (!map.isObject() || map.empty()) {
            throw std::runtime_error(
                "codec " + id + " requires a non-empty enum map");
        }
        o << "[[codec]]\nid = " << tomlQuoted(id)
          << "\nkind = \"enum_u16\"\nmap = { ";
        bool first = true;
        for (auto const& k : map.getMemberNames()) {
            if (!first) o << ", ";
            first = false;
            o << tomlQuoted(k) << " = " << tomlQuoted(map[k].asString());
        }
        o << " }\n\n";
    }

    for (auto const& r : db->execSqlSync(
             "SELECT id,kind,params_json,scheduler_json "
             "FROM transports WHERE enabled=1")) {
        auto const id = r["id"].as<std::string>();
        auto const kind = r["kind"].as<std::string>();
        auto p = parseObject(
            r["params_json"].as<std::string>(),
            "transport " + id + " params_json");
        o << "[[transport]]\nid = " << tomlQuoted(id)
          << "\nkind = " << tomlQuoted(kind) << "\n";
        if (kind == "modbus_tcp_client") {
            o << "host = "
              << tomlQuoted(p.get("host", "127.0.0.1").asString()) << "\n"
              << "port = " << p.get("port", 502).asInt() << "\n"
              << "slave_id = " << p.get("slave_id", 1).asInt() << "\n";
        } else if (kind == "s7_client") {
            o << "host = "
              << tomlQuoted(p.get("host", "127.0.0.1").asString()) << "\n"
              << "rack = " << p.get("rack", 0).asInt() << "\n"
              << "slot = " << p.get("slot", 1).asInt() << "\n"
              << "db = " << p.get("db", 1).asInt() << "\n";
        } else if (kind == "opc_ua_client") {
            o << "endpoint_url = "
              << tomlQuoted(
                     p.get("endpoint_url",
                           "opc.tcp://127.0.0.1:4840").asString())
              << "\nnode_id_template = "
              << tomlQuoted(
                     p.get("node_id_template", "ns=2;s=Sim_%1").asString())
              << "\n";
        } else {
            throw std::runtime_error(
                "transport " + id + " uses unsupported kind " + kind);
        }
        auto scheduler = parseObject(
            r["scheduler_json"].as<std::string>(),
            "transport " + id + " scheduler_json");
        auto const schedulerKind = scheduler.get(
            "kind",
            kind == "opc_ua_client" || kind == "s7_client"
                ? "credit" : "serial").asString();
        o << "reconnect_interval_ms = "
          << p.get("reconnect_interval_ms", 15000).asInt() << "\n"
          << "connect_timeout_ms = "
          << p.get("connect_timeout_ms", 3000).asInt() << "\n"
          << "request_timeout_ms = "
          << p.get("request_timeout_ms", 1000).asInt() << "\n"
          << "[transport.scheduler]\nkind = "
          << tomlQuoted(schedulerKind) << "\n"
          << "max_inflight = "
          << scheduler.get(
                 "max_inflight",
                 schedulerKind == "credit" ? 4 : 1).asInt() << "\n"
          << "max_queue_depth = "
          << scheduler.get("max_queue_depth", 256).asInt() << "\n"
          << "inter_request_gap_ms = "
          << scheduler.get("inter_request_gap_ms", 0).asInt() << "\n"
          << "starvation_guard_ms = "
          << scheduler.get("starvation_guard_ms", 5000).asInt() << "\n\n";
    }

    for (auto const& r : db->execSqlSync(
             "SELECT id,transport_id,reg_table,start,count,period_ms FROM poll_ranges WHERE enabled=1")) {
        auto const start = r["start"].as<int>();
        o << "[[poll_range]]\nmodule_id = "
          << tomlQuoted("poll_" + r["id"].as<std::string>()) << "\n"
          << "transport = "
          << tomlQuoted(r["transport_id"].as<std::string>()) << "\n"
          << "table = " << tomlQuoted(r["reg_table"].as<std::string>()) << "\n"
          << "range = [" << start << ", " << r["count"].as<int>() << "]\n"  // 加载器语义 [start, count]
          << "period_ms = " << r["period_ms"].as<int>() << "\n\n";
    }

    for (auto const& r : db->execSqlSync(
             "SELECT id,transport_id,reg_table,addr,type,word_order,scale,codec_id,kind "
             "FROM datapoints WHERE enabled=1")) {
        auto const type = r["type"].as<std::string>();
        o << "[[datapoint]]\nid = "
          << tomlQuoted(r["id"].as<std::string>()) << "\n"
          << "kind = " << tomlQuoted(r["kind"].as<std::string>())
          << "\ntype = " << tomlQuoted(type)
          << "\n[datapoint.source]\nport = "
          << tomlQuoted(r["transport_id"].as<std::string>()) << "\n"
          << "table = " << tomlQuoted(r["reg_table"].as<std::string>()) << "\n"
          << "addr = " << r["addr"].as<int>() << "\n";
        double const scale = r["scale"].isNull() ? 1.0 : r["scale"].as<double>();
        if (scale != 1.0) o << "scale = " << scale << "\n";
        if (!r["codec_id"].isNull() && !r["codec_id"].as<std::string>().empty())
            o << "codec = "
              << tomlQuoted(r["codec_id"].as<std::string>()) << "\n";
        if (is32(type)) {
            o << "wordOrder = "
              << tomlQuoted(wordOrder(r["word_order"].as<std::string>()))
              << "\n";
        }
        o << "\n";
    }
    return o.str();
}

std::uint64_t recordVersion(DbClientPtr const& db,
                            std::string const& toml,
                            std::string const& note,
                            std::string const& author) {
    db->execSqlSync("BEGIN IMMEDIATE");
    try {
        db->execSqlSync(
            "UPDATE config_versions SET status='superseded' "
            "WHERE status='active'");
        auto inserted = db->execSqlSync(
            "INSERT INTO config_versions(status,snapshot_json,note,author,"
            "applied_at) VALUES('active',$1,$2,$3,strftime('%s','now'))",
            toml, note, author);
        db->execSqlSync("COMMIT");
        return inserted.insertId();
    } catch (...) {
        try {
            db->execSqlSync("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
}

} // namespace

void registerConfigApply(RuntimeHost& rt, std::string genPath) {
    // GET /api/v1/config/status
    app().registerHandler("/api/v1/config/status",
        [&rt](HttpRequestPtr const&,
              std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto database = app().getDbClient();
                auto const rendered = buildRuntimeToml(database);
                auto active = database->execSqlSync(
                    "SELECT version,snapshot_json,author,note,applied_at "
                    "FROM config_versions WHERE status='active' "
                    "ORDER BY version DESC LIMIT 1");
                Json::Value result;
                result["runtimeRunning"] = rt.running();
                result["draftDirty"] =
                    active.empty()
                    || active[0]["snapshot_json"].as<std::string>() != rendered;
                result["renderedToml"] = rendered;
                Json::Value counts;
                for (auto const* table :
                     {"transports", "datapoints", "poll_ranges", "codecs",
                      "conversion_rules"}) {
                    counts[table] = database->execSqlSync(
                        std::string("SELECT COUNT(*) AS c FROM ") + table)[0]
                                            ["c"].as<int>();
                }
                result["counts"] = counts;
                if (!active.empty()) {
                    result["activeVersion"] =
                        Json::UInt64(active[0]["version"].as<std::uint64_t>());
                    result["author"] =
                        active[0]["author"].as<std::string>();
                    result["note"] = active[0]["note"].as<std::string>();
                    result["appliedAt"] =
                        Json::Int64(active[0]["applied_at"].as<std::int64_t>());
                }
                cb(wc::ok(result));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(3001, e.what(), k400BadRequest));
            }
        }, {Get});

    // POST /api/v1/config/validate
    app().registerHandler("/api/v1/config/validate",
        [genPath](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            std::unique_lock applyLock(g_applyMutex, std::try_to_lock);
            if (!applyLock.owns_lock()) {
                cb(fail(
                    1003,
                    "another configuration operation is already running",
                    k409Conflict));
                return;
            }
            try {
                auto toml = buildRuntimeToml(app().getDbClient());
                std::string const tmp = genPath + ".validate";
                std::string writeError;
                if (!replaceFile(tmp, toml, writeError)) {
                    cb(fail(3001, writeError, k500InternalServerError));
                    return;
                }
                std::string err;
                bool ok = RuntimeHost::validate(tmp, err);
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
                Json::Value d; d["valid"] = ok; if (!ok) d["error"] = err;
                cb(wc::ok(d));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(3001, e.what(), k400BadRequest));
            }
        }, {Post});

    // POST /api/v1/config/apply
    app().registerHandler("/api/v1/config/apply",
        [&rt, genPath](HttpRequestPtr const& request, std::function<void(HttpResponsePtr const&)>&& cb) {
            std::unique_lock applyLock(g_applyMutex, std::try_to_lock);
            if (!applyLock.owns_lock()) {
                cb(fail(
                    1003,
                    "another configuration operation is already running",
                    k409Conflict));
                return;
            }
            try {
                auto db = app().getDbClient();
                auto toml = buildRuntimeToml(db);
                std::string error;
                if (!applyRuntimeConfig(rt, genPath, toml, error)) {
                    cb(fail(3001, error, k400BadRequest));
                    return;
                }
                Json::Value d;
                d["applied"] = true;
                try {
                    auto const version = recordVersion(
                        db,
                        toml,
                        "apply",
                        request->attributes()->get<std::string>(
                            "auth_username"));
                    d["versionRecorded"] = true;
                    d["version"] = Json::UInt64(version);
                } catch (std::exception const& versionError) {
                    d["versionRecorded"] = false;
                    d["warning"] =
                        "runtime applied but version history failed: "
                      + std::string(versionError.what());
                    LOG_ERROR << d["warning"].asString();
                }
                cb(wc::ok(d));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(3001, e.what(), k400BadRequest));
            }
        }, {Post});

    // GET /api/v1/config/versions
    app().registerHandler("/api/v1/config/versions",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto r = app().getDbClient()->execSqlSync(
                    "SELECT version,status,author,note,applied_at,created_at FROM config_versions "
                    "ORDER BY version DESC LIMIT 50");
                cb(wc::ok(resultToArray(r)));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(3001, e.what(), k400BadRequest));
            }
        }, {Get});

    // POST /api/v1/config/versions/{v}/rollback
    app().registerHandler("/api/v1/config/versions/{v}/rollback",
        [&rt, genPath](HttpRequestPtr const& request, std::function<void(HttpResponsePtr const&)>&& cb, std::string v) {
            std::unique_lock applyLock(g_applyMutex, std::try_to_lock);
            if (!applyLock.owns_lock()) {
                cb(fail(
                    1003,
                    "another configuration operation is already running",
                    k409Conflict));
                return;
            }
            try {
                auto db = app().getDbClient();
                auto r = db->execSqlSync(
                    "SELECT snapshot_json FROM config_versions WHERE version=$1", v);
                if (r.empty()) { cb(fail(1404, "version not found", k404NotFound)); return; }
                auto toml = r[0]["snapshot_json"].as<std::string>();
                std::string error;
                if (!applyRuntimeConfig(rt, genPath, toml, error)) {
                    cb(fail(
                        3001,
                        "rollback failed: " + error,
                        k400BadRequest));
                    return;
                }
                Json::Value result;
                result["rolledBack"] = true;
                try {
                    auto const version = recordVersion(
                        db,
                        toml,
                        "rollback from v" + v,
                        request->attributes()->get<std::string>(
                            "auth_username"));
                    result["versionRecorded"] = true;
                    result["version"] = Json::UInt64(version);
                } catch (std::exception const& versionError) {
                    result["versionRecorded"] = false;
                    result["warning"] =
                        "runtime rolled back but version history failed: "
                      + std::string(versionError.what());
                    LOG_ERROR << result["warning"].asString();
                }
                cb(wc::ok(result));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(3001, e.what(), k400BadRequest));
            }
        }, {Post});
}

} // namespace wc
