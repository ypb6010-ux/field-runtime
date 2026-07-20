// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "core/log/DedupFilter.h"
#include "core/log/LogFilter.h"
#include "core/log/Logger.h"
#include "core/log/Sinks.h"

using namespace core::log;

namespace {

// Thread-safe recording sink — the dispatch thread writes, the test thread
// reads after flush().
class RecordingSink : public ILogSink {
public:
    void write(LogRecord const& r) override {
        std::lock_guard lk(m_mtx);
        m_system.push_back(r);
    }
    void write(OperationRecord const& r) override {
        std::lock_guard lk(m_mtx);
        m_ops.push_back(r);
    }
    std::vector<LogRecord> system() {
        std::lock_guard lk(m_mtx);
        return m_system;
    }
    std::vector<OperationRecord> ops() {
        std::lock_guard lk(m_mtx);
        return m_ops;
    }
private:
    std::mutex                   m_mtx;
    std::vector<LogRecord>       m_system;
    std::vector<OperationRecord> m_ops;
};

class ThrowingSink : public ILogSink {
public:
    void write(LogRecord const&) override { throw std::runtime_error("write failed"); }
    void write(OperationRecord const&) override { throw std::runtime_error("write failed"); }
    void flush() override { throw std::runtime_error("flush failed"); }
};

class ReentrantSink : public ILogSink {
public:
    explicit ReentrantSink(Logger& logger) : m_logger(logger) {}
    void write(LogRecord const&) override {
        ++calls;
        m_logger.logf(LogLevel::Error, "sink", "reentrant", "must be rejected");
        m_logger.flush();
    }
    void write(OperationRecord const&) override {}
    std::atomic<int> calls{0};
private:
    Logger& m_logger;
};

} // namespace

TEST_CASE("Logger delivers system records to sinks", "[logger]") {
    Logger logger;
    auto sink = std::make_shared<RecordingSink>();
    logger.addSink(sink);

    logger.logf(LogLevel::Info, "transport", "PLC1", "connected");
    logger.logf(LogLevel::Error, "scheduler", "PLC1", "circuit open");
    logger.flush();

    auto recs = sink->system();
    REQUIRE(recs.size() == 2);
    REQUIRE(recs[0].category == "transport");
    REQUIRE(recs[0].message == "connected");
    REQUIRE(recs[1].level == LogLevel::Error);
}

TEST_CASE("Logger isolates a throwing sink and continues dispatch",
          "[logger][stability]") {
    Logger logger;
    auto bad = std::make_shared<ThrowingSink>();
    auto good = std::make_shared<RecordingSink>();
    logger.addSink(bad);
    logger.addSink(good);

    logger.logf(LogLevel::Error, "test", "throwing-sink", "still delivered");
    logger.flush();

    REQUIRE(good->system().size() == 1);
    REQUIRE(logger.sinkFailureCount() == 1);
}

TEST_CASE("Logger rejects reentrant sink emissions without deadlocking",
          "[logger][stability][reentrant]") {
    Logger logger;
    auto sink = std::make_shared<ReentrantSink>(logger);
    logger.addSink(sink);

    logger.logf(LogLevel::Info, "test", "reentrant-sink", "outer");
    logger.flush();

    REQUIRE(sink->calls.load() == 1);
    REQUIRE(logger.droppedCount() == 1);
}

TEST_CASE("Logger filters system records below threshold", "[logger]") {
    Logger logger;
    auto sink = std::make_shared<RecordingSink>();
    logger.addSink(sink);
    logger.setThreshold(LogLevel::Warn);

    logger.logf(LogLevel::Debug, "x", "s", "dropped");
    logger.logf(LogLevel::Info,  "x", "s", "dropped");
    logger.logf(LogLevel::Warn,  "x", "s", "kept");
    logger.logf(LogLevel::Error, "x", "s", "kept");
    logger.flush();

    auto recs = sink->system();
    REQUIRE(recs.size() == 2);
    REQUIRE(recs[0].message == "kept");
    REQUIRE(recs[1].message == "kept");
}

TEST_CASE("Per-category threshold overrides global", "[logger]") {
    Logger logger;
    auto sink = std::make_shared<RecordingSink>();
    logger.addSink(sink);
    logger.setThreshold(LogLevel::Error);
    logger.setCategoryThreshold("transport", LogLevel::Debug);

    logger.logf(LogLevel::Debug, "transport", "s", "kept");
    logger.logf(LogLevel::Debug, "scheduler", "s", "dropped");
    logger.flush();

    auto recs = sink->system();
    REQUIRE(recs.size() == 1);
    REQUIRE(recs[0].category == "transport");
}

TEST_CASE("Operation records are never filtered by level", "[logger]") {
    Logger logger;
    auto sink = std::make_shared<RecordingSink>();
    logger.addSink(sink);
    logger.setThreshold(LogLevel::Critical);

    OperationRecord op;
    op.actor  = "ui:user";
    op.action = "reset";
    op.target = "belt2";
    op.result = "ok";
    logger.logOperation(op);
    logger.flush();

    auto ops = sink->ops();
    REQUIRE(ops.size() == 1);
    REQUIRE(ops[0].action == "reset");
    REQUIRE(ops[0].actor == "ui:user");
}

