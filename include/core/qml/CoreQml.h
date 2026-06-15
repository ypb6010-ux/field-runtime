// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "core/core_global.h"

class QQmlContext;

namespace core { class ICore; }

namespace core::qml {

// Qt convenience: build an ICore and expose the `log` bridge on `ctx`
// (`Text { text: log... }`, `log.info(...)`). The bridge is owned by the Core
// instance, so it is torn down with the Core (before the logger) — keep the
// returned ICore alive as long as `ctx` is used.
//
// This lives in core/qml (the Qt adapter) so the Qt-free ICore.h carries no
// QQmlContext.
CORE_EXPORT std::unique_ptr<ICore> createWithQml(QQmlContext* ctx,
                                                 bool installDefaultConsole = true);

} // namespace core::qml
