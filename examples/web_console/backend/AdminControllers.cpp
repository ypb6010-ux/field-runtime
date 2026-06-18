// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "AdminControllers.h"
#include "Envelope.h"

#include <algorithm>
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

void seedDemo() {
    try {
        auto d = db();
        if (d->execSqlSync("SELECT COUNT(*) AS c FROM events")[0]["c"].as<int>() == 0) {
            char const* ev[][4] = {
                {"info", "system", "1001", "web_console 启动"},
                {"info", "transport", "2001", "sim_modbus 已连接"},
                {"info", "transport", "2002", "sim_opc 已连接"},
                {"warn", "transport", "2010", "sim_s7 重连一次后恢复"},
                {"info", "config", "3001", "配置 v1 已发布生效"},
                {"warn", "polling", "4001", "poll_modbus 单次读超时"},
                {"error", "transport", "2099", "外部 PLC 连接被拒(演示)"},
                {"info", "conversion", "5001", "转换规则 r1 启用"},
            };
            for (auto& e : ev)
                d->execSqlSync("INSERT INTO events(ts,level,source,code,message) VALUES(strftime('%s','now')*1000,$1,$2,$3,$4)",
                               e[0], e[1], std::atoi(e[2]), e[3]);
        }
        if (d->execSqlSync("SELECT COUNT(*) AS c FROM audit_log")[0]["c"].as<int>() == 0) {
            char const* au[][3] = {
                {"admin", "login", "session"},
                {"admin", "config:apply", "config v1"},
                {"admin", "datapoint:create", "sim.temperature"},
                {"operator", "data:write", "sim_modbus@1"},
                {"admin", "user:create", "viewer"},
            };
            for (auto& a : au)
                d->execSqlSync("INSERT INTO audit_log(ts,user_id,action,target) VALUES(strftime('%s','now')*1000,(SELECT id FROM users WHERE username=$1),$2,$3)",
                               a[0], a[1], a[2]);
        }
    } catch (...) { /* tables may not exist yet on very first run */ }
}

} // namespace

void registerAdminControllers() {
    app().registerBeginningAdvice([] { seedDemo(); });

    // GET /system/events?level=&page=&size=
    app().registerHandler("/api/v1/system/events",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            int const page = std::max(0, req->getParameter("page").empty() ? 0 : std::stoi(req->getParameter("page")));
            int const size = std::clamp(req->getParameter("size").empty() ? 100 : std::stoi(req->getParameter("size")), 1, 1000);
            std::string const level = req->getParameter("level");
            try {
                Result r = level.empty()
                    ? db()->execSqlSync("SELECT ts,level,source,code,message FROM events ORDER BY ts DESC LIMIT $1 OFFSET $2", size, page * size)
                    : db()->execSqlSync("SELECT ts,level,source,code,message FROM events WHERE level=$1 ORDER BY ts DESC LIMIT $2 OFFSET $3", level, size, page * size);
                cb(ok(resultToArray(r)));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Get});

    // GET /audit
    app().registerHandler("/api/v1/audit",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            int const page = std::max(0, req->getParameter("page").empty() ? 0 : std::stoi(req->getParameter("page")));
            int const size = std::clamp(req->getParameter("size").empty() ? 100 : std::stoi(req->getParameter("size")), 1, 1000);
            try {
                auto r = db()->execSqlSync(
                    "SELECT a.ts, u.username AS user, a.action, a.target, a.detail_json "
                    "FROM audit_log a LEFT JOIN users u ON u.id=a.user_id ORDER BY a.ts DESC LIMIT $1 OFFSET $2", size, page * size);
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
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            try {
                for (auto const& k : j->getMemberNames())
                    db()->execSqlSync("INSERT INTO settings(key,value_json) VALUES($1,$2) ON CONFLICT(key) DO UPDATE SET value_json=$2",
                                      k, jsonCol((*j)[k]));
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
            try {
                db()->execSqlSync("INSERT INTO users(username,password_hash,role_id,enabled) VALUES($1,$2,$3,$4)",
                                  s(*j, "username"), utils::getMd5(s(*j, "password", "demo")),
                                  s(*j, "role", "viewer"), (*j).get("enabled", 1).asInt());
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});

    // PUT /users/{id} {role,enabled}
    app().registerHandler("/api/v1/users/{id}",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            try {
                db()->execSqlSync("UPDATE users SET role_id=$1, enabled=$2 WHERE id=$3",
                                  s(*j, "role", "viewer"), (*j).get("enabled", 1).asInt(), id);
                cb(ok());
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Put});

    // DELETE /users/{id}
    app().registerHandler("/api/v1/users/{id}",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            try { db()->execSqlSync("DELETE FROM users WHERE id=$1", id); cb(ok()); }
            catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Delete});

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
