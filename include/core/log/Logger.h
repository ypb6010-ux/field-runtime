#pragma once

#include <memory>
#include <QString>

#include "core/core_global.h"
#include "core/log/ILogSink.h"
#include "core/log/LogTypes.h"

namespace core::log {

// Built-in async logging facade. Owned by ICore; one per application.
//
// Emit side (log / logOperation / convenience helpers) is PUBLIC, thread-safe,
// non-blocking, and write-only — any thread (transport worker, scheduler,
// plugin, UI bridge) may call it. Records are pushed onto a bounded queue and
// fanned out to registered sinks on a single dedicated dispatch thread, so a
// slow sink never stalls the caller.
//
// Configuration side (threshold / addSink / removeSink) belongs to the owner.
// That asymmetry — public emit, owned configuration — is what keeps logging
// from becoming a back-door into core internals.
class CORE_EXPORT Logger {
public:
    explicit Logger(int maxQueueDepth = 8192);
    ~Logger();   // flushes the queue and joins the dispatch thread

    CORE_DISABLE_COPY_MOVE(Logger)

    // ── configuration (owner only) ─────────────────────────────────────
    void setThreshold(LogLevel min);                          // global
    void setCategoryThreshold(QString const& category, LogLevel min);
    LogLevel threshold() const;

    void addSink(std::shared_ptr<ILogSink> sink);
    void removeSink(ILogSink* sink);

    // ── emit (public, write-only, value types only) ────────────────────
    void log(LogRecord rec);            // system / diagnostic
    void logOperation(OperationRecord rec); // run / audit (never filtered)

    void logf(LogLevel    level,
              QString      category,
              QString      source,
              QString      message,
              QVariantMap  fields = {});

    // Block until every record queued so far has been handed to all sinks.
    void flush();
    // Flush + stop the dispatch thread (idempotent; also run by ~Logger).
    void stop();

    // System records below threshold dropped at the gate are not counted;
    // this counts records dropped under queue overload (Trace/Debug only —
    // Warn+ and all operation records are never dropped).
    quint64 droppedCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core::log
