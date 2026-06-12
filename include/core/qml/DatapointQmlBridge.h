// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <string>

#include <QObject>
#include <QString>

#include "core/core_global.h"

namespace core::dp  { class DatapointRegistry; }
namespace core::bus { class EventBus; }

namespace core::qml {

class QtDatapoint;

// Thin QObject that lets QML look up datapoints by id. The returned QtDatapoint
// is a QObject with a `value` Q_PROPERTY + NOTIFY, so QML binds to its `value`
// directly and updates automatically when it changes. Wrappers are created on
// demand and cached (owned as children of the bridge).
//
// Usage:
//   Text   { text: bridge.dp("belt2.run_state").value }
//   Button { onClicked: bridge.dp("belt2.cmd.start").write(true) }
class CORE_EXPORT DatapointQmlBridge : public QObject {
    Q_OBJECT
public:
    DatapointQmlBridge(dp::DatapointRegistry& registry,
                       bus::EventBus& bus,
                       QObject* parent = nullptr);
    ~DatapointQmlBridge() override;

    Q_INVOKABLE QtDatapoint* dp(QString const& id);

private:
    dp::DatapointRegistry&              m_registry;
    bus::EventBus&                      m_bus;
    std::map<std::string, QtDatapoint*> m_cache;
};

} // namespace core::qml
