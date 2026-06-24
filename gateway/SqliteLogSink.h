// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include "core/log/ILogSink.h"
#include "core/log/LogTypes.h"

struct sqlite3;
struct sqlite3_stmt;

namespace core::gateway {

// Persists log records into the `logs` table of the gateway's sqlite database.
//
// Thread model: the Logger fans every record out on its single dispatch thread,
// so write()/flush() are never reentrant. This sink therefore owns its OWN
// sqlite connection (separate from Persistence, which is used on the io thread)
// and opens it lazily on the first write — i.e. on the dispatch thread that will
// use it. That keeps one connection per thread and sidesteps cross-thread reuse;
// WAL + busy_timeout absorb the brief writer overlap with the telemetry path.
class SqliteLogSink : public core::log::ILogSink {
public:
    SqliteLogSink(std::string dbPath,
                  core::log::LogLevel minLevel = core::log::LogLevel::Info,
                  int maxRows = 50000);
    ~SqliteLogSink() override;

    SqliteLogSink(SqliteLogSink const&) = delete;
    SqliteLogSink& operator=(SqliteLogSink const&) = delete;

    void write(core::log::LogRecord const& rec) override;
    void write(core::log::OperationRecord const& rec) override;
    void flush() override;

private:
    bool ensureOpen();
    void insertRow(char const* level,
                   std::string const& module,
                   std::string const& key,
                   std::string const& msg,
                   std::int64_t tsMs);
    void pruneIfDue();

    std::string m_path;
    core::log::LogLevel m_minLevel;
    int m_maxRows;
    sqlite3* m_db = nullptr;
    sqlite3_stmt* m_insert = nullptr;
    bool m_failed = false;
    std::uint64_t m_inserts = 0;
};

} // namespace core::gateway
