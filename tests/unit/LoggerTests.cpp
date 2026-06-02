#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

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
