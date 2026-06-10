// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/persistence/Persistence.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <QxOrm.h>

#include "core/ICore.h"
#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/bus/Subscription.h"
#include "core/dp/Datapoint.h"
#include "core/dp/DatapointRegistry.h"
#include "core/log/ILogSink.h"
#include "core/log/Logger.h"

#include "models/Telemetry.h"
#include "models/OperationLog.h"
#include "models/SystemLog.h"

namespace core::persist {

namespace {
constexpr int kPageSize = 50;

quint64 parseStamp(QString const& s) {
    return QDateTime::fromString(s, QStringLiteral("yyyy-MM-dd hh:mm:ss"))
        .toMSecsSinceEpoch();
}
QString fmtStamp(quint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(qint64(ms))
        .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}
} // namespace

// ---------------------------------------------------------------------------
// DbLogSink — forwards Logger records into Persistence's queues. Kept private
// so the Logger registration does not leak the concrete Persistence type.
// ---------------------------------------------------------------------------
class Persistence::Impl;

class DbLogSink : public core::log::ILogSink {
public:
    explicit DbLogSink(Persistence::Impl& owner) : m_owner(owner) {}
    void write(core::log::LogRecord const& r) override;
    void write(core::log::OperationRecord const& r) override;
private:
    Persistence::Impl& m_owner;
};

class Persistence::Impl {
public:
    Impl(core::ICore& core, Config cfg) : m_core(core), m_cfg(std::move(cfg)) {}

    ~Impl() { stop(); }

    bool start() {
        if (!ensureDatabaseExists()) return false;
        if (!configureAndOpen())     return false;
        createTables();
        buildPersistTags();
        subscribe();
        installSink();
        m_running = true;
        m_worker  = std::thread([this] { run(); });
        m_core.logger().logf(core::log::LogLevel::Info,
            QStringLiteral("persistence"), QStringLiteral("db"),
            QStringLiteral("connected"),
            {{QStringLiteral("dbname"), m_cfg.dbname}});
        return true;
    }

