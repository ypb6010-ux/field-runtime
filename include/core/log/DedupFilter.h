// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <mutex>

#include <QDateTime>
#include <QHash>
#include <QString>

#include "core/core_global.h"

namespace core::log {

// Suppresses repeated emissions of the same event key.
//
// Intended to sit in front of a user/DB sink so a condition that keeps
// re-asserting (e.g. "database insert failed" every batch) does not flood the
// log: the first occurrence passes, repeats within `windowMs` are counted and
// suppressed, and the next occurrence after the window passes again carrying
// the suppressed count for an aggregated message ("repeated N times in 60s").
//
// Recommended key (see Core-日志与数据库模块-需求规格 §2.8.5):
//   <category>:<source>:<eventKey>
//
// Thread-safe: guards its own state, since emit and reset may come from
// different threads. Unique-key state is bounded (4096 entries by default);
// the oldest key is evicted when the cap is reached.
class CORE_EXPORT DedupFilter {
public:
    explicit DedupFilter(qint64 windowMs = 60'000, int maxEntries = 4096)
        : m_windowMs(std::max<qint64>(0, windowMs))
        , m_maxEntries(std::max(1, maxEntries)) {
    }

    // True if `key` should be emitted now: first time seen, or the window
    // since its last emit has elapsed. Repeats within the window return false
    // and bump the suppressed counter.
    bool accept(QString const& key, QDateTime const& now);
    bool accept(QString const& key) { return accept(key, QDateTime::currentDateTime()); }

    // Repeats suppressed for `key` since its last emit, then cleared. Call
    // right after accept() returns true to annotate an aggregated message.
    quint64 takeSuppressed(QString const& key);

    // Forget `key` so its next occurrence emits fresh (e.g. on recovery).
    void reset(QString const& key);
    void clear();

private:
    struct Entry {
        QDateTime lastEmit;
        quint64   suppressed = 0;
    };

    qint64                m_windowMs;
    int                   m_maxEntries;
    QHash<QString, Entry> m_entries;
    std::mutex            m_mtx;
};

} // namespace core::log
