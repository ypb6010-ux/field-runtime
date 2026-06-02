#include "core/log/Logger.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include <QHash>

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

    bool passesThreshold(LogRecord const& r) const {
        LogLevel min = globalMin.load(std::memory_order_relaxed);
        auto it = categoryMin.constFind(r.category);
        if (it != categoryMin.constEnd()) min = it.value();
        return r.level >= min;
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
                if (queue.empty()) {
                    drainedGen.fetch_add(1, std::memory_order_release);
                    drainCv.notify_all();
                }
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
        std::unique_lock lk(mtx);
        if (queue.empty()) return;
        quint64 target = drainedGen.load(std::memory_order_acquire) + 1;
        drainCv.wait(lk, [&] {
            return stopping
                || (queue.empty()
                    && drainedGen.load(std::memory_order_acquire) >= target);
        });
    }

    int                             cap;
    std::mutex                      mtx;
    std::condition_variable         cv;        // queue not empty
    std::condition_variable         spaceCv;   // queue has room
    std::condition_variable         drainCv;   // queue drained
    std::deque<Entry>               queue;
    bool                            stopping = false;
    std::atomic<quint64>            dropped{0};
    std::atomic<quint64>            drainedGen{0};

    std::atomic<LogLevel>           globalMin{LogLevel::Info};
    std::mutex                      sinkMtx;
    QHash<QString, LogLevel>        categoryMin;   // guarded by sinkMtx
    std::vector<std::shared_ptr<ILogSink>> sinks;  // guarded by sinkMtx

    std::thread                     worker;
};

Logger::Logger(int maxQueueDepth)
    : m_impl(std::make_unique<Impl>(maxQueueDepth)) {}

Logger::~Logger() = default;

void Logger::setThreshold(LogLevel min) {
    m_impl->globalMin.store(min, std::memory_order_relaxed);
}

void Logger::setCategoryThreshold(QString const& category, LogLevel min) {
    std::lock_guard lk(m_impl->sinkMtx);
    m_impl->categoryMin.insert(category, min);
}

LogLevel Logger::threshold() const {
    return m_impl->globalMin.load(std::memory_order_relaxed);
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
    if (rec.ts.isNull()) rec.ts = QDateTime::currentDateTime();
    {
        // Category threshold lives under sinkMtx; read it via the helper which
        // takes the lock only if a per-category override exists.
        std::lock_guard lk(m_impl->sinkMtx);
        LogLevel min = m_impl->globalMin.load(std::memory_order_relaxed);
        auto it = m_impl->categoryMin.constFind(rec.category);
        if (it != m_impl->categoryMin.constEnd()) min = it.value();
        if (rec.level < min) return;
    }
    m_impl->enqueue(Entry{std::move(rec)});
}

void Logger::logOperation(OperationRecord rec) {
    if (rec.ts.isNull()) rec.ts = QDateTime::currentDateTime();
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

} // namespace core::log
