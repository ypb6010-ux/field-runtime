import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    spacing: 8

    function levelColor(rec) {
        if (rec.kind === "operation") return "#6a1b9a"
        switch (rec.level) {
        case 0: return "#888888"   // Trace
        case 1: return "#00838f"   // Debug
        case 2: return "#2e7d32"   // Info
        case 3: return "#e0a800"   // Warn
        case 4: return "#c62828"   // Error
        case 5: return "#b71c1c"   // Critical
        }
        return "#333"
    }

    // ── toolbar ──────────────────────────────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 10; Layout.leftMargin: 12; Layout.rightMargin: 12
        spacing: 10

        Label { text: qsTr("级别阈值:") }
        ComboBox {
            id: levelBox
            currentIndex: 2
            model: ["Trace", "Debug", "Info", "Warn", "Error", "Critical"]
            onActivated: demo.setLogLevel(currentIndex)
        }

        ToolSeparator {}

        Label { text: qsTr("运行日志(审计):") }
        Button { text: qsTr("启动"); onClicked: demo.emitOperation("start", "line1") }
        Button { text: qsTr("停止"); onClicked: demo.emitOperation("stop", "line1") }
        Button { text: qsTr("复位"); onClicked: demo.emitOperation("reset", "line1") }

        ToolSeparator {}

        // Direct use of the injected `log` QML bridge — a system log from the UI.
        Button { text: qsTr("UI 警告");
                 onClicked: log.warn("ui", "operator pressed the warn button") }

        Item { Layout.fillWidth: true }
        Label { text: qsTr("丢弃: ") + (demo ? demo.dropped : 0); color: "#999" }
    }

    // ── live stream ──────────────────────────────────────────────────────
    Frame {
        Layout.fillWidth: true; Layout.fillHeight: true
        Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.bottomMargin: 12
        ListView {
            anchors.fill: parent
            clip: true
            model: demo ? demo.logs : []
            spacing: 2
            delegate: RowLayout {
                width: ListView.view.width
                spacing: 8
                Label { text: modelData.ts; color: "#999"
                        font.family: "Consolas"; Layout.preferredWidth: 90 }
                Rectangle {
                    radius: 3; color: levelColor(modelData)
                    implicitHeight: 18; implicitWidth: b.implicitWidth + 10
                    Label { id: b; anchors.centerIn: parent; text: modelData.badge
                            color: "white"; font.pixelSize: 11 }
                }
                Label { text: "[" + modelData.category + "/" + modelData.source + "]"
                        color: "#777"; Layout.preferredWidth: 200; elide: Text.ElideRight }
                Label { text: modelData.text; Layout.fillWidth: true
                        elide: Text.ElideRight }
            }
        }
    }
}
