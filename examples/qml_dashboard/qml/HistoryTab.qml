import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    spacing: 8

    property var result: ({ data: [], pages: 0, page: 0 })

    function nowText(offsetMs) {
        return Qt.formatDateTime(new Date(Date.now() + offsetMs),
                                 "yyyy-MM-dd hh:mm:ss")
    }
    function runQuery() {
        var s = startField.text, e = endField.text, p = pageSpin.value
        if (sourceBox.currentIndex === 0)
            result = demo.queryTelemetry(tagField.text, s, e, p)
        else if (sourceBox.currentIndex === 1)
            result = demo.queryOperation(s, e, p)
        else
            result = demo.querySystem(0, s, e, p)
    }
    function rowText(rec) {
        var parts = []
        for (var k in rec)
            if (k !== "ts" && k !== "id" && k !== "ts_text")
                parts.push(k + "=" + rec[k])
        return parts.join("   ")
    }

    // ── unavailable banner ───────────────────────────────────────────────
    Label {
        visible: !!demo && !demo.dbAvailable
        Layout.fillWidth: true; Layout.margins: 20
        text: qsTr("数据库不可用 (Postgres)。启动时未能连接,历史查询不可用。")
        color: "#c62828"; font.pixelSize: 15
    }

    // ── query controls ───────────────────────────────────────────────────
    RowLayout {
        visible: !!demo && demo.dbAvailable
        Layout.fillWidth: true
        Layout.topMargin: 10; Layout.leftMargin: 12; Layout.rightMargin: 12
        spacing: 8

        ComboBox {
            id: sourceBox
            model: ["Telemetry", "Operation", "System"]
            onActivated: { tagField.visible = (currentIndex === 0) }
        }
        TextField {
            id: tagField; placeholderText: qsTr("tag (可空)")
            text: "plc1.temperature"; Layout.preferredWidth: 150
        }
        TextField { id: startField; text: nowText(-3600 * 1000); Layout.preferredWidth: 160 }
        Label { text: "→" }
        TextField { id: endField; text: nowText(60 * 1000); Layout.preferredWidth: 160 }
        Label { text: qsTr("页") }
        SpinBox { id: pageSpin; from: 0; to: 9999; value: 0 }
        Button { text: qsTr("查询"); highlighted: true; onClicked: runQuery() }
        Item { Layout.fillWidth: true }
        Label { text: qsTr("第 %1/%2 页").arg(result.page + 1).arg(Math.max(result.pages, 1))
                color: "#777" }
    }

    // ── results ──────────────────────────────────────────────────────────
    Frame {
        visible: !!demo && demo.dbAvailable
        Layout.fillWidth: true; Layout.fillHeight: true
        Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.bottomMargin: 12
        ListView {
            anchors.fill: parent
            clip: true
            model: result.data
            spacing: 3
            delegate: RowLayout {
                width: ListView.view.width
                spacing: 10
                Label { text: modelData.ts_text ? modelData.ts_text : ""
                        color: "#1565c0"; font.family: "Consolas"
                        Layout.preferredWidth: 150 }
                Label { text: rowText(modelData); Layout.fillWidth: true
                        font.family: "Consolas"; elide: Text.ElideRight }
            }
        }
    }
}
