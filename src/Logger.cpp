// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/Logger.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/dp/ValueQt.h"

namespace core::log {

const char* levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warn:     return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT";
    }
    return "INFO";
}

LogLevel levelFromString(std::string const& s) noexcept {
    auto const begin = s.find_first_not_of(" \t\r\n");
    auto const end   = s.find_last_not_of(" \t\r\n");
    std::string v = (begin == std::string::npos)
                  ? std::string{}
                  : s.substr(begin, end - begin + 1);
    for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (v == "trace")                       return LogLevel::Trace;
    if (v == "debug")                       return LogLevel::Debug;
    if (v == "info" || v == "log")          return LogLevel::Info;
    if (v == "warn" || v == "warning")      return LogLevel::Warn;
    if (v == "error")                       return LogLevel::Error;
    if (v == "crit" || v == "critical")     return LogLevel::Critical;
    return LogLevel::Info;
}

namespace {
using Entry = std::variant<LogRecord, OperationRecord>;

// Operation records and Warn+ system records are protected: never dropped
// under overload. Only Trace/Debug system records are droppable.
bool isDroppable(Entry const& e) {
    auto const* sys = std::get_if<LogRecord>(&e);
    return sys && sys->level < LogLevel::Info;
}
} // namespace

class Logger::Impl {
public:
    explicit Impl(int maxQueueDepth)
        : cap(maxQueueDepth > 1 ? maxQueueDepth : 1) {
        worker = std::thread([this] { run(); });
    }

    ~Impl() { stop(); }

    void stop() {
        {
            std::lock_guard lk(mtx);
            if (stopping) return;
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void enqueue(Entry e) {
        std::unique_lock lk(mtx);
        if (stopping) return;
        if (int(queue.size()) >= cap) {
            if (isDroppable(e)) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // Protected record: wait for the dispatch thread to make room
            // rather than dropping audit / error data.
            spaceCv.wait(lk, [this] {
                return stopping || int(queue.size()) < cap;
            });
            if (stopping) return;
        }
        queue.push_back(std::move(e));
        lk.unlock();
        cv.notify_one();
    }

    void run() {
        std::vector<std::shared_ptr<ILogSink>> localSinks;
        std::deque<Entry> batch;
        for (;;) {
            {
                std::unique_lock lk(mtx);
                cv.wait(lk, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    if (stopping) break;
                    continue;
                }
                batch.swap(queue);
                processing = true;   // batch is in flight (queue may now be empty)
            }
            spaceCv.notify_all();

            {
                std::lock_guard sk(sinkMtx);
                localSinks = sinks;
            }
            for (auto& entry : batch) {
                for (auto const& s : localSinks) {
                    std::visit([&](auto const& rec) { s->write(rec); }, entry);
                }
            }
            batch.clear();

            {
                std::lock_guard lk(mtx);
                processing = false;
                drainedGen.fetch_add(1, std::memory_order_release);
                drainCv.notify_all();
            }
        }
        // Final drain on shutdown so nothing queued is lost.
        std::deque<Entry> tail;
        {
            std::lock_guard lk(mtx);
            tail.swap(queue);
        }
        {
            std::lock_guard sk(sinkMtx);
            localSinks = sinks;
        }
        for (auto& entry : tail) {
            for (auto const& s : localSinks) {
                std::visit([&](auto const& rec) { s->write(rec); }, entry);
            }
        }
        for (auto const& s : localSinks) s->flush();
        drainedGen.fetch_add(1, std::memory_order_release);
        drainCv.notify_all();
    }

    void flush() {
        // Wait until nothing is queued AND no batch is mid-delivery. Checking
        // only `queue.empty()` would return early in the window after the
        // dispatch thread swaps the queue out but before it writes the batch
        // to the sinks (the flaky race).
        std::unique_lock lk(mtx);
        drainCv.wait(lk, [this] {
            return stopping || (queue.empty() && !processing);
        });
    }

    int                             cap;
    std::mutex                      mtx;
    std::condition_variable         cv;        // queue not empty
    std::condition_variable         spaceCv;   // queue has room
    std::condition_variable         drainCv;   // queue drained
    std::deque<Entry>               queue;
    bool                            stopping   = false;
    bool                            processing = false;   // a batch is mid-delivery
    std::atomic<quint64>            dropped{0};
    std::atomic<quint64>            drainedGen{0};

    mutable std::mutex              filterMtx;
    LogFilter                       coreFilter;    // gate, guarded by filterMtx
    std::mutex                      sinkMtx;
    std::vector<std::shared_ptr<ILogSink>> sinks;  // guarded by sinkMtx

    std::thread                     worker;
};

Logger::Logger(int maxQueueDepth)
    : m_impl(std::make_unique<Impl>(maxQueueDepth)) {}

Logger::~Logger() = default;

void Logger::setThreshold(LogLevel min) {
    std::lock_guard lk(m_impl->filterMtx);
    m_impl->coreFilter.setDefaultMinLevel(min);
}

void Logger::setCategoryThreshold(QString const& category, LogLevel min) {
    std::lock_guard lk(m_impl->filterMtx);
    m_impl->coreFilter.setCategory(category.toStdString(), true, min);
}

LogLevel Logger::threshold() const {
    std::lock_guard lk(m_impl->filterMtx);
    return m_impl->coreFilter.defaultRule().minLevel;
}

void Logger::setFilter(LogFilter filter) {
    std::lock_guard lk(m_impl->filterMtx);
    m_impl->coreFilter = std::move(filter);
}

LogFilter Logger::filter() const {
    std::lock_guard lk(m_impl->filterMtx);
    return m_impl->coreFilter;
}

void Logger::addSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) return;
    std::lock_guard lk(m_impl->sinkMtx);
    m_impl->sinks.push_back(std::move(sink));
}

void Logger::removeSink(ILogSink* sink) {
    std::lock_guard lk(m_impl->sinkMtx);
    std::erase_if(m_impl->sinks,
                  [sink](auto const& s) { return s.get() == sink; });
}

void Logger::log(LogRecord rec) {
    if (rec.ts == LogTime{}) rec.ts = std::chrono::system_clock::now();
    {
        std::lock_guard lk(m_impl->filterMtx);
        if (!m_impl->coreFilter.passes(rec)) return;
    }
    m_impl->enqueue(Entry{std::move(rec)});
}

void Logger::logOperation(OperationRecord rec) {
    if (rec.ts == LogTime{}) rec.ts = std::chrono::system_clock::now();
    if (rec.category.empty()) rec.category = "audit";
    {
        // Audit is gated on the category axis only (see LogFilter); a raised
        // level threshold never drops it, but disabling its category does.
        std::lock_guard lk(m_impl->filterMtx);
        if (!m_impl->coreFilter.passes(rec)) return;
    }
    m_impl->enqueue(Entry{std::move(rec)});
}

void Logger::logf(LogLevel level, QString category, QString source,
                  QString message, QVariantMap fields) {
    LogRecord rec;
    rec.ts       = std::chrono::system_clock::now();
    rec.level    = level;
    rec.category = category.toStdString();
    rec.source   = source.toStdString();
    rec.message  = message.toStdString();
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        rec.fields.emplace(it.key().toStdString(), dp::fromQVariant(it.value()));
    }
    log(std::move(rec));
}

void Logger::flush()             { m_impl->flush(); }
void Logger::stop()              { m_impl->stop(); }
quint64 Logger::droppedCount() const {
    return m_impl->dropped.load(std::memory_order_relaxed);
}

} // namespace core::log
