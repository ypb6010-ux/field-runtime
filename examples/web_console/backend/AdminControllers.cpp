// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "AdminControllers.h"
#include "AuthControllers.h"
#include "Envelope.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/Utilities.h>

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

DbClientPtr db() { return app().getDbClient(); }

std::string s(Json::Value const& j, char const* k, std::string d = "") {
    return j.isMember(k) && j[k].isString() ? j[k].asString() : d;
}

int queryInt(HttpRequestPtr const& request,
             char const* name,
             int fallback,
             int minimum,
             int maximum) {
    auto const value = request->getParameter(name);
    if (value.empty()) return fallback;
    try {
        std::size_t used = 0;
        auto const parsed = std::stoll(value, &used, 10);
        if (used != value.size()) return fallback;
        return std::clamp(
            parsed,
            static_cast<long long>(minimum),
            static_cast<long long>(maximum));
    } catch (...) {
        return fallback;
    }
}

std::int64_t queryInt64(HttpRequestPtr const& request,
                        char const* name,
                        std::int64_t fallback,
                        std::int64_t minimum,
                        std::int64_t maximum) {
    auto const value = request->getParameter(name);
    if (value.empty()) return fallback;
    try {
        std::size_t used = 0;
        auto const parsed = std::stoll(value, &used, 10);
        if (used != value.size()) return fallback;
        return std::clamp(parsed, minimum, maximum);
    } catch (...) {
        return fallback;
    }
}

std::optional<std::string> validateSetting(std::string const& key,
                                           Json::Value const& value) {
    if (key == "sample_retention_days") {
        if (!value.isIntegral() || value.asInt64() < 1
            || value.asInt64() > 3650) {
            return "sample_retention_days must be in 1..3650";
        }
        return std::nullopt;
    }
    return "unknown setting: " + key;
}

bool boolLike(Json::Value const& value) {
    return value.isBool()
        || (value.isIntegral()
            && (value.asInt64() == 0 || value.asInt64() == 1));
}

} // namespace

