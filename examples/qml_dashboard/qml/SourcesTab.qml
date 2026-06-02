import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    spacing: 10

    function kindColor(k) {
        if (k.indexOf("Modbus") === 0) return "#2e7d32"
        if (k.indexOf("MQTT") === 0)   return "#1565c0"
        if (k.indexOf("OPC") === 0)    return "#6a1b9a"
        return "#555555"
    }

    component Badge: Rectangle {
        property string text: ""
        property color bg: "#555555"
        radius: 4
        color: bg
        implicitHeight: 22
        implicitWidth: label.implicitWidth + 14
        Label { id: label; anchors.centerIn: parent; text: parent.text
                color: "white"; font.pixelSize: 12 }
    }

    Label {
        Layout.topMargin: 12; Layout.leftMargin: 12
        text: qsTr("数据源 Transports")
        font.bold: true; font.pixelSize: 16
    }

    Frame {
        Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
        Layout.preferredHeight: 150
        ListView {
            anchors.fill: parent
            clip: true
            model: demo ? demo.transports : []
            spacing: 6
            delegate: RowLayout {
                width: ListView.view.width
                spacing: 12
                Label { text: modelData.id; font.bold: true
                        Layout.preferredWidth: 70 }
                Badge { text: modelData.kind; bg: kindColor(modelData.kind) }
                Badge { text: modelData.state
                        bg: modelData.connected ? "#2e7d32" : "#b71c1c" }
                Label { color: "#777"
                        text: "queue=" + modelData.queue + "  p99=" + modelData.p99 + "ms" }
                Item { Layout.fillWidth: true }
            }
        }
    }

    Label {
        Layout.leftMargin: 12
        text: qsTr("数据点 Datapoints（带数据来源标识）")
        font.bold: true; font.pixelSize: 16
    }

    Frame {
        Layout.fillWidth: true; Layout.fillHeight: true
        Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.bottomMargin: 12
        ListView {
            anchors.fill: parent
            clip: true
            model: demo ? demo.datapoints : []
            spacing: 6
            delegate: RowLayout {
                width: ListView.view.width
                spacing: 12
                Label { text: modelData.id; Layout.preferredWidth: 130 }
                Label { text: modelData.value
                        font.family: "Consolas"; font.pixelSize: 15
                        Layout.preferredWidth: 110 }
                Badge { text: modelData.state
                        bg: modelData.state === "Ok" ? "#2e7d32" : "#e0a800" }
                Item { Layout.fillWidth: true }
                Label { text: qsTr("来源:"); color: "#999" }
                Badge { text: modelData.sourceKind
                        bg: kindColor(modelData.sourceKind) }
            }
        }
    }
}
