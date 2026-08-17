// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "AuthControllers.h"
#include "Envelope.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <chrono>
#include <optional>
#include <span>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <drogon/utils/Utilities.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

using namespace drogon;
using namespace drogon::orm;

namespace wc {

namespace {

struct Session {
    std::string username;
    std::string role;
    std::set<std::string> perms;
    std::chrono::steady_clock::time_point expiresAt;
};

struct LoginAttempt {
    int failures = 0;
    std::chrono::steady_clock::time_point windowStart{};
    std::chrono::steady_clock::time_point blockedUntil{};
};

std::mutex g_mtx;
std::unordered_map<std::string, Session> g_tokens;   // bearer token -> session
std::unordered_map<std::string, LoginAttempt> g_loginAttempts;
constexpr auto kSessionLifetime = std::chrono::hours(8);
constexpr std::size_t kMaxSessions = 4096;
constexpr std::size_t kMaxSessionsPerUser = 20;
constexpr std::size_t kMaxLoginAttemptEntries = 4096;
constexpr auto kLoginWindow = std::chrono::minutes(15);
constexpr auto kLoginBlock = std::chrono::minutes(15);
constexpr int kMaxLoginFailures = 5;
constexpr int kMaxSourceLoginFailures = 20;
constexpr int kPbkdf2Iterations = 210000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

std::string hex(std::span<unsigned char const> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto const byte : bytes) out << std::setw(2) << int(byte);
    return out.str();
}

std::string randomSecret(std::size_t bytes) {
    std::vector<unsigned char> value(bytes);
    if (value.empty()
        || RAND_bytes(value.data(), static_cast<int>(value.size())) != 1) {
        throw std::runtime_error("secure random generation failed");
    }
    return hex(value);
}

std::optional<std::vector<unsigned char>> unhex(std::string const& text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> result(text.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto const digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        auto const high = digit(text[index * 2]);
        auto const low = digit(text[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return result;
}

bool verifyPassword(std::string const& password,
                    std::string const& stored,
                    bool& legacy) {
    legacy = stored.rfind("pbkdf2_sha256$", 0) != 0;
    if (legacy) {
        auto const candidate = utils::getMd5(password);
        return candidate.size() == stored.size()
            && CRYPTO_memcmp(candidate.data(), stored.data(), stored.size()) == 0;
    }
    auto const first = stored.find('$');
    auto const second = stored.find('$', first + 1);
    auto const third = stored.find('$', second + 1);
    if (first == std::string::npos || second == std::string::npos
        || third == std::string::npos) {
        return false;
    }
    int iterations = 0;
    try {
        std::size_t used = 0;
        iterations = std::stoi(
            stored.substr(first + 1, second - first - 1), &used, 10);
        if (used != second - first - 1 || iterations < 10000
            || iterations > 10000000) {
            return false;
        }
    } catch (...) {
        return false;
    }
    auto const salt = unhex(stored.substr(second + 1, third - second - 1));
    auto const expected = unhex(stored.substr(third + 1));
    if (!salt || !expected || salt->empty() || expected->size() != kHashBytes) {
        return false;
    }
    std::array<unsigned char, kHashBytes> actual{};
    if (PKCS5_PBKDF2_HMAC(
            password.data(),
            static_cast<int>(password.size()),
            salt->data(),
            static_cast<int>(salt->size()),
            iterations,
            EVP_sha256(),
            static_cast<int>(actual.size()),
            actual.data()) != 1) {
        return false;
    }
    return CRYPTO_memcmp(
               actual.data(), expected->data(), expected->size()) == 0;
}

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
    if (req->path().rfind("/ws/", 0) == 0) {
        return req->getParameter("token");  // WS handshake convenience
    }
    return {};
}

void purgeExpiredSessionsLocked() {
    auto const now = std::chrono::steady_clock::now();
    std::erase_if(g_tokens, [now](auto const& entry) {
        return entry.second.expiresAt <= now;
    });
}

void purgeLoginAttemptsLocked(
    std::chrono::steady_clock::time_point now) {
    std::erase_if(g_loginAttempts, [now](auto const& entry) {
        auto const& attempt = entry.second;
        return attempt.blockedUntil <= now
            && (attempt.windowStart.time_since_epoch().count() == 0
                || now - attempt.windowStart > kLoginWindow);
    });
}

bool loginBlocked(std::string const& username) {
    std::lock_guard lock(g_mtx);
    auto const now = std::chrono::steady_clock::now();
    purgeLoginAttemptsLocked(now);
    auto const found = g_loginAttempts.find(username);
    return found != g_loginAttempts.end()
        && found->second.blockedUntil > now;
}

void recordLoginFailure(std::string const& key, int maximumFailures) {
    std::lock_guard lock(g_mtx);
    auto const now = std::chrono::steady_clock::now();
    purgeLoginAttemptsLocked(now);
    if (g_loginAttempts.size() >= kMaxLoginAttemptEntries
        && !g_loginAttempts.contains(key)) {
        g_loginAttempts.erase(g_loginAttempts.begin());
    }
    auto& attempt = g_loginAttempts[key];
    if (attempt.windowStart.time_since_epoch().count() == 0
        || now - attempt.windowStart > kLoginWindow) {
        attempt.windowStart = now;
        attempt.failures = 0;
    }
    if (++attempt.failures >= maximumFailures) {
        attempt.blockedUntil = now + kLoginBlock;
    }
}

void clearLoginFailures(std::string const& username) {
    std::lock_guard lock(g_mtx);
    g_loginAttempts.erase(username);
}

std::string requiredWritePermission(std::string const& path) {
    if (path.rfind("/api/v1/auth/", 0) == 0) {
        return {};
    }
    if (path.rfind("/api/v1/users", 0) == 0
        || path.rfind("/api/v1/roles", 0) == 0) {
        return "user:manage";
    }
    if (path.rfind("/api/v1/system/settings", 0) == 0
        || path.rfind("/api/v1/system/maintenance", 0) == 0) {
        return "system:settings";
    }
    if (path.rfind("/api/v1/config/apply", 0) == 0
        || path.find("/rollback") != std::string::npos) {
        return "config:apply";
    }
    if (path.rfind("/api/v1/conversion", 0) == 0) {
        return "conversion:manage";
    }
    if (path.rfind("/api/v1/data", 0) == 0) {
        return "data:write";
    }
    if (path == "/api/v1/control/write"
        || path == "/api/v1/control/routes/activate") {
        return "data:write";
    }
    return "config:write";
}

std::string requiredReadPermission(std::string const& path) {
    if (path.rfind("/api/v1/auth/", 0) == 0) {
        return {};
    }
    if (path == "/api/v1/system/info") {
        return "data:read";
    }
    if (path.rfind("/api/v1/users", 0) == 0
        || path.rfind("/api/v1/roles", 0) == 0
        || path.rfind("/api/v1/audit", 0) == 0) {
        return "user:manage";
    }
    if (path.rfind("/api/v1/system/settings", 0) == 0
        || path.rfind("/api/v1/system/events", 0) == 0) {
        return "system:settings";
    }
    if (path.rfind("/api/v1/transports", 0) == 0
        || path.rfind("/api/v1/datapoints", 0) == 0
        || path.rfind("/api/v1/poll_ranges", 0) == 0
        || path.rfind("/api/v1/codecs", 0) == 0
        || path.rfind("/api/v1/config", 0) == 0) {
        return "config:read";
    }
    if (path == "/api/v1/control/config") return "config:read";
    if (path == "/api/v1/control/runtime") return "data:read";
    if (path.rfind("/api/v1/conversions", 0) == 0) {
        return "conversion:manage";
    }
    if (path.rfind("/api/v1/data", 0) == 0
        || path.rfind("/api/v1/runtime", 0) == 0) {
        return "data:read";
    }
    return {};
}

bool whitelisted(std::string const& path, HttpMethod m) {
    if (m == Options) return true;
    if (path == "/api/v1/auth/login") return true;
    if (path == "/api/v1/system/health") return true;
    if (path.rfind("/api/docs", 0) == 0) return true;  // Swagger UI / openapi
    if (path.rfind("/ws/", 0) == 0) return false;       // WebSocket needs a token (?token=)
    if (path.rfind("/api/", 0) != 0) return true;       // static assets
    return false;
}

} // namespace

std::string makePasswordHash(std::string const& password) {
    std::array<unsigned char, kSaltBytes> salt{};
    std::array<unsigned char, kHashBytes> digest{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1
        || PKCS5_PBKDF2_HMAC(
               password.data(),
               static_cast<int>(password.size()),
               salt.data(),
               static_cast<int>(salt.size()),
               kPbkdf2Iterations,
               EVP_sha256(),
               static_cast<int>(digest.size()),
               digest.data()) != 1) {
        throw std::runtime_error("password hashing failed");
    }
    return "pbkdf2_sha256$" + std::to_string(kPbkdf2Iterations) + "$"
         + hex(salt) + "$" + hex(digest);
}

void invalidateSessionsForUser(std::string const& username) {
    std::lock_guard lock(g_mtx);
    std::erase_if(g_tokens, [&username](auto const& entry) {
        return entry.second.username == username;
    });
}

bool isSessionTokenValid(std::string const& token) {
    if (token.empty()) return false;
    std::lock_guard lock(g_mtx);
    purgeExpiredSessionsLocked();
    return g_tokens.contains(token);
}

void registerAuth() {
    // Bootstrap a single administrator on first run. Prefer an explicit
    // environment secret; otherwise print a one-time random password.
    app().registerBeginningAdvice([] {
        try {
            auto db = app().getDbClient();
            auto n = db->execSqlSync("SELECT COUNT(*) AS c FROM users")[0]["c"].as<int>();
            if (n == 0) {
                auto const configured =
                    std::getenv("FIELD_CONSOLE_ADMIN_PASSWORD");
                std::string password =
                    configured && *configured
                        ? configured
                        : randomSecret(12);
                db->execSqlSync("INSERT INTO users(username,password_hash,role_id) VALUES($1,$2,'admin')",
                                "admin", makePasswordHash(password));
                if (!configured || !*configured) {
                    std::cerr
                        << "FieldRuntime Console initial login: admin / "
                        << password
                        << "\nChange the password after first login.\n";
                }
            }
        } catch (std::exception const& error) {
            std::cerr << "failed to bootstrap administrator: "
                      << error.what() << "\n";
            throw;
        }
    });

    std::string const corsOrigin = [] {
        auto const configured = std::getenv("FIELD_CONSOLE_CORS_ORIGIN");
        return configured ? std::string(configured) : std::string{};
    }();

    // Security headers and optional, explicitly scoped CORS.
    app().registerPostHandlingAdvice(
        [corsOrigin](
            HttpRequestPtr const& request,
            HttpResponsePtr const& resp) {
            if (!corsOrigin.empty()) {
                resp->addHeader("Access-Control-Allow-Origin", corsOrigin);
                resp->addHeader("Vary", "Origin");
                resp->addHeader(
                    "Access-Control-Allow-Headers",
                    "Content-Type, Authorization");
                resp->addHeader(
                    "Access-Control-Allow-Methods",
                    "GET, POST, PUT, DELETE, OPTIONS");
            }
            resp->addHeader("X-Content-Type-Options", "nosniff");
            resp->addHeader("Referrer-Policy", "no-referrer");
            if (request->path().rfind("/api/docs", 0) == 0) {
                resp->addHeader("X-Frame-Options", "SAMEORIGIN");
                resp->addHeader(
                    "Content-Security-Policy",
                    "default-src 'self'; "
                    "script-src 'self' 'unsafe-inline' https://unpkg.com; "
                    "style-src 'self' 'unsafe-inline' https://unpkg.com; "
                    "img-src 'self' data:; connect-src 'self'; "
                    "frame-ancestors 'self'; base-uri 'self'");
            } else {
                resp->addHeader("X-Frame-Options", "DENY");
                resp->addHeader(
                    "Content-Security-Policy",
                    "default-src 'self'; script-src 'self'; "
                    "style-src 'self' 'unsafe-inline'; "
                    "img-src 'self' data:; "
                    "connect-src 'self' ws: wss:; "
                    "frame-src 'self'; frame-ancestors 'none'; "
                    "base-uri 'self'; form-action 'self'");
            }
            if (request->path().rfind("/api/", 0) == 0) {
                resp->addHeader("Cache-Control", "no-store");
            }

            auto const method = request->method();
            if (resp->statusCode() >= k400BadRequest
                || (method != Post && method != Put && method != Delete)) {
                return;
            }
            auto const username =
                request->attributes()->get<std::string>("auth_username");
            if (username.empty()) return;
            app().getDbClient()->execSqlAsync(
                "INSERT INTO audit_log(ts,user_id,action,target) "
                "VALUES(strftime('%s','now')*1000,"
                "(SELECT id FROM users WHERE username=$1),$2,$3)",
                [](Result const&) {},
                [](DrogonDbException const& error) {
                    LOG_WARN << "audit insert failed: "
                             << error.base().what();
                },
                username,
                std::string(request->methodString()),
                request->path());
        });

    // Global auth gate.
    app().registerPreRoutingAdvice(
        [](HttpRequestPtr const& req, AdviceCallback&& stop, AdviceChainCallback&& pass) {
            if (whitelisted(req->path(), req->method())) { pass(); return; }
            Session sess;
            {
                std::lock_guard lk(g_mtx);
                purgeExpiredSessionsLocked();
                auto it = g_tokens.find(bearer(req));
                if (it == g_tokens.end()) {
                    stop(fail(2001, "unauthorized", k401Unauthorized));
                    return;
                }
                sess = it->second;
            }
            req->attributes()->insert("auth_username", sess.username);
            auto const m = req->method();
            bool const isWrite = (m == Post || m == Put || m == Delete);
            auto const required = isWrite
                ? requiredWritePermission(req->path())
                : requiredReadPermission(req->path());
            if (!required.empty() && !sess.perms.count(required)) {
                stop(fail(
                    2003,
                    "forbidden: missing permission " + required,
                    k403Forbidden));
                return;
            }
            pass();
        });

    auto const dummyPasswordHash =
        makePasswordHash(randomSecret(16));

    // POST /api/v1/auth/login
    app().registerHandler("/api/v1/auth/login",
        [dummyPasswordHash](
            HttpRequestPtr const& req,
            std::function<void(HttpResponsePtr const&)>&& cb) {
            auto j = req->getJsonObject();
            if (!j || !(*j)["username"].isString()
                || !(*j)["password"].isString()) {
                cb(fail(1001, "username and password are required"));
                return;
            }
            std::string const u = (*j)["username"].asString();
            std::string const pw = (*j)["password"].asString();
            if (u.empty() || u.size() > 64 || pw.empty()
                || pw.size() > 256) {
                cb(fail(2002, "bad credentials", k401Unauthorized));
                return;
            }
            auto const usernameAttemptKey = "user:" + u;
            auto const sourceAttemptKey =
                "source:" + req->peerAddr().toIp();
            if (loginBlocked(usernameAttemptKey)
                || loginBlocked(sourceAttemptKey)) {
                cb(fail(
                    2005,
                    "too many failed login attempts; try again later",
                    k429TooManyRequests));
                return;
            }
            try {
                auto r = app().getDbClient()->execSqlSync(
                    "SELECT password_hash,role_id FROM users WHERE username=$1 AND enabled=1", u);
                bool legacy = false;
                bool const valid = verifyPassword(
                    pw,
                    r.empty()
                        ? dummyPasswordHash
                        : r[0]["password_hash"].as<std::string>(),
                    legacy);
                if (r.empty() || !valid) {
                    recordLoginFailure(
                        usernameAttemptKey,
                        kMaxLoginFailures);
                    recordLoginFailure(
                        sourceAttemptKey,
                        kMaxSourceLoginFailures);
                    cb(fail(2002, "bad credentials", k401Unauthorized));
                    return;
                }
                clearLoginFailures(usernameAttemptKey);
                clearLoginFailures(sourceAttemptKey);
                if (legacy) {
                    app().getDbClient()->execSqlSync(
                        "UPDATE users SET password_hash=$1 WHERE username=$2",
                        makePasswordHash(pw),
                        u);
                }
                std::string const role = r[0]["role_id"].as<std::string>();
                auto perms = permsForRole(role);
                std::string const token = randomSecret(32);
                {
                    std::lock_guard lk(g_mtx);
                    purgeExpiredSessionsLocked();
                    if (g_tokens.size() >= kMaxSessions) {
                        cb(fail(
                            2004,
                            "too many active sessions",
                            k503ServiceUnavailable));
                        return;
                    }
                    auto const userSessions = std::count_if(
                        g_tokens.begin(),
                        g_tokens.end(),
                        [&u](auto const& entry) {
                            return entry.second.username == u;
                        });
                    if (userSessions >= kMaxSessionsPerUser) {
                        cb(fail(
                            2004,
                            "too many active sessions for this user",
                            k429TooManyRequests));
                        return;
                    }
                    g_tokens[token] = {
                        u,
                        role,
                        perms,
                        std::chrono::steady_clock::now() + kSessionLifetime};
                }
                app().getDbClient()->execSqlAsync(
                    "UPDATE users SET last_login_at=strftime('%s','now') "
                    "WHERE username=$1",
                    [](Result const&) {},
                    [](DrogonDbException const&) {},
                    u);
                app().getDbClient()->execSqlAsync(
                    "INSERT INTO audit_log(ts,user_id,action,target) "
                    "VALUES(strftime('%s','now')*1000,"
                    "(SELECT id FROM users WHERE username=$1),'login','session')",
                    [](Result const&) {},
                    [](DrogonDbException const&) {},
                    u);

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
            purgeExpiredSessionsLocked();
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
