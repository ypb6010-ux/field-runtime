#pragma once

#include <QObject>
#include <QString>

#include "core/core_global.h"

namespace core::dp { class Datapoint; class DatapointRegistry; }

namespace core::qml {

// Thin QObject that lets QML look up datapoints by id. The returned Datapoint
// is itself a QObject with `value` Q_PROPERTY + NOTIFY, so QML binds to its
// `value` directly and updates automatically when it changes.
//
// Usage:
//   Text   { text: bridge.dp("belt2.run_state").value }
//   Button { onClicked: bridge.dp("belt2.cmd.start").write(true) }
class CORE_EXPORT DatapointQmlBridge : public QObject {
    Q_OBJECT
public:
    explicit DatapointQmlBridge(dp::DatapointRegistry& registry,
                                QObject* parent = nullptr);
    ~DatapointQmlBridge() override;

    Q_INVOKABLE dp::Datapoint* dp(QString const& id) const;

private:
    dp::DatapointRegistry& m_registry;
};

} // namespace core::qml
