// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <utility>

#include <QMetaObject>
#include <QObject>
#include <QThread>

namespace core::transport::detail {

// BlockingQueuedConnection deadlocks when called from the receiver's thread.
// Execute inline in that case and retain the synchronous hop otherwise.
template <typename Fn>
bool invokeBlocking(QObject* context, Fn&& fn) {
    if (!context) return false;
    if (QThread::currentThread() == context->thread()) {
        std::forward<Fn>(fn)();
        return true;
    }
    return QMetaObject::invokeMethod(
        context, std::forward<Fn>(fn), Qt::BlockingQueuedConnection);
}

} // namespace core::transport::detail
