// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// web_console backend: Drogon HTTP/WebSocket app, SQLite, embedded runtime,
// conversion engine, configuration publication, and static SPA serving.
#include "Platform.h"

#include "AdminControllers.h"
#include "AuthControllers.h"
#include "ConfigApply.h"
#include "ConfigControllers.h"
#include "ConversionEngine.h"
#include "DataControllers.h"
#include "DocsControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"
#include "WsControllers.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>

#include <drogon/drogon.h>

#ifndef WEB_CONSOLE_VERSION
#define WEB_CONSOLE_VERSION "development"
#endif

namespace {

std::chrono::steady_clock::time_point g_start;
wc::RuntimeHost g_runtime;

std::string readFile(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f.eof()) {
        throw std::runtime_error("cannot read file: " + path);
    }
    return ss.str();
}

// Run the schema through Drogon's own DbClient so it shares the exact same
// SQLite connection/file as every query (avoids a second sqlite layer pointing
// at a different file). The schema uses no ';' inside literals, so a simple
// split on ';' yields individual statements.
void runSchema(std::string const& schemaPath) {
    auto db = drogon::app().getDbClient();
    if (!db) {
        throw std::runtime_error("database client is unavailable");
    }
    db->execSqlSync("PRAGMA foreign_keys=ON");
    db->execSqlSync("PRAGMA journal_mode=WAL");
    db->execSqlSync("PRAGMA busy_timeout=5000");
    // Strip `--` line comments first (they may contain ';'), then split on ';'.
    std::istringstream in(readFile(schemaPath));
    std::string line, clean;
    while (std::getline(in, line)) {
        auto p = line.find("--");
        if (p != std::string::npos) line.erase(p);
        clean += line;
        clean += '\n';
    }
    std::string stmt;
    auto flush = [&] {
        auto a = stmt.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            db->execSqlSync(stmt.substr(a));
        }
        stmt.clear();
    };
    for (char c : clean) { if (c == ';') flush(); else stmt += c; }
    flush();
}

std::uint16_t parsePort(char const* text) {
    if (!text || !*text) {
        throw std::invalid_argument("empty port");
    }
    std::size_t used = 0;
    auto const value = std::stoul(text, &used, 10);
    if (used != std::string(text).size() || value == 0
        || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::out_of_range("port must be in 1..65535");
    }
    return static_cast<std::uint16_t>(value);
}

std::string resourcePath(
    std::string const& developmentPath,
    std::filesystem::path const& installPath) {
    std::error_code error;
    if (std::filesystem::exists(developmentPath, error) && !error) {
        return developmentPath;
    }
    error.clear();
    if (std::filesystem::exists(installPath, error) && !error) {
        return installPath.lexically_normal().string();
    }
    return developmentPath;
}

using wc::ok;

} // namespace