void registerAdminControllers() {
    // GET /system/events?level=&page=&size=&since=<epoch-ms>
    app().registerHandler("/api/v1/system/events",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            int const page =
                queryInt(req, "page", 0, 0, 1'000'000);
            int const size =
                queryInt(req, "size", 100, 1, 1000);
            std::string const level = req->getParameter("level");
            auto const since = queryInt64(
                req,
                "since",
                0,
                0,
                std::numeric_limits<std::int64_t>::max());
            try {
                Result r;
                if (level.empty() && since == 0) {
                    r = db()->execSqlSync(
                        "SELECT ts,level,source,code,message FROM events "
                        "ORDER BY ts DESC LIMIT $1 OFFSET $2",
                        size,
                        page * size);
                } else if (level.empty()) {
                    r = db()->execSqlSync(
                        "SELECT ts,level,source,code,message FROM events "
                        "WHERE ts >= $1 ORDER BY ts DESC LIMIT $2 OFFSET $3",
                        since,
                        size,
                        page * size);
                } else if (since == 0) {
                    r = db()->execSqlSync(
                        "SELECT ts,level,source,code,message FROM events "
                        "WHERE level=$1 ORDER BY ts DESC LIMIT $2 OFFSET $3",
                        level,
                        size,
                        page * size);
                } else {
                    r = db()->execSqlSync(
                        "SELECT ts,level,source,code,message FROM events "
                        "WHERE level=$1 AND ts >= $2 "
                        "ORDER BY ts DESC LIMIT $3 OFFSET $4",
                        level,
                        since,
                        size,
                        page * size);
                }
                cb(ok(resultToArray(r)));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    // GET /audit?page=&size=&since=<epoch-ms>
    app().registerHandler("/api/v1/audit",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            int const page =
                queryInt(req, "page", 0, 0, 1'000'000);
            int const size =
                queryInt(req, "size", 100, 1, 1000);
            auto const since = queryInt64(
                req,
                "since",
                0,
                0,
                std::numeric_limits<std::int64_t>::max());
            try {
                auto r = since == 0
                    ? db()->execSqlSync(
                          "SELECT a.ts, u.username AS user, a.action, "
                          "a.target, a.detail_json FROM audit_log a "
                          "LEFT JOIN users u ON u.id=a.user_id "
                          "ORDER BY a.ts DESC LIMIT $1 OFFSET $2",
                          size,
                          page * size)
                    : db()->execSqlSync(
                          "SELECT a.ts, u.username AS user, a.action, "
                          "a.target, a.detail_json FROM audit_log a "
                          "LEFT JOIN users u ON u.id=a.user_id "
                          "WHERE a.ts >= $1 ORDER BY a.ts DESC "
                          "LIMIT $2 OFFSET $3",
                          since,
                          size,
                          page * size);
                cb(ok(resultToArray(r)));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    // GET /system/settings  → { key: value }
    app().registerHandler("/api/v1/system/settings",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                Json::Value d(Json::objectValue);
                for (auto const& row : db()->execSqlSync("SELECT key,value_json FROM settings"))
                    d[row["key"].as<std::string>()] = parseJsonOr(row["value_json"].as<std::string>(), Json::Value(row["value_json"].as<std::string>()));
                cb(ok(d));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    // PUT /system/settings  { key: value, ... }
    app().registerHandler("/api/v1/system/settings",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j || !j->isObject()) {
                cb(fail(1001, "invalid JSON body"));
                return;
            }
            for (auto const& key : j->getMemberNames()) {
                if (auto error = validateSetting(key, (*j)[key])) {
                    cb(fail(1001, *error));
                    return;
                }
            }
            try {
                db()->execSqlSync("BEGIN IMMEDIATE");
                try {
                for (auto const& k : j->getMemberNames())
                    db()->execSqlSync("INSERT INTO settings(key,value_json) VALUES($1,$2) ON CONFLICT(key) DO UPDATE SET value_json=$2",
                                      k, jsonCol((*j)[k]));
                    db()->execSqlSync("COMMIT");
                } catch (...) {
                    try { db()->execSqlSync("ROLLBACK"); } catch (...) {}
                    throw;
                }
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Put});

    // GET /users
    app().registerHandler("/api/v1/users",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try { cb(ok(resultToArray(db()->execSqlSync(
                "SELECT id,username,role_id,enabled,created_at,last_login_at FROM users ORDER BY id")))); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    // POST /users {username,role,password}
    app().registerHandler("/api/v1/users",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            auto const username = s(*j, "username");
            auto const password = s(*j, "password");
            auto const role = s(*j, "role", "viewer");
            if (username.empty() || username.size() > 64
                || std::any_of(
                    username.begin(), username.end(), [](unsigned char c) {
                        return std::iscntrl(c) || std::isspace(c);
                    })) {
                cb(fail(
                    1001,
                    "username must contain 1..64 non-whitespace characters"));
                return;
            }
            if (password.size() < 8 || password.size() > 256) {
                cb(fail(1001, "password must contain 8..256 characters"));
                return;
            }
            if (role != "viewer" && role != "operator" && role != "admin") {
                cb(fail(1001, "unknown role"));
                return;
            }
            if (!(*j)["enabled"].isNull() && !boolLike((*j)["enabled"])) {
                cb(fail(1001, "enabled must be boolean"));
                return;
            }
            try {
                db()->execSqlSync("INSERT INTO users(username,password_hash,role_id,enabled) VALUES($1,$2,$3,$4)",
                                  username, makePasswordHash(password),
                                  role, (*j).get("enabled", 1).asBool() ? 1 : 0);
                cb(ok());
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(4001, e.what(), k500InternalServerError));
            }
        }, {Post});

    // PUT /users/{id} {role,enabled}
    app().registerHandler("/api/v1/users/{id}",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            auto const role = s(*j, "role", "viewer");
            if (role != "viewer" && role != "operator" && role != "admin") {
                cb(fail(1001, "unknown role"));
                return;
            }
            if (!(*j)["enabled"].isNull() && !boolLike((*j)["enabled"])) {
                cb(fail(1001, "enabled must be boolean"));
                return;
            }
            try {
                auto existing = db()->execSqlSync(
                    "SELECT username,role_id,enabled FROM users WHERE id=$1", id);
                if (existing.empty()) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                bool const nextEnabled = (*j).get("enabled", 1).asBool();
                if (existing[0]["role_id"].as<std::string>() == "admin"
                    && (role != "admin" || !nextEnabled)
                    && db()->execSqlSync(
                           "SELECT COUNT(*) AS c FROM users "
                           "WHERE role_id='admin' AND enabled=1")[0]["c"].as<int>()
                           <= 1) {
                    cb(fail(1001, "cannot disable or demote the last administrator"));
                    return;
                }
                db()->execSqlSync("UPDATE users SET role_id=$1, enabled=$2 WHERE id=$3",
                                  role,
                                  nextEnabled ? 1 : 0,
                                  id);
                invalidateSessionsForUser(
                    existing[0]["username"].as<std::string>());
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Put});

    // DELETE /users/{id}
    app().registerHandler("/api/v1/users/{id}",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            try {
                auto existing = db()->execSqlSync(
                    "SELECT username,role_id,enabled FROM users WHERE id=$1", id);
                if (existing.empty()) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                if (existing[0]["role_id"].as<std::string>() == "admin"
                    && existing[0]["enabled"].as<int>() != 0
                    && db()->execSqlSync(
                           "SELECT COUNT(*) AS c FROM users "
                           "WHERE role_id='admin' AND enabled=1")[0]["c"].as<int>()
                           <= 1) {
                    cb(fail(1001, "cannot delete the last administrator"));
                    return;
                }
                db()->execSqlSync("DELETE FROM users WHERE id=$1", id);
                invalidateSessionsForUser(
                    existing[0]["username"].as<std::string>());
                cb(ok());
            }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Delete});

    // PUT /users/{id}/password {password}
    app().registerHandler("/api/v1/users/{id}/password",
        [](HttpRequestPtr const& req,
           std::function<void(HttpResponsePtr const&)>&& cb,
           std::string id) {
            auto j = req->getJsonObject();
            if (!j || !(*j)["password"].isString()) {
                cb(fail(1001, "password is required"));
                return;
            }
            auto const password = (*j)["password"].asString();
            if (password.size() < 8 || password.size() > 256) {
                cb(fail(1001, "password must contain 8..256 characters"));
                return;
            }
            try {
                auto existing = db()->execSqlSync(
                    "SELECT username FROM users WHERE id=$1", id);
                if (existing.empty()) {
                    cb(fail(1404, "not found", k404NotFound));
                    return;
                }
                db()->execSqlSync(
                    "UPDATE users SET password_hash=$1 WHERE id=$2",
                    makePasswordHash(password),
                    id);
                invalidateSessionsForUser(
                    existing[0]["username"].as<std::string>());
                cb(ok());
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            } catch (std::exception const& e) {
                cb(fail(4001, e.what(), k500InternalServerError));
            }
        }, {Put});

    // POST /system/maintenance/cleanup-samples
    app().registerHandler("/api/v1/system/maintenance/cleanup-samples",
        [](HttpRequestPtr const&,
           std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto result = db()->execSqlSync(
                    "DELETE FROM samples WHERE ts < "
                    "(strftime('%s','now') * 1000 - "
                    "COALESCE((SELECT CAST(value_json AS INTEGER) "
                    "FROM settings WHERE key='sample_retention_days'),30) "
                    "* 86400000)");
                Json::Value data;
                data["deleted"] =
                    Json::UInt64(result.affectedRows());
                cb(ok(data));
            } catch (DrogonDbException const& e) {
                cb(fail(4000, e.base().what(), k500InternalServerError));
            }
        }, {Post});

    // GET /roles  (+ 权限数 / 用户数)
    app().registerHandler("/api/v1/roles",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                Json::Value arr(Json::arrayValue);
                for (auto const& r : db()->execSqlSync("SELECT id,description FROM roles ORDER BY id")) {
                    auto const id = r["id"].as<std::string>();
                    Json::Value o;
                    o["id"] = id;
                    o["description"] = r["description"].as<std::string>();
                    o["perms"] = db()->execSqlSync("SELECT COUNT(*) AS c FROM role_permissions WHERE role_id=$1", id)[0]["c"].as<int>();
                    o["users"] = db()->execSqlSync("SELECT COUNT(*) AS c FROM users WHERE role_id=$1", id)[0]["c"].as<int>();
                    Json::Value perms(Json::arrayValue);
                    for (auto const& p : db()->execSqlSync("SELECT permission FROM role_permissions WHERE role_id=$1", id))
                        perms.append(p["permission"].as<std::string>());
                    o["permissions"] = perms;
                    arr.append(o);
                }
                cb(ok(arr));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});
}

} // namespace wc
