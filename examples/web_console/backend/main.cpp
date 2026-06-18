// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
//
// web_console backend (W1 skeleton): Drogon HTTP app + SQLite schema init +
// system health endpoint + static SPA serving. Later stages add config/data/
// conversion controllers, the runtime host, WebSocket hub and RBAC.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdlib>
// drogon/orm/SqlBinder.h uses htonll/ntohll; this SDK's winsock2.h does not
// expose them in this TU, so provide them (x64 host is little-endian).
inline unsigned long long htonll(unsigned long long v) { return _byteswap_uint64(v); }
inline unsigned long long ntohll(unsigned long long v) { return _byteswap_uint64(v); }
#endif

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <drogon/drogon.h>
#include <sqlite3.h>

namespace {

std::chrono::steady_clock::time_point g_start;

std::string readFile(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Run the schema (multi-statement) with the raw sqlite3 C API before Drogon
// opens its own connection. sqlite3_exec handles multiple statements in one go.
bool initSchema(std::string const& dbPath, std::string const& schemaPath) {
    std::string const sql = readFile(schemaPath);
    if (sql.empty()) {
        std::cerr << "schema file empty or missing: " << schemaPath << "\n";
        return false;
    }
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        std::cerr << "sqlite3_open(" << dbPath << ") failed: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return false;
    }
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "schema exec failed: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    return true;
}

// Uniform response envelope: { "code":0, "message":"ok", "data":... }.
drogon::HttpResponsePtr ok(Json::Value data) {
    Json::Value root;
    root["code"] = 0;
    root["message"] = "ok";
    root["data"] = std::move(data);
    return drogon::HttpResponse::newHttpJsonResponse(root);
}

} // namespace

int main(int argc, char** argv) {
    std::string const dbPath  = argc > 1 ? argv[1] : "console.db";
    std::uint16_t const port  = argc > 2 ? std::uint16_t(std::stoi(argv[2])) : 8080;
    std::string const wwwRoot = argc > 3 ? argv[3] : WEB_CONSOLE_WWW;
    std::string const schema  = WEB_CONSOLE_SCHEMA;

    if (!initSchema(dbPath, schema)) return EXIT_FAILURE;

    g_start = std::chrono::steady_clock::now();

    using namespace drogon;
    app().createDbClient("sqlite3", "", 0, dbPath, "", "", 1);

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

    app().setDocumentRoot(wwwRoot);
    app().addListener("0.0.0.0", port);
    std::cout << "web_console backend on http://0.0.0.0:" << port
              << " (db=" << dbPath << ", www=" << wwwRoot << ")" << std::endl;
    app().run();
    return EXIT_SUCCESS;
}
