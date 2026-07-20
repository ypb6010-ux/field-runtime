// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "core/log/DedupFilter.h"

#include <iterator>

namespace core::log {

bool DedupFilter::accept(QString const& key, QDateTime const& now) {
    std::lock_guard lk(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        if (m_entries.size() >= m_maxEntries) {
            auto oldest = m_entries.begin();
            for (auto candidate = std::next(oldest);
                 candidate != m_entries.end(); ++candidate) {
                if (candidate->lastEmit < oldest->lastEmit) oldest = candidate;
            }
            m_entries.erase(oldest);
        }
        m_entries.insert(key, Entry{now, 0});
        return true;
    }
    if (it->lastEmit.msecsTo(now) >= m_windowMs) {
        it->lastEmit = now;
        return true;
    }
    ++it->suppressed;
    return false;
}

quint64 DedupFilter::takeSuppressed(QString const& key) {
    std::lock_guard lk(m_mtx);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return 0;
    }
    quint64 const n = it->suppressed;
    it->suppressed = 0;
    return n;
}

void DedupFilter::reset(QString const& key) {
    std::lock_guard lk(m_mtx);
    m_entries.remove(key);
}

void DedupFilter::clear() {
    std::lock_guard lk(m_mtx);
    m_entries.clear();
}

} // namespace core::log
