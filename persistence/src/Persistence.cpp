// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/persistence/Persistence.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

std::optional<quint64> parseStamp(QString const& s) {
    auto const dt = QDateTime::fromString(
        s, QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    if (!dt.isValid()) return std::nullopt;
    auto const ms = dt.toMSecsSinceEpoch();
    if (ms < 0) return std::nullopt;
    return quint64(ms);
}
QString fmtStamp(quint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(qint64(ms))
        .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}

QJsonObject errorResult(QString message) {
    return {{QStringLiteral("error"), std::move(message)}};
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
    class CallbackGate {
    public:
        explicit CallbackGate(Impl* owner) : m_owner(owner) {}

        template <class Fn>
        void invoke(Fn&& fn) {
            Impl* owner = nullptr;
            {
                std::lock_guard lk(m_mtx);
                if (!m_accepting || !m_owner) return;
                ++m_active;
                owner = m_owner;
            }
            struct Exit {
                CallbackGate* gate;
                ~Exit() { gate->leave(); }
            } exit{this};
            fn(*owner);
        }

        void open(Impl* owner) {
            std::lock_guard lk(m_mtx);
            m_owner = owner;
            m_accepting = true;
        }

        void closeAndDrain() {
            std::unique_lock lk(m_mtx);
            m_accepting = false;
            m_cv.wait(lk, [this] { return m_active == 0; });
            m_owner = nullptr;
        }

    private:
        void leave() {
            std::lock_guard lk(m_mtx);
            if (--m_active == 0) m_cv.notify_all();
        }

        std::mutex              m_mtx;
        std::condition_variable m_cv;
        Impl*                   m_owner = nullptr;
        int                     m_active = 0;
        bool                    m_accepting = true;
    };

    Impl(core::ICore& core, Config cfg)
        : m_core(core)
        , m_cfg(std::move(cfg))
        , m_callbackGate(std::make_shared<CallbackGate>(this)) {}

    ~Impl() { stop(); }

    bool start() {
        if (m_running) return true;
        if (m_cfg.batchSize <= 0 || m_cfg.flushMs <= 0
            || m_cfg.maxQueuedRows <= 0) {
            logError(QStringLiteral(
                "batchSize, flushMs and maxQueuedRows must be > 0"));
            return false;
        }
        if (m_cfg.dbname.trimmed().isEmpty()) {
            logError(QStringLiteral("dbname must not be empty"));
            return false;
        }
        if (m_cfg.driver != QStringLiteral("QPSQL")
            && m_cfg.driver != QStringLiteral("QMYSQL")
            && m_cfg.driver != QStringLiteral("QSQLITE")) {
            logError(QStringLiteral("unsupported SQL driver: %1").arg(m_cfg.driver));
            return false;
        }
        if (m_cfg.driver != QStringLiteral("QSQLITE")
            && (m_cfg.port < 1 || m_cfg.port > 65535)) {
            logError(QStringLiteral("database port must be in [1, 65535]"));
            return false;
        }
        if (!ensureDatabaseExists()) return false;
        if (!configureAndOpen())     return false;
        if (!createTables()) return false;
        buildPersistTags();
        {
            std::lock_guard lk(m_mtx);
            m_running = true;
        }
        m_callbackGate->open(this);
        subscribe();
        installSink();
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
        m_callbackGate->closeAndDrain();
        m_dpSub.reset();
        m_dpStateSub.reset();
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
        if (!makeRoomForTelemetry()) return;
        m_telemetry.push_back(std::move(row));
        maybeNotify();
    }
    void pushOperation(OperationLog row) {
        std::lock_guard lk(m_mtx);
        if (!m_running) return;
        makeRoomForOperation();
        m_ops.push_back(std::move(row));
        maybeNotify();
    }
    void pushSystem(SystemLog row) {
        std::lock_guard lk(m_mtx);
        if (!m_running) return;
        if (!makeRoomForSystem()) return;
        m_system.push_back(std::move(row));
        maybeNotify();
    }

    Config const& cfg() const { return m_cfg; }

    // ── queries (called from QML thread) ───────────────────────────────
    QJsonObject queryTelemetry(QString const& tag, QString const& start,
                               QString const& end, int page) {
        auto const startMs = parseStamp(start);
        auto const endMs   = parseStamp(end);
        if (!startMs || !endMs || *startMs > *endMs) {
            return errorResult(QStringLiteral(
                "invalid time range; expected yyyy-MM-dd hh:mm:ss and start <= end"));
        }
        page = std::clamp(page, 0, std::numeric_limits<int>::max() / kPageSize);
        QList<std::shared_ptr<Telemetry>> list;
        qx::QxSqlQuery q;
        q.where("ts").isBetween(*startMs, *endMs);
        if (!tag.isEmpty()) q.and_("tag").isEqualTo(tag);
        long const count = qx::dao::count<Telemetry>(q);   // before order/limit
        q.orderAsc("ts");
        q.limit(kPageSize, page * kPageSize);
        if (auto e = qx::dao::fetch_by_query(q, list); e.isValid())
            return errorResult(e.text());
        return pack(list, count, page);
    }

    QJsonObject queryOperation(QString const& start, QString const& end, int page) {
        auto const startMs = parseStamp(start);
        auto const endMs   = parseStamp(end);
        if (!startMs || !endMs || *startMs > *endMs) {
            return errorResult(QStringLiteral(
                "invalid time range; expected yyyy-MM-dd hh:mm:ss and start <= end"));
        }
        page = std::clamp(page, 0, std::numeric_limits<int>::max() / kPageSize);
        QList<std::shared_ptr<OperationLog>> list;
        qx::QxSqlQuery q;
        q.where("ts").isBetween(*startMs, *endMs);
        long const count = qx::dao::count<OperationLog>(q);   // before order/limit
        q.orderAsc("ts");
        q.limit(kPageSize, page * kPageSize);
        if (auto e = qx::dao::fetch_by_query(q, list); e.isValid())
            return errorResult(e.text());
        return pack(list, count, page);
    }

    QJsonObject querySystem(int minLevel, QString const& start,
                            QString const& end, int page) {
        auto const startMs = parseStamp(start);
        auto const endMs   = parseStamp(end);
        if (!startMs || !endMs || *startMs > *endMs) {
            return errorResult(QStringLiteral(
                "invalid time range; expected yyyy-MM-dd hh:mm:ss and start <= end"));
        }
        page = std::clamp(page, 0, std::numeric_limits<int>::max() / kPageSize);
        QList<std::shared_ptr<SystemLog>> list;
        qx::QxSqlQuery q;
        q.where("ts").isBetween(*startMs, *endMs);
        if (minLevel > 0) q.and_("level").isGreaterThanOrEqualTo(minLevel);
        long const count = qx::dao::count<SystemLog>(q);   // before order/limit
        q.orderAsc("ts");
        q.limit(kPageSize, page * kPageSize);
        if (auto e = qx::dao::fetch_by_query(q, list); e.isValid())
            return errorResult(e.text());
        return pack(list, count, page);
    }

private:
    std::size_t queueDepthLocked() const {
        return m_telemetry.size() + m_ops.size() + m_system.size();
    }

    bool makeRoomForTelemetry() {
        if (queueDepthLocked() < std::size_t(m_cfg.maxQueuedRows)) return true;
        ++m_droppedRows;
        if (m_telemetry.empty()) return false;
        m_telemetry.pop_front();
        return true;
    }

    bool makeRoomForSystem() {
        if (queueDepthLocked() < std::size_t(m_cfg.maxQueuedRows)) return true;
        ++m_droppedRows;
        if (!m_telemetry.empty()) {
            m_telemetry.pop_front();
            return true;
        }
        if (m_system.empty()) return false; // preserve operation audit rows
        m_system.pop_front();
        return true;
    }

    void makeRoomForOperation() {
        if (queueDepthLocked() < std::size_t(m_cfg.maxQueuedRows)) return;
        ++m_droppedRows;
        if (!m_telemetry.empty())      m_telemetry.pop_front();
        else if (!m_system.empty())    m_system.pop_front();
        else if (!m_ops.empty())       m_ops.pop_front();
    }

    template <class T>
    QJsonObject pack(QList<std::shared_ptr<T>> const& list, long count, int page) {
        QJsonObject root;
        count = std::max(0L, count);
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
        auto const gate = m_callbackGate;
        m_dpSub = std::make_unique<core::bus::Subscription>(
            m_core.bus().subscribe<core::bus::DpChanged>(
                [gate](core::bus::DpChanged const& e) {
                    gate->invoke([&](Impl& owner) {
                        auto it = owner.m_persistTags.constFind(e.id);
                        if (it == owner.m_persistTags.constEnd()) return;
                        Telemetry row;
                        row.tag   = it.value();
                        row.ts    = quint64(e.timestamp.isValid()
                                      ? e.timestamp.toMSecsSinceEpoch()
                                      : QDateTime::currentMSecsSinceEpoch());
                        row.value = e.value.toString();
                        owner.pushTelemetry(std::move(row));
                    });
                }));
        m_dpStateSub = std::make_unique<core::bus::Subscription>(
            m_core.bus().subscribe<core::bus::DpStateChanged>(
                [gate](core::bus::DpStateChanged const& e) {
                    gate->invoke([&](Impl& owner) {
                        auto it = owner.m_persistTags.constFind(e.id);
                        if (it == owner.m_persistTags.constEnd()) return;
                        Telemetry row;
                        row.tag     = it.value();
                        row.ts      = quint64(e.timestamp.isValid()
                                          ? e.timestamp.toMSecsSinceEpoch()
                                          : QDateTime::currentMSecsSinceEpoch());
                        row.value   = e.value.toString();
                        row.quality = e.state;
                        owner.pushTelemetry(std::move(row));
                    });
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
        QString const connectionName = QStringLiteral("core_persist_boot_%1")
            .arg(quintptr(this), 0, 16);
        {
            QSqlDatabase boot = QSqlDatabase::addDatabase(
                m_cfg.driver, connectionName);
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
                q.prepare(QStringLiteral(
                    "SELECT 1 FROM pg_database WHERE datname = :dbname"));
                q.bindValue(QStringLiteral(":dbname"), m_cfg.dbname);
                if (!q.exec()) {
                    logError(QStringLiteral("database lookup failed: %1")
                                 .arg(q.lastError().text()));
                    ok = false;
                } else if (!q.next()) {
                    QString quotedName = m_cfg.dbname;
                    quotedName.replace('"', QStringLiteral("\"\""));
                    QSqlQuery c(boot);
                    if (!c.exec(QStringLiteral("CREATE DATABASE \"%1\"")
                                    .arg(quotedName))) {
                        logError(QStringLiteral("CREATE DATABASE failed: %1")
                                     .arg(c.lastError().text()));
                        ok = false;
                    }
                }
                boot.close();
            }
        }
        QSqlDatabase::removeDatabase(connectionName);
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
    bool createTables() {
        QString idType;
        if      (m_cfg.driver == QStringLiteral("QPSQL"))  idType = QStringLiteral("BIGSERIAL PRIMARY KEY");
        else if (m_cfg.driver == QStringLiteral("QMYSQL")) idType = QStringLiteral("BIGINT AUTO_INCREMENT PRIMARY KEY");
        else                                               idType = QStringLiteral("INTEGER PRIMARY KEY AUTOINCREMENT");

        QSqlDatabase conn = qx::QxSqlDatabase::getSingleton()->getDatabase();
        bool ok = true;
        auto ddl = [&](QString const& sql) {
            QSqlQuery q(conn);
            if (!q.exec(sql)) {
                logError(QStringLiteral("DDL failed: %1").arg(q.lastError().text()));
                ok = false;
            }
        };
        ddl(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS telemetry ("
            "id %1, tag VARCHAR(255) NOT NULL, ts BIGINT NOT NULL, "
            "value TEXT, quality INTEGER)")
            .arg(idType));
        if (m_cfg.driver == QStringLiteral("QMYSQL")) {
            QSqlQuery exists(conn);
            exists.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM information_schema.statistics "
                "WHERE table_schema = DATABASE() AND table_name = 'telemetry' "
                "AND index_name = 'idx_telemetry_tag_ts'"));
            if (!exists.exec() || !exists.next()) {
                logError(QStringLiteral("index lookup failed: %1")
                             .arg(exists.lastError().text()));
                ok = false;
            } else if (exists.value(0).toInt() == 0) {
                ddl(QStringLiteral(
                    "CREATE INDEX idx_telemetry_tag_ts ON telemetry(tag, ts)"));
            }
        } else {
            ddl(QStringLiteral(
                "CREATE INDEX IF NOT EXISTS idx_telemetry_tag_ts "
                "ON telemetry(tag, ts)"));
        }
        ddl(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS operation_log ("
            "id %1, ts BIGINT NOT NULL, actor TEXT, action TEXT, target TEXT, "
            "old_value TEXT, new_value TEXT, result TEXT, note TEXT)").arg(idType));
        ddl(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS system_log ("
            "id %1, ts BIGINT NOT NULL, level INTEGER, category TEXT, source TEXT, "
            "message TEXT)").arg(idType));
        return ok;
    }

    void run() {
        std::deque<Telemetry>    tb;
        std::deque<OperationLog> ob;
        std::deque<SystemLog>    sb;
        for (;;) {
            qsizetype dropped = 0;
            if (tb.empty() && ob.empty() && sb.empty()) {
                std::unique_lock lk(m_mtx);
                // Flush when a full batch is ready, or after flushMs so a
                // low-rate stream is not held indefinitely. Waking merely
                // because one row existed made every row its own DB batch.
                m_cv.wait_for(lk, std::chrono::milliseconds(m_cfg.flushMs), [this] {
                    auto const depth = m_telemetry.size()
                                     + m_ops.size()
                                     + m_system.size();
                    return !m_running || depth >= std::size_t(m_cfg.batchSize);
                });
                if (m_running && m_telemetry.empty()
                              && m_ops.empty()
                              && m_system.empty()) {
                    continue;
                }
                tb.swap(m_telemetry);
                ob.swap(m_ops);
                sb.swap(m_system);
                dropped = m_droppedRows;
                m_droppedRows = 0;
                if (!m_running && tb.empty() && ob.empty() && sb.empty()) break;
            }
            if (dropped > 0) {
                logError(QStringLiteral(
                    "persistence queue full; dropped %1 oldest/lower-priority rows")
                    .arg(dropped));
            }
            if (flushBatch(tb, ob, sb)) continue;

            // Retain the failed local batch and retry it after the configured
            // interval. Producers continue into the bounded main queue. During
            // shutdown, do not hang forever on an unavailable database.
            std::unique_lock lk(m_mtx);
            if (!m_running) {
                auto const lost = tb.size() + ob.size() + sb.size();
                lk.unlock();
                logError(QStringLiteral(
                    "final persistence flush failed; %1 rows were not stored")
                    .arg(lost));
                break;
            }
            m_cv.wait_for(lk, std::chrono::milliseconds(m_cfg.flushMs),
                          [this] { return !m_running; });
        }
    }

    template <class T, class Q>
    bool insertAll(Q& dq) {
        if (dq.empty()) return true;
        QList<std::shared_ptr<T>> list;
        list.reserve(int(dq.size()));
        for (auto const& row : dq) list.append(std::make_shared<T>(row));
        if (auto e = qx::dao::insert(list); e.isValid()) {
            logError(QStringLiteral("insert failed: %1").arg(e.text()));
            return false;
        }
        dq.clear();
        return true;
    }

    bool flushBatch(std::deque<Telemetry>& tb,
                    std::deque<OperationLog>& ob,
                    std::deque<SystemLog>& sb) {
        bool const telemetryOk = insertAll<Telemetry>(tb);
        bool const operationOk = insertAll<OperationLog>(ob);
        bool const systemOk    = insertAll<SystemLog>(sb);
        return telemetryOk && operationOk && systemOk;
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
    std::unique_ptr<core::bus::Subscription>         m_dpStateSub;
    std::shared_ptr<DbLogSink>                       m_sink;
    std::shared_ptr<CallbackGate>                    m_callbackGate;

    std::mutex                                       m_mtx;
    std::condition_variable                          m_cv;
    std::deque<Telemetry>                            m_telemetry;
    std::deque<OperationLog>                         m_ops;
    std::deque<SystemLog>                            m_system;
    qsizetype                                        m_droppedRows = 0;
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
