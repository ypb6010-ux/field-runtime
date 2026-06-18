// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// web_console backend (W1 skeleton): Drogon HTTP app + SQLite schema init +
// system health endpoint + static SPA serving. Later stages add config/data/
// conversion controllers, the runtime host, WebSocket hub and RBAC.
#include "Platform.h"

#include "ConfigControllers.h"
#include "DataControllers.h"
#include "Envelope.h"
#include "RuntimeHost.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <drogon/drogon.h>

namespace {

std::chrono::steady_clock::time_point g_start;
wc::RuntimeHost g_runtime;

std::string readFile(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Run the schema through Drogon's own DbClient so it shares the exact same
// SQLite connection/file as every query (avoids a second sqlite layer pointing
// at a different file). The schema uses no ';' inside literals, so a simple
// split on ';' yields individual statements.
void runSchema(std::string const& schemaPath) {
    auto db = drogon::app().getDbClient();
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
            try { db->execSqlSync(stmt.substr(a)); }
            catch (std::exception const& e) { std::cerr << "schema stmt: " << e.what() << "\n"; }
        }
        stmt.clear();
    };
    for (char c : clean) { if (c == ';') flush(); else stmt += c; }
}

using wc::ok;

} // namespace

int main(int argc, char** argv) {
    std::string const dbPath  = argc > 1 ? argv[1] : "console.db";
    std::uint16_t const port  = argc > 2 ? std::uint16_t(std::stoi(argv[2])) : 8080;
    std::string const wwwRoot = argc > 3 ? argv[3] : WEB_CONSOLE_WWW;
    std::string const schema  = WEB_CONSOLE_SCHEMA;

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
            data["status"] = "ok";
            data["version"] = "0.1";
            data["uptime_s"] = Json::Int64(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - g_start).count());
            data["db"] = db ? "connected" : "down";
            cb(ok(std::move(data)));
        },
        {Get});

    // ── A 系统: GET /api/v1/system/info ──
    app().registerHandler(
        "/api/v1/system/info",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            Json::Value data, protos(Json::arrayValue);
            for (auto const* p : {"modbus_tcp_client", "opc_ua_client", "mqtt_client", "s7_client"})
                protos.append(p);
            data["protocols"] = protos;
            data["name"] = "FieldRuntime Console";
            cb(ok(std::move(data)));
        },
        {Get});

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

    if (!g_runtime.start(WEB_CONSOLE_RUNTIME_TOML))
        std::cerr << "RuntimeHost: failed to load " << WEB_CONSOLE_RUNTIME_TOML << "\n";
    wc::startSampler(g_runtime);

    app().setDocumentRoot(wwwRoot);
    app().addListener("0.0.0.0", port);
    std::cout << "web_console backend on http://0.0.0.0:" << port
              << " (db=" << dbPath << ", www=" << wwwRoot << ")" << std::endl;
    app().run();
    g_runtime.stop();
    return EXIT_SUCCESS;
}
