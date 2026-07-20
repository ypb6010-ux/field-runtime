// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <mutex>
#include <utility>

#include <QDateTime>

#include "core/bus/BusEvents.h"
#include "core/bus/EventBus.h"
#include "core/transport/TransportTypes.h"

namespace core::transport::detail {

// Shared, thread-safe implementation of TransportStatus semantics. Mutations
// happen under the mutex, while EventBus publication happens after unlocking
// so subscribers may query status() reentrantly without deadlocking.
class TransportStatusTracker {
public:
    TransportStatusTracker(QString id,
                           TransportKind kind,
                           bus::EventBus* bus,
                           EndpointInfo local = {},
                           EndpointInfo remote = {})
        : m_bus(bus) {
        m_status.transportId    = std::move(id);
        m_status.kind           = kind;
        m_status.localEndpoint  = std::move(local);
        m_status.remoteEndpoint = std::move(remote);
        m_status.changedAt      = QDateTime::currentDateTimeUtc();
    }

    TransportStatus snapshot() const {
        std::lock_guard lock(m_mutex);
        return m_status;
    }

    bool update(ConnectionState state, QString errorMessage = {}) {
        TransportStatus before;
        TransportStatus after;
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state == state
                && m_status.errorMessage == errorMessage) {
                return false;
            }
            before = m_status;
            m_status.state        = state;
            m_status.errorMessage = std::move(errorMessage);
            m_status.changedAt    = QDateTime::currentDateTimeUtc();
            ++m_status.revision;
            after = m_status;
        }
        if (m_bus) m_bus->publish(bus::TransportStateChanged{before, after});
        return true;
    }

private:
    bus::EventBus*          m_bus = nullptr;
    mutable std::mutex      m_mutex;
    TransportStatus        m_status;
};

} // namespace core::transport::detail
