// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/DedupFilter.h"

namespace core::log {

bool DedupFilter::accept(std::string const& key, TimePoint now) {
    std::lock_guard lk(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        m_entries.emplace(key, Entry{now, 0});
        return true;
    }
    auto const elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - it->second.lastEmit).count();
    if (elapsedMs >= m_windowMs) {
        it->second.lastEmit = now;
        return true;
    }
    ++it->second.suppressed;
    return false;
}

std::uint64_t DedupFilter::takeSuppressed(std::string const& key) {
    std::lock_guard lk(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return 0;
    }
    std::uint64_t const n = it->second.suppressed;
    it->second.suppressed = 0;
    return n;
}

void DedupFilter::reset(std::string const& key) {
    std::lock_guard lk(m_mtx);
    m_entries.erase(key);
}

void DedupFilter::clear() {
    std::lock_guard lk(m_mtx);
    m_entries.clear();
}

} // namespace core::log
