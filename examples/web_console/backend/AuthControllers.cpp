// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "AuthControllers.h"
#include "Envelope.h"

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/Utilities.h>

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

struct Session {
    std::string username;
    std::string role;
    std::set<std::string> perms;
};

std::mutex g_mtx;
std::unordered_map<std::string, Session> g_tokens;   // bearer token -> session

std::string hash(std::string const& pw) { return utils::getMd5(pw); }

std::set<std::string> permsForRole(std::string const& role) {
    std::set<std::string> p;
    try {
        auto r = app().getDbClient()->execSqlSync(
            "SELECT permission FROM role_permissions WHERE role_id=$1", role);
        for (auto const& row : r) p.insert(row["permission"].as<std::string>());
    } catch (...) {}
    return p;
}

std::string bearer(HttpRequestPtr const& req) {
    auto h = req->getHeader("authorization");
    if (h.rfind("Bearer ", 0) == 0) return h.substr(7);
    auto t = req->getParameter("token");   // WS handshake convenience
    return t;
}

bool whitelisted(std::string const& path, HttpMethod m) {
    if (m == Options) return true;
    if (path == "/api/v1/auth/login") return true;
    if (path == "/api/v1/system/health") return true;
    if (path.rfind("/api/docs", 0) == 0) return true;  // Swagger UI / openapi
    if (path.rfind("/api/", 0) != 0) return true;       // static assets, /ws
    return false;
    return false;
}

} // namespace

void registerAuth() {
    // Seed default users on first run (idempotent).
    app().registerBeginningAdvice([] {
        try {
            auto db = app().getDbClient();
            auto n = db->execSqlSync("SELECT COUNT(*) AS c FROM users")[0]["c"].as<int>();
            if (n == 0) {
                db->execSqlSync("INSERT INTO users(username,password_hash,role_id) VALUES($1,$2,'admin')",
                                "admin", hash("admin"));
                db->execSqlSync("INSERT INTO users(username,password_hash,role_id) VALUES($1,$2,'viewer')",
                                "viewer", hash("viewer"));
            }
        } catch (...) {}
    });

    // CORS for the dev frontend.
    app().registerPostHandlingAdvice(
        [](HttpRequestPtr const&, HttpResponsePtr const& resp) {
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        });

    // Global auth gate.
    app().registerPreRoutingAdvice(
        [](HttpRequestPtr const& req, AdviceCallback&& stop, AdviceChainCallback&& pass) {
            if (whitelisted(req->path(), req->method())) { pass(); return; }
            Session sess;
            {
                std::lock_guard lk(g_mtx);
                auto it = g_tokens.find(bearer(req));
                if (it == g_tokens.end()) {
                    stop(fail(2001, "unauthorized", k401Unauthorized));
                    return;
                }
                sess = it->second;
            }
            // Coarse RBAC: writes need config:write or data:write; viewers are read-only.
            auto const m = req->method();
            bool const isWrite = (m == Post || m == Put || m == Delete);
            if (isWrite && !sess.perms.count("config:write") && !sess.perms.count("data:write")) {
                stop(fail(2003, "forbidden: read-only role", k403Forbidden));
                return;
            }
            pass();
        });

    // POST /api/v1/auth/login
    app().registerHandler("/api/v1/auth/login",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j) { cb(fail(1001, "invalid JSON body")); return; }
            std::string const u = (*j)["username"].asString();
            std::string const pw = (*j)["password"].asString();
            try {
                auto r = app().getDbClient()->execSqlSync(
                    "SELECT password_hash,role_id FROM users WHERE username=$1 AND enabled=1", u);
                if (r.empty() || r[0]["password_hash"].as<std::string>() != hash(pw)) {
                    cb(fail(2002, "bad credentials", k401Unauthorized));
                    return;
                }
                std::string const role = r[0]["role_id"].as<std::string>();
                auto perms = permsForRole(role);
                std::string const token = utils::genRandomString(40);
                { std::lock_guard lk(g_mtx); g_tokens[token] = {u, role, perms}; }

                Json::Value user; user["username"] = u; user["role"] = role;
                Json::Value pa(Json::arrayValue);
                for (auto const& p : perms) pa.append(p);
                user["permissions"] = pa;
                Json::Value data; data["accessToken"] = token; data["user"] = user;
                cb(ok(data));
            } catch (DrogonDbException const& e) { cb(fail(4000, e.base().what(), k500InternalServerError)); }
        }, {Post});

    // GET /api/v1/auth/me
    app().registerHandler("/api/v1/auth/me",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            std::lock_guard lk(g_mtx);
            auto it = g_tokens.find(bearer(req));
            if (it == g_tokens.end()) { cb(fail(2001, "unauthorized", k401Unauthorized)); return; }
            Json::Value user; user["username"] = it->second.username; user["role"] = it->second.role;
            Json::Value pa(Json::arrayValue);
            for (auto const& p : it->second.perms) pa.append(p);
            user["permissions"] = pa;
            cb(ok(user));
        }, {Get});

    // POST /api/v1/auth/logout
    app().registerHandler("/api/v1/auth/logout",
        [](HttpRequestPtr const& req, std::function<void(HttpResponsePtr const&)>&& cb) {
            std::lock_guard lk(g_mtx);
            g_tokens.erase(bearer(req));
            cb(ok());
        }, {Post});
}

} // namespace wc
