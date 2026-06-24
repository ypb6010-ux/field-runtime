// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "SqliteLogSink.h"

#include <chrono>
#include <sqlite3.h>
#include <utility>

#include "GatewayJson.h"

namespace core::gateway {

namespace {

std::int64_t toMs(core::log::LogTime ts) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               ts.time_since_epoch())
        .count();
}

// Renders the structured fields onto the message text so they survive in the DB
// (the console/file sinks keep them via formatLine; here columns are flat).
std::string withFields(std::string message,
                       std::map<std::string, dp::Value> const& fields) {
    for (auto const& [name, value] : fields) {
        message += ' ';
        message += name;
        message += '=';
        message += json::value(value);
    }
    return message;
}

} // namespace

SqliteLogSink::SqliteLogSink(std::string dbPath,
                             core::log::LogLevel minLevel,
                             int maxRows)
    : m_path(std::move(dbPath))
    , m_minLevel(minLevel)
    , m_maxRows(maxRows > 0 ? maxRows : 50000) {}

SqliteLogSink::~SqliteLogSink() {
    if (m_insert) sqlite3_finalize(m_insert);
    if (m_db) sqlite3_close(m_db);
}

bool SqliteLogSink::ensureOpen() {
    if (m_db) return true;
    if (m_failed) return false;

    if (sqlite3_open(m_path.c_str(), &m_db) != SQLITE_OK) {
        m_failed = true;
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        return false;
    }
    // Same file as the telemetry path (WAL set there). busy_timeout lets a log
    // insert wait out the brief telemetry write lock instead of erroring BUSY.
    sqlite3_busy_timeout(m_db, 2000);
    sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db,
                 "CREATE TABLE IF NOT EXISTS logs("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "level TEXT,"
                 "module TEXT,"
                 "key TEXT,"
                 "msg TEXT,"
                 "ts INTEGER NOT NULL"
                 ");",
                 nullptr, nullptr, nullptr);

    char const* sql =
        "INSERT INTO logs(level,module,key,msg,ts) VALUES(?,?,?,?,?);";
    if (sqlite3_prepare_v2(m_db, sql, -1, &m_insert, nullptr) != SQLITE_OK) {
        m_failed = true;
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    return true;
}

void SqliteLogSink::insertRow(char const* level,
                              std::string const& module,
                              std::string const& key,
                              std::string const& msg,
                              std::int64_t tsMs) {
    if (!ensureOpen()) return;
    sqlite3_reset(m_insert);
    sqlite3_clear_bindings(m_insert);
    sqlite3_bind_text(m_insert, 1, level, -1, SQLITE_STATIC);
    sqlite3_bind_text(m_insert, 2, module.c_str(), int(module.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(m_insert, 3, key.c_str(), int(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(m_insert, 4, msg.c_str(), int(msg.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(m_insert, 5, sqlite3_int64(tsMs));
    if (sqlite3_step(m_insert) == SQLITE_DONE) {
        ++m_inserts;
        pruneIfDue();
    }
}

void SqliteLogSink::write(core::log::LogRecord const& rec) {
    if (int(rec.level) < int(m_minLevel)) return;
    insertRow(core::log::levelName(rec.level),
              rec.category,
              rec.source,
              withFields(rec.message, rec.fields),
              toMs(rec.ts));
}

void SqliteLogSink::write(core::log::OperationRecord const& rec) {
    // Audit trail: always recorded (operation records carry no severity).
    std::string msg = "actor=" + rec.actor + " target=" + rec.target
                      + " result=" + rec.result;
    if (!rec.note.empty()) msg += " note=" + rec.note;
    insertRow("Audit", rec.category, rec.action, msg, toMs(rec.ts));
}

void SqliteLogSink::pruneIfDue() {
    if ((m_inserts % 256) != 0) return;
    std::string sql =
        "DELETE FROM logs WHERE id <= (SELECT max(id) FROM logs) - "
        + std::to_string(m_maxRows) + ";";
    sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, nullptr);
}

void SqliteLogSink::flush() {
    // Each insert is its own auto-committed transaction; nothing buffered.
}

} // namespace core::gateway
