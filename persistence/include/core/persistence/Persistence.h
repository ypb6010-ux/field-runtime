// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include "core/log/LogTypes.h"
#include "CorePersistence_global.h"

namespace core { class ICore; }

namespace core::persist {

struct Config {
    QString  driver   = QStringLiteral("QPSQL");   // QPSQL | QSQLITE | QMYSQL
    QString  host     = QStringLiteral("localhost");
    int      port     = 5432;
    QString  user     = QStringLiteral("postgres");
    QString  password;
    QString  dbname   = QStringLiteral("jmj_core");
    int      batchSize = 100;
    int      flushMs   = 500;
    int      maxQueuedRows = 100000;              // bounded producer memory
    core::log::LogLevel systemLogMinLevel = core::log::LogLevel::Info;
    bool     autoCreateDatabase = true;            // QPSQL: CREATE DATABASE if missing
};

// External persistence module for the new core. Stores three streams into SQL
// via QxOrm:
//   - telemetry      ← DpChanged (datapoints carrying a persistTag)
//   - operation_log  ← OperationRecord (registered as a Logger sink)
//   - system_log     ← LogRecord       (registered as a Logger sink)
//
// Core does not depend on this module; it consumes ICore's EventBus and Logger.
// Writes happen on a dedicated thread with batching so they never stall polling.
class COREPERSISTENCE_EXPORT Persistence : public QObject {
    Q_OBJECT
public:
    class Impl;   // public so the .cpp's private helper sink may name it

    Persistence(core::ICore& core, Config cfg, QObject* parent = nullptr);
    ~Persistence() override;

    // Connect (auto-creating the database for QPSQL), create tables, subscribe
    // to DpChanged, register as a Logger sink, and start the writer thread.
    bool start();
    void stop();

    // Paginated queries for QML (page 0-indexed). Timestamps are
    // "yyyy-MM-dd hh:mm:ss"; results carry { pages, page, limit, data:[...] }.
    Q_INVOKABLE QJsonObject getTelemetry(QString const& tag, QString const& start,
                                         QString const& end, int page = 0);
    Q_INVOKABLE QJsonObject getOperationLog(QString const& start, QString const& end,
                                            int page = 0);
    Q_INVOKABLE QJsonObject getSystemLog(int minLevel, QString const& start,
                                         QString const& end, int page = 0);

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::persist
