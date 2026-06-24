// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Persistence.h"

#include <algorithm>
#include <sqlite3.h>
#include <utility>

#include "GatewayJson.h"

namespace core::gateway {

namespace {

std::string sqliteError(sqlite3* db) {
    auto const* msg = sqlite3_errmsg(db);
    return msg ? std::string(msg) : std::string("sqlite error");
}

void bindText(sqlite3_stmt* stmt, int index, std::string const& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), int(value.size()), SQLITE_TRANSIENT);
}

} // namespace

Persistence::Persistence() = default;

Persistence::~Persistence() {
    close();
}

bool Persistence::open(PersistenceConfig config, std::string& error) {
    close();
    m_config = std::move(config);
    if (!m_config.enable) return true;
    if (m_config.path.empty()) m_config.path = "field_gateway.db";
    if (m_config.maxRows <= 0) m_config.maxRows = 100000;
    if (m_config.backfillBatch <= 0) m_config.backfillBatch = 100;

    if (sqlite3_open(m_config.path.c_str(), &m_db) != SQLITE_OK) {
        error = sqliteError(m_db);
        close();
        return false;
    }

    // SqliteLogSink opens a second writer on the same file (logger thread). Wait
    // out its brief commit lock instead of failing telemetry writes with BUSY.
    sqlite3_busy_timeout(m_db, 2000);

    if (!exec("PRAGMA journal_mode=WAL;", error)) return false;
    if (!exec("PRAGMA synchronous=NORMAL;", error)) return false;
    if (!exec(
            "CREATE TABLE IF NOT EXISTS telemetry("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "dp_id TEXT NOT NULL,"
            "value TEXT NOT NULL,"
            "quality TEXT NOT NULL,"
            "ts INTEGER NOT NULL,"
            "published INTEGER NOT NULL DEFAULT 0"
            ");",
            error)) return false;
    if (!exec(
            "CREATE INDEX IF NOT EXISTS idx_telemetry_published_id "
            "ON telemetry(published,id);",
            error)) return false;
    if (!exec(
            "CREATE TABLE IF NOT EXISTS logs("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "level TEXT,"
            "module TEXT,"
            "key TEXT,"
            "msg TEXT,"
            "ts INTEGER NOT NULL"
            ");",
            error)) return false;
    return true;
}

void Persistence::close() {
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool Persistence::enabled() const {
    return m_db != nullptr && m_config.enable;
}

PersistenceConfig const& Persistence::config() const {
    return m_config;
}

std::optional<std::int64_t> Persistence::insertTelemetry(std::string const& dpId,
                                                         dp::Value const& value,
                                                         dp::DpState state,
                                                         dp::Timestamp timestamp,
                                                         std::string& error) {
    if (!enabled()) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    char const* sql =
        "INSERT INTO telemetry(dp_id,value,quality,ts,published) VALUES(?,?,?,?,0);";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqliteError(m_db);
        return std::nullopt;
    }

    auto const valueText = json::value(value);
    auto const quality = json::dpState(state);
    bindText(stmt, 1, dpId);
    bindText(stmt, 2, valueText);
    bindText(stmt, 3, quality);
    sqlite3_bind_int64(stmt, 4, sqlite3_int64(json::timestampMs(timestamp)));

    bool const ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqliteError(m_db);
    sqlite3_finalize(stmt);
    if (!ok) return std::nullopt;
    return std::int64_t(sqlite3_last_insert_rowid(m_db));
}

bool Persistence::markPublished(std::int64_t rowId, std::string& error) {
    if (!enabled()) return true;
    sqlite3_stmt* stmt = nullptr;
    char const* sql = "UPDATE telemetry SET published=1 WHERE id=?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqliteError(m_db);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, sqlite3_int64(rowId));
    bool const ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqliteError(m_db);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<TelemetryRow> Persistence::pendingTelemetry(int limit, std::string& error) {
    std::vector<TelemetryRow> rows;
    if (!enabled()) return rows;
    if (limit <= 0) limit = m_config.backfillBatch;

    sqlite3_stmt* stmt = nullptr;
    char const* sql =
        "SELECT id,dp_id,value,quality,ts FROM telemetry "
        "WHERE published=0 ORDER BY id LIMIT ?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqliteError(m_db);
        return rows;
    }
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TelemetryRow row;
        row.rowId = sqlite3_column_int64(stmt, 0);
        if (auto const* s = sqlite3_column_text(stmt, 1)) {
            row.dpId = reinterpret_cast<char const*>(s);
        }
        if (auto const* s = sqlite3_column_text(stmt, 2)) {
            row.valueJson = reinterpret_cast<char const*>(s);
        }
        if (auto const* s = sqlite3_column_text(stmt, 3)) {
            row.quality = reinterpret_cast<char const*>(s);
        }
        row.ts = sqlite3_column_int64(stmt, 4);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

bool Persistence::prune(std::string& error) {
    if (!enabled()) return true;
    if (m_config.maxRows <= 0) return true;

    sqlite3_stmt* stmt = nullptr;
    char const* sql =
        "DELETE FROM telemetry WHERE id IN ("
        "SELECT id FROM telemetry WHERE published=1 ORDER BY id ASC "
        "LIMIT (SELECT max(0, count(*) - ?) FROM telemetry)"
        ");";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqliteError(m_db);
        return false;
    }
    sqlite3_bind_int(stmt, 1, m_config.maxRows);
    bool const ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqliteError(m_db);
    sqlite3_finalize(stmt);
    return ok;
}

int Persistence::pendingCount(std::string& error) const {
    if (!enabled()) return 0;
    sqlite3_stmt* stmt = nullptr;
    char const* sql = "SELECT count(*) FROM telemetry WHERE published=0;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqliteError(m_db);
        return 0;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool Persistence::exec(char const* sql, std::string& error) {
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        error = err ? std::string(err) : sqliteError(m_db);
        sqlite3_free(err);
        return false;
    }
    return true;
}

} // namespace core::gateway
