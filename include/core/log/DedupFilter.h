// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

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
// different threads.
class CORE_EXPORT DedupFilter {
public:
    using Clock     = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    explicit DedupFilter(std::int64_t windowMs = 60'000)
        : m_windowMs(windowMs) {
    }

    // True if `key` should be emitted now: first time seen, or the window
    // since its last emit has elapsed. Repeats within the window return false
    // and bump the suppressed counter.
    bool accept(std::string const& key, TimePoint now);
    bool accept(std::string const& key) { return accept(key, Clock::now()); }

    // Repeats suppressed for `key` since its last emit, then cleared. Call
    // right after accept() returns true to annotate an aggregated message.
    std::uint64_t takeSuppressed(std::string const& key);

    // Forget `key` so its next occurrence emits fresh (e.g. on recovery).
    void reset(std::string const& key);
    void clear();

private:
    struct Entry {
        TimePoint     lastEmit;
        std::uint64_t suppressed = 0;
    };

    std::int64_t                           m_windowMs;
    std::unordered_map<std::string, Entry> m_entries;
    std::mutex                             m_mtx;
};

} // namespace core::log
