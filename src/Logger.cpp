// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/Logger.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

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

LogLevel levelFromString(QString const& s) noexcept {
    QString const v = s.trimmed().toLower();
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
        workerId = worker.get_id();
    }

    ~Impl() { stop(); }

    void writeToSink(std::shared_ptr<ILogSink> const& sink,
                     Entry const& entry) noexcept {
        try {
            std::visit([&](auto const& rec) { sink->write(rec); }, entry);
        } catch (...) {
            sinkFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void flushSink(std::shared_ptr<ILogSink> const& sink) noexcept {
        try {
            sink->flush();
        } catch (...) {
            sinkFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void stop() {
        bool notify = false;
        {
            std::lock_guard lk(mtx);
            if (!stopping) {
                stopping = true;
                notify = true;
            }
        }
        if (notify) {
            cv.notify_all();
            spaceCv.notify_all();
            drainCv.notify_all();
        }
        // A sink is invoked on this thread. Joining ourselves would deadlock;
        // the owning thread will perform the join on its later stop/destructor.
        if (worker.joinable() && std::this_thread::get_id() != workerId) {
            worker.join();
        }
    }

    void enqueue(Entry e) {
        // Do not feed records emitted by a sink back into the same logger.
        // Besides recursive amplification, a protected record could otherwise
        // block this sole consumer waiting for its own queue to make room.
        if (std::this_thread::get_id() == workerId) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
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
                    writeToSink(s, entry);
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
                writeToSink(s, entry);
            }
        }
        for (auto const& s : localSinks) flushSink(s);
        drainedGen.fetch_add(1, std::memory_order_release);
        drainCv.notify_all();
    }

    void flush() {
        if (std::this_thread::get_id() == workerId) return;
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
    std::atomic<quint64>            sinkFailures{0};
    std::atomic<quint64>            drainedGen{0};

    mutable std::mutex              filterMtx;
    LogFilter                       coreFilter;    // gate, guarded by filterMtx
    std::mutex                      sinkMtx;
    std::vector<std::shared_ptr<ILogSink>> sinks;  // guarded by sinkMtx

    std::thread                     worker;
    std::thread::id                 workerId;
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
    m_impl->coreFilter.setCategory(category, true, min);
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
    {
        std::lock_guard lk(m_impl->sinkMtx);
        std::erase_if(m_impl->sinks,
                      [sink](auto const& s) { return s.get() == sink; });
    }
    // The dispatch thread may already hold a snapshot containing this sink.
    // Wait for that batch to finish so callers can safely destroy a sink whose
    // implementation references their own lifetime (for example Persistence).
    m_impl->flush();
}

void Logger::log(LogRecord rec) {
    if (rec.ts.isNull()) rec.ts = QDateTime::currentDateTime();
    {
        std::lock_guard lk(m_impl->filterMtx);
        if (!m_impl->coreFilter.passes(rec)) return;
    }
    m_impl->enqueue(Entry{std::move(rec)});
}

void Logger::logOperation(OperationRecord rec) {
    if (rec.ts.isNull()) rec.ts = QDateTime::currentDateTime();
    if (rec.category.isEmpty()) rec.category = QStringLiteral("audit");
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
    rec.ts       = QDateTime::currentDateTime();
    rec.level    = level;
    rec.category = std::move(category);
    rec.source   = std::move(source);
    rec.message  = std::move(message);
    rec.fields   = std::move(fields);
    log(std::move(rec));
}

void Logger::flush()             { m_impl->flush(); }
void Logger::stop()              { m_impl->stop(); }
quint64 Logger::droppedCount() const {
    return m_impl->dropped.load(std::memory_order_relaxed);
}
quint64 Logger::sinkFailureCount() const {
    return m_impl->sinkFailures.load(std::memory_order_relaxed);
}

} // namespace core::log