    void stop() {
        if (m_sink) {
            m_core.logger().removeSink(m_sink.get());
            m_sink.reset();
        }
        m_dpSub.reset();
        {
            std::lock_guard lk(m_mtx);
            if (!m_running) return;
            m_running = false;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    // ── enqueue (called from bus / logger threads) ─────────────────────
    void pushTelemetry(Telemetry row) {
        std::lock_guard lk(m_mtx);
        if (!m_running) return;
        m_telemetry.push_back(std::move(row));
        maybeNotify();
    }
    void pushOperation(OperationLog row) {
        std::lock_guard lk(m_mtx);
        if (!m_running) return;
        m_ops.push_back(std::move(row));
        maybeNotify();
    }
    void pushSystem(SystemLog row) {
        std::lock_guard lk(m_mtx);
        if (!m_running) return;
        m_system.push_back(std::move(row));
        maybeNotify();
    }

    Config const& cfg() const { return m_cfg; }

    // ── queries (called from QML thread) ───────────────────────────────
    QJsonObject queryTelemetry(QString const& tag, QString const& start,
                               QString const& end, int page) {
        page = std::max(0, page);
        QList<std::shared_ptr<Telemetry>> list;
        qx::QxSqlQuery q;
        q.where("ts").isBetween(parseStamp(start), parseStamp(end));
        if (!tag.isEmpty()) q.and_("tag").isEqualTo(tag);
        long const count = qx::dao::count<Telemetry>(q);   // before order/limit
        q.orderAsc("ts");
        q.limit(kPageSize, page * kPageSize);
        if (auto e = qx::dao::fetch_by_query(q, list); e.isValid()) return {};
        return pack(list, count, page);
    }

    QJsonObject queryOperation(QString const& start, QString const& end, int page) {
        page = std::max(0, page);
        QList<std::shared_ptr<OperationLog>> list;
        qx::QxSqlQuery q;
        q.where("ts").isBetween(parseStamp(start), parseStamp(end));
        long const count = qx::dao::count<OperationLog>(q);   // before order/limit
        q.orderAsc("ts");
        q.limit(kPageSize, page * kPageSize);
        if (auto e = qx::dao::fetch_by_query(q, list); e.isValid()) return {};
        return pack(list, count, page);
    }

    QJsonObject querySystem(int minLevel, QString const& start,
                            QString const& end, int page) {
        page = std::max(0, page);
        QList<std::shared_ptr<SystemLog>> list;
        qx::QxSqlQuery q;
        q.where("ts").isBetween(parseStamp(start), parseStamp(end));
        if (minLevel > 0) q.and_("level").isGreaterThanOrEqualTo(minLevel);
        long const count = qx::dao::count<SystemLog>(q);   // before order/limit
        q.orderAsc("ts");
        q.limit(kPageSize, page * kPageSize);
        if (auto e = qx::dao::fetch_by_query(q, list); e.isValid()) return {};
        return pack(list, count, page);
    }

private:
    template <class T>
    QJsonObject pack(QList<std::shared_ptr<T>> const& list, long count, int page) {
        QJsonObject root;
        root["pages"] = int(std::ceil(double(count) / kPageSize));
        root["page"]  = page;
        root["limit"] = kPageSize;
        QJsonArray array;
        for (auto const& item : list) {
            QJsonParseError err;
            auto doc = QJsonDocument::fromJson(
                qx::serialization::json::to_byte_array(item), &err);
            if (err.error != QJsonParseError::NoError) continue;
            QJsonObject obj = doc.object();
            obj["ts_text"] = fmtStamp(item->ts);
            array.append(obj);
        }
        root["data"] = array;
        return root;
    }

    void maybeNotify() {
        int const depth = int(m_telemetry.size() + m_ops.size() + m_system.size());
        if (depth >= m_cfg.batchSize) m_cv.notify_one();
    }

    void buildPersistTags() {
        for (auto const& dp : m_core.datapoints().all()) {
            QString const tag = dp->persistTag();
            if (!tag.isEmpty()) m_persistTags.insert(dp->id(), tag);
        }
    }

    void subscribe() {
        m_dpSub = std::make_unique<core::bus::Subscription>(
            m_core.bus().subscribe<core::bus::DpChanged>(
                [this](core::bus::DpChanged const& e) {
                    auto it = m_persistTags.constFind(e.id);
                    if (it == m_persistTags.constEnd()) return;
                    Telemetry row;
                    row.tag   = it.value();
                    row.ts    = quint64(e.timestamp.isValid()
                                  ? e.timestamp.toMSecsSinceEpoch()
                                  : QDateTime::currentMSecsSinceEpoch());
                    row.value = e.value.toString();
                    pushTelemetry(std::move(row));
                }));
    }

    void installSink() {
        m_sink = std::make_shared<DbLogSink>(*this);
        m_core.logger().addSink(m_sink);
    }

    bool ensureDatabaseExists() {
        if (m_cfg.driver != QStringLiteral("QPSQL") || !m_cfg.autoCreateDatabase)
            return true;
        bool ok = true;
        {
            QSqlDatabase boot = QSqlDatabase::addDatabase(
                m_cfg.driver, QStringLiteral("core_persist_boot"));
            boot.setHostName(m_cfg.host);
            boot.setPort(m_cfg.port);
            boot.setUserName(m_cfg.user);
            boot.setPassword(m_cfg.password);
            boot.setDatabaseName(QStringLiteral("postgres"));
            if (!boot.open()) {
                logError(QStringLiteral("bootstrap connect failed: %1")
                             .arg(boot.lastError().text()));
                ok = false;
            } else {
                QSqlQuery q(boot);
                q.exec(QStringLiteral(
                    "SELECT 1 FROM pg_database WHERE datname='%1'").arg(m_cfg.dbname));
                if (!q.next()) {
                    QSqlQuery c(boot);
                    if (!c.exec(QStringLiteral("CREATE DATABASE \"%1\"").arg(m_cfg.dbname))) {
                        logError(QStringLiteral("CREATE DATABASE failed: %1")
                                     .arg(c.lastError().text()));
                        ok = false;
                    }
                }
                boot.close();
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("core_persist_boot"));
        return ok;
    }

    bool configureAndOpen() {
        auto* db = qx::QxSqlDatabase::getSingleton();
        db->setDriverName(m_cfg.driver);
        db->setHostName(m_cfg.host);
        db->setPort(m_cfg.port);
        db->setUserName(m_cfg.user);
        db->setPassword(m_cfg.password);
        db->setDatabaseName(m_cfg.dbname);
        db->setTraceSqlQuery(false);
        QSqlDatabase conn = db->getDatabase();
        if (!conn.isOpen() && !conn.open()) {
            logError(QStringLiteral("connect failed: %1").arg(conn.lastError().text()));
            return false;
        }
        return true;
    }

    // QxOrm's create_table<T>() only emits SQLite DDL (AUTOINCREMENT), so we
    // issue portable DDL ourselves with a driver-appropriate auto-id column.
    void createTables() {
        QString idType;
        if      (m_cfg.driver == QStringLiteral("QPSQL"))  idType = QStringLiteral("BIGSERIAL PRIMARY KEY");
        else if (m_cfg.driver == QStringLiteral("QMYSQL")) idType = QStringLiteral("BIGINT AUTO_INCREMENT PRIMARY KEY");
        else                                               idType = QStringLiteral("INTEGER PRIMARY KEY AUTOINCREMENT");

        QSqlDatabase conn = qx::QxSqlDatabase::getSingleton()->getDatabase();
        auto ddl = [&](QString const& sql) {
            QSqlQuery q(conn);
            if (!q.exec(sql))
                logError(QStringLiteral("DDL failed: %1").arg(q.lastError().text()));
        };
        ddl(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS telemetry ("
            "id %1, tag TEXT NOT NULL, ts BIGINT NOT NULL, value TEXT, quality INTEGER)")
            .arg(idType));
        ddl(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_telemetry_tag_ts ON telemetry(tag, ts)"));
        ddl(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS operation_log ("
            "id %1, ts BIGINT NOT NULL, actor TEXT, action TEXT, target TEXT, "
            "old_value TEXT, new_value TEXT, result TEXT, note TEXT)").arg(idType));
        ddl(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS system_log ("
            "id %1, ts BIGINT NOT NULL, level INTEGER, category TEXT, source TEXT, "
            "message TEXT)").arg(idType));
    }

    void run() {
        for (;;) {
            std::deque<Telemetry>    tb;
            std::deque<OperationLog> ob;
            std::deque<SystemLog>    sb;
            {
                std::unique_lock lk(m_mtx);
                m_cv.wait_for(lk, std::chrono::milliseconds(m_cfg.flushMs), [this] {
                    return !m_running
                        || !m_telemetry.empty() || !m_ops.empty() || !m_system.empty();
                });
                tb.swap(m_telemetry);
                ob.swap(m_ops);
                sb.swap(m_system);
                if (!m_running && tb.empty() && ob.empty() && sb.empty()) break;
            }
            flushBatch(tb, ob, sb);
        }
    }

    template <class T, class Q>
    void insertAll(Q& dq) {
        if (dq.empty()) return;
        QList<std::shared_ptr<T>> list;
        list.reserve(int(dq.size()));
        for (auto& row : dq) list.append(std::make_shared<T>(std::move(row)));
        if (auto e = qx::dao::insert(list); e.isValid()) {
            logError(QStringLiteral("insert failed: %1").arg(e.text()));
        }
    }

    void flushBatch(std::deque<Telemetry>& tb,
                    std::deque<OperationLog>& ob,
                    std::deque<SystemLog>& sb) {
        insertAll<Telemetry>(tb);
        insertAll<OperationLog>(ob);
        insertAll<SystemLog>(sb);
    }

    // Diagnostics about persistence itself go straight to the console-backed
    // logger (never recursively into the DB sink path).
    void logError(QString msg) {
        m_core.logger().logf(core::log::LogLevel::Error,
            QStringLiteral("persistence"), QStringLiteral("db"), std::move(msg));
    }

public:
    core::ICore&                                     m_core;
    Config                                           m_cfg;

private:
    QHash<QString, QString>                          m_persistTags;
    std::unique_ptr<core::bus::Subscription>         m_dpSub;
    std::shared_ptr<DbLogSink>                       m_sink;

    std::mutex                                       m_mtx;
    std::condition_variable                          m_cv;
    std::deque<Telemetry>                            m_telemetry;
    std::deque<OperationLog>                         m_ops;
    std::deque<SystemLog>                            m_system;
    bool                                             m_running = false;
    std::thread                                      m_worker;
};

// ── DbLogSink methods (need full Impl) ─────────────────────────────────
void DbLogSink::write(core::log::LogRecord const& r) {
    if (r.level < m_owner.m_cfg.systemLogMinLevel) return;
    // Break the feedback loop: persistence's own diagnostics (DB errors etc.)
    // must not be re-persisted, or one DB failure amplifies without bound.
    if (r.category == QStringLiteral("persistence")) return;
    SystemLog row;
    row.ts       = quint64(r.ts.isValid() ? r.ts.toMSecsSinceEpoch()
                                          : QDateTime::currentMSecsSinceEpoch());
    row.level    = int(r.level);
    row.category = r.category;
    row.source   = r.source;
    row.message  = r.message;
    m_owner.pushSystem(std::move(row));
}

void DbLogSink::write(core::log::OperationRecord const& r) {
    OperationLog row;
    row.ts        = quint64(r.ts.isValid() ? r.ts.toMSecsSinceEpoch()
                                           : QDateTime::currentMSecsSinceEpoch());
    row.actor     = r.actor;
    row.action    = r.action;
    row.target    = r.target;
    row.old_value = r.oldValue.toString();
    row.new_value = r.newValue.toString();
    row.result    = r.result;
    row.note      = r.note;
    m_owner.pushOperation(std::move(row));
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
Persistence::Persistence(core::ICore& core, Config cfg, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(core, std::move(cfg))) {}

Persistence::~Persistence() = default;

bool Persistence::start() { return m_impl->start(); }
void Persistence::stop()  { m_impl->stop(); }

QJsonObject Persistence::getTelemetry(QString const& tag, QString const& start,
                                      QString const& end, int page) {
    return m_impl->queryTelemetry(tag, start, end, page);
}
QJsonObject Persistence::getOperationLog(QString const& start, QString const& end,
                                         int page) {
    return m_impl->queryOperation(start, end, page);
}
QJsonObject Persistence::getSystemLog(int minLevel, QString const& start,
                                      QString const& end, int page) {
    return m_impl->querySystem(minLevel, start, end, page);
}

} // namespace core::persist