int main(int argc, char** argv) {
    std::string const dbPath  = argc > 1 ? argv[1] : "console.db";
    std::uint16_t port = 8080;
    try {
        if (argc > 2) port = parsePort(argv[2]);
    } catch (std::exception const& e) {
        std::cerr << "invalid listen port: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    auto const executableDirectory =
        std::filesystem::absolute(argv[0]).parent_path();
    auto const installedResources =
        executableDirectory / ".." / "share" / "web_console";
    std::string const wwwRoot =
        argc > 3
            ? argv[3]
            : resourcePath(
                  WEB_CONSOLE_WWW,
                  installedResources / "www");
    std::string const schema = resourcePath(
        WEB_CONSOLE_SCHEMA,
        installedResources / "schema.sql");
    std::string const initialRuntime = resourcePath(
        WEB_CONSOLE_RUNTIME_TOML,
        installedResources / "runtime.toml");
    std::string const generatedRuntime = dbPath + ".runtime.toml";
    std::error_code runtimePathError;
    std::string const startupRuntime =
        std::filesystem::is_regular_file(
            generatedRuntime,
            runtimePathError)
            && !runtimePathError
        ? generatedRuntime
        : initialRuntime;
    std::string const openapi = resourcePath(
        WEB_CONSOLE_OPENAPI,
        installedResources / "openapi.yaml");

    g_start = std::chrono::steady_clock::now();

    using namespace drogon;
    app().createDbClient("sqlite3", "", 0, dbPath, "", "", 1);
    app().registerBeginningAdvice([schema] { runSchema(schema); });

    // ── A 系统: GET /api/v1/system/health ──
    app().registerHandler(
        "/api/v1/system/health",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto db = app().getDbClient();
            Json::Value data;
            data["version"] = WEB_CONSOLE_VERSION;
            data["uptime_s"] = Json::Int64(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - g_start).count());
            data["runtime"] = g_runtime.running() ? "running" : "stopped";
            try {
                if (!db) throw std::runtime_error("database client unavailable");
                db->execSqlSync("SELECT 1");
                data["db"] = "connected";
                data["status"] =
                    g_runtime.running() ? "ok" : "degraded";
                auto response = ok(std::move(data));
                if (!g_runtime.running()) {
                    response->setStatusCode(k503ServiceUnavailable);
                }
                cb(response);
            } catch (std::exception const& error) {
                data["db"] = "down";
                data["status"] = "error";
                data["error"] = error.what();
                auto response = ok(std::move(data));
                response->setStatusCode(k503ServiceUnavailable);
                cb(response);
            }
        },
        {Get});

    // ── A 系统: GET /api/v1/system/info ──
    app().registerHandler(
        "/api/v1/system/info",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            Json::Value data, protos(Json::arrayValue);
            for (auto const* p : {"modbus_tcp_client", "opc_ua_client", "s7_client"})
                protos.append(p);
            data["protocols"] = protos;
            data["name"] = "FieldRuntime Console";
            cb(ok(std::move(data)));
        },
        {Get});

    wc::registerAuth();
    wc::registerDocs(openapi);
    wc::registerAdminControllers();
    wc::registerConfigControllers();

    // ── E 数据: live values from the embedded RuntimeHost ──
    app().registerHandler("/api/v1/data/latest",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            Json::Value arr(Json::arrayValue);
            for (auto const& d : g_runtime.datapoints()) {
                Json::Value o;
                o["id"] = d.id; o["value"] = wc::valueToJson(d.value);
                o["quality"] = d.state; o["ts"] = Json::Int64(d.ts);
                arr.append(o);
            }
            cb(ok(arr));
        }, {Get});

    app().registerHandler("/api/v1/data/points/{id}",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb, std::string id) {
            for (auto const& d : g_runtime.datapoints()) {
                if (d.id != id) continue;
                Json::Value o;
                o["id"] = d.id; o["value"] = wc::valueToJson(d.value);
                o["quality"] = d.state; o["ts"] = Json::Int64(d.ts);
                cb(ok(o)); return;
            }
            cb(wc::fail(1404, "datapoint not found", k404NotFound));
        }, {Get});

    app().registerHandler("/api/v1/runtime/transports",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            Json::Value arr(Json::arrayValue);
            for (auto const& t : g_runtime.transports()) {
                Json::Value o; o["id"] = t.id; o["kind"] = t.kind; o["state"] = t.state;
                arr.append(o);
            }
            cb(ok(arr));
        }, {Get});

    wc::registerDataControllers(g_runtime);
    wc::registerConfigApply(g_runtime, generatedRuntime);

    // Gateway startup keeps slow southbound connects asynchronous, so this is
    // now deterministic and does not need a detached thread whose lifetime
    // could outlive application shutdown.
    if (!g_runtime.start(startupRuntime)) {
        std::cerr << "RuntimeHost: failed to load "
                  << startupRuntime << "\n";
    }
    wc::registerConversionControllers();
    wc::startSampler(g_runtime);
    wc::startWsPump(g_runtime);
    wc::startConversionEngine(g_runtime);

    app().setDocumentRoot(wwwRoot);
    app().setClientMaxBodySize(1024 * 1024);
    app().addListener("0.0.0.0", port);
    std::cout << "web_console backend on http://0.0.0.0:" << port
              << " (db=" << dbPath << ", www=" << wwwRoot << ")" << std::endl;
    app().run();
    g_runtime.stop();
    return EXIT_SUCCESS;
}