TEST_CASE("LogFilter gates on category and level independently", "[logfilter]") {
    LogFilter f;
    f.setDefault(true, LogLevel::Info);
    f.setCategory("switch", false, LogLevel::Trace);   // disabled at any level
    f.setCategory("config", true, LogLevel::Error);    // enabled but Error+ only

    REQUIRE(f.passes("transport", LogLevel::Info));
    REQUIRE_FALSE(f.passes("transport", LogLevel::Debug));  // below default floor
    REQUIRE_FALSE(f.passes("switch", LogLevel::Error));     // category disabled
    REQUIRE_FALSE(f.passes("config", LogLevel::Info));      // below category floor
    REQUIRE(f.passes("config", LogLevel::Error));
}

TEST_CASE("LogFilter gates operation records on category axis only", "[logfilter]") {
    LogFilter f;
    f.setDefault(true, LogLevel::Critical);   // very high level floor
    OperationRecord op;
    op.category = "audit";
    REQUIRE(f.passes(op));                     // level ignored for ops
    f.setCategory("audit", false, LogLevel::Trace);
    REQUIRE_FALSE(f.passes(op));               // disabling category hides it
}

TEST_CASE("LogFilter inherit then override is independent of base", "[logfilter]") {
    LogFilter base;
    base.setDefault(true, LogLevel::Info);
    base.setCategory("switch", true, LogLevel::Info);

    LogFilter biz = LogFilter::inherit(base);
    REQUIRE(biz.passes("switch", LogLevel::Info));      // inherited
    biz.setCategory("switch", false, LogLevel::Info);   // override: hide switch
    REQUIRE_FALSE(biz.passes("switch", LogLevel::Info));
    REQUIRE(base.passes("switch", LogLevel::Info));     // base untouched
}

TEST_CASE("Logger setFilter replaces the gate", "[logger]") {
    Logger logger;
    auto sink = std::make_shared<RecordingSink>();
    logger.addSink(sink);

    LogFilter f;
    f.setDefault(false, LogLevel::Trace);     // block everything by default
    f.setCategory("alarm", true, LogLevel::Info);
    logger.setFilter(f);

    logger.logf(LogLevel::Error, "transport", "s", "blocked");
    logger.logf(LogLevel::Info,  "alarm", "s", "kept");
    logger.flush();

    auto recs = sink->system();
    REQUIRE(recs.size() == 1);
    REQUIRE(recs[0].category == "alarm");
}

TEST_CASE("Logger gates operation records by category", "[logger]") {
    Logger logger;
    auto sink = std::make_shared<RecordingSink>();
    logger.addSink(sink);

    LogFilter f;   // default enabled; explicitly hide audit
    f.setCategory("audit", false, LogLevel::Trace);
    logger.setFilter(f);

    OperationRecord op;
    op.action   = "server-write";
    op.category = "audit";
    logger.logOperation(op);
    logger.flush();

    REQUIRE(sink->ops().empty());
}

TEST_CASE("DedupFilter suppresses repeats within window", "[dedup]") {
    DedupFilter d(1000);   // 1s window
    QDateTime const t0(QDate(2026, 6, 10), QTime(0, 0, 0));

    REQUIRE(d.accept("k", t0));                      // first occurrence emits
    REQUIRE_FALSE(d.accept("k", t0.addMSecs(200)));  // within window: suppressed
    REQUIRE_FALSE(d.accept("k", t0.addMSecs(900)));
    REQUIRE(d.takeSuppressed("k") == 2);
    REQUIRE(d.accept("k", t0.addMSecs(1100)));       // window elapsed: emits again

    d.reset("k");
    REQUIRE(d.accept("k", t0.addMSecs(1200)));       // reset: emits fresh
}

TEST_CASE("DedupFilter bounds unique-key memory by evicting the oldest entry",
          "[dedup][bounded]") {
    DedupFilter d(10'000, 2);
    QDateTime const t0(QDate(2026, 6, 10), QTime(0, 0, 0));
    REQUIRE(d.accept("oldest", t0));
    REQUIRE(d.accept("newer", t0.addMSecs(1)));
    REQUIRE(d.accept("newest", t0.addMSecs(2)));

    // `oldest` was evicted to preserve the cap, so it emits fresh even though
    // its original dedup window would still be active.
    REQUIRE(d.accept("oldest", t0.addMSecs(3)));
}

TEST_CASE("formatLine renders a system record", "[logger]") {
    LogRecord r;
    r.ts       = QDateTime(QDate(2026, 5, 29), QTime(14, 23, 45, 678));
    r.level    = LogLevel::Warn;
    r.category = "transport";
    r.source   = "PLC1";
    r.message  = "reconnect";
    QString line = formatLine(r);
    REQUIRE(line.contains("2026-05-29 14:23:45.678"));
    REQUIRE(line.contains("[Core/Transport]"));
    REQUIRE(line.contains("PLC1 reconnect"));
}
