// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1100
    height: 760
    visible: true
    title: qsTr("Modbus 现场网关 HMI — FieldRuntime 示例")

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            Label {
                text: qsTr("协议网关")
                font.pixelSize: 18; font.bold: true
            }
            Rectangle {
                width: 12; height: 12; radius: 6
                color: gw.running ? (gw.connected ? "#2ecc71" : "#f39c12") : "#bdc3c7"
            }
            Label { text: gw.running ? (gw.connected ? qsTr("已连接") : qsTr("连接中…")) : qsTr("已停止") }
            Item { Layout.fillWidth: true }
            Label { text: gw.status; elide: Text.ElideRight; Layout.maximumWidth: 560 }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // ── left column: configuration + control ───────────────────────
        ColumnLayout {
            Layout.preferredWidth: 420
            Layout.fillHeight: true
            spacing: 12

            // 1. connection parameters
            GroupBox {
                title: qsTr("① 连接参数配置")
                Layout.fillWidth: true
                GridLayout {
                    anchors.fill: parent
                    columns: 2; columnSpacing: 8; rowSpacing: 6
                    Label { text: qsTr("PLC 主机") }
                    TextField { id: hostField; text: gw.host; Layout.fillWidth: true }
                    Label { text: qsTr("PLC 端口") }
                    SpinBox { id: plcPortField; from: 1; to: 65535; value: gw.plcPort; editable: true }
                    Label { text: qsTr("操作箱端口") }
                    SpinBox { id: opboxPortField; from: 1; to: 65535; value: gw.opboxPort; editable: true }
                    Label { text: qsTr("轮询周期 (ms)") }
                    SpinBox { id: periodField; from: 50; to: 10000; stepSize: 50; value: gw.periodMs; editable: true }
                    Button {
                        text: qsTr("应用并重启运行时")
                        Layout.columnSpan: 2; Layout.fillWidth: true
                        onClicked: {
                            gw.host = hostField.text
                            gw.plcPort = plcPortField.value
                            gw.opboxPort = opboxPortField.value
                            gw.periodMs = periodField.value
                            gw.apply()
                        }
                    }
                }
            }

            // 2. datapoint configuration
            GroupBox {
                title: qsTr("② 数据点配置")
                Layout.fillWidth: true
                Layout.fillHeight: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: gw.points
                        delegate: RowLayout {
                            width: ListView.view.width
                            visible: modelData.role === "status"
                            height: visible ? 28 : 0
                            Label { text: modelData.id; Layout.preferredWidth: 110; elide: Text.ElideRight }
                            Label { text: "HR" + modelData.address; Layout.preferredWidth: 50 }
                            Label { text: modelData.type; Layout.preferredWidth: 50 }
                            Label { text: "×" + modelData.scale; Layout.preferredWidth: 50 }
                            Item { Layout.fillWidth: true }
                            Button { text: qsTr("删除"); flat: true; onClicked: gw.removePoint(modelData.id) }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField { id: newId; placeholderText: qsTr("id"); Layout.preferredWidth: 100 }
                        SpinBox { id: newAddr; from: 0; to: 31; value: 5 }
                        ComboBox { id: newType; model: ["U16","S16","U32","S32","F32"]; Layout.preferredWidth: 80 }
                        TextField { id: newScale; placeholderText: qsTr("scale"); text: "1.0"; Layout.preferredWidth: 60 }
                        Button {
                            text: qsTr("添加")
                            onClicked: {
                                gw.addPoint(newId.text, newAddr.value, newType.currentText, parseFloat(newScale.text))
                                newId.text = ""
                            }
                        }
                    }
                    Label {
                        text: qsTr("提示:增删点后点「应用并重启运行时」生效。")
                        font.pixelSize: 11; color: "#888"
                    }
                }
            }

            // 4. protocol-conversion control
            GroupBox {
                title: qsTr("④ 协议转换控制(操作箱 Modbus Server ↔ PLC 桥接)")
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6
                    Switch {
                        text: qsTr("转发使能(操作箱写 → 下发 PLC)")
                        checked: gw.forwarding
                        onToggled: gw.forwarding = checked
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("设定值") }
                        SpinBox { id: spVal; from: 0; to: 65535; value: 1000; editable: true }
                        Button { text: qsTr("下发(Core sink)"); onClicked: gw.writeSetpoint(spVal.value) }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("操作箱写") }
                        SpinBox { id: opVal; from: 0; to: 65535; value: 4321; editable: true }
                        Button { text: qsTr("模拟操作箱(→桥接→PLC)"); onClicked: gw.simulateOperatorWrite(opVal.value) }
                    }
                }
            }
        }

        // ── right column: live data + log ──────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // 3. live data display
            GroupBox {
                title: qsTr("③ 实时数据")
                Layout.fillWidth: true
                Layout.preferredHeight: 360
                ColumnLayout {
                    anchors.fill: parent
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("数据点"); Layout.preferredWidth: 140; font.bold: true }
                        Label { text: qsTr("地址");   Layout.preferredWidth: 60;  font.bold: true }
                        Label { text: qsTr("类型");   Layout.preferredWidth: 60;  font.bold: true }
                        Label { text: qsTr("数值");   Layout.fillWidth: true;      font.bold: true }
                        Label { text: qsTr("质量");   Layout.preferredWidth: 80;  font.bold: true }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: gw.points
                        delegate: RowLayout {
                            width: ListView.view.width
                            height: 30
                            Label {
                                text: modelData.id + (modelData.role === "echo" ? qsTr("  (回读)") : "")
                                Layout.preferredWidth: 140; elide: Text.ElideRight
                            }
                            Label { text: "HR" + modelData.address; Layout.preferredWidth: 60 }
                            Label { text: modelData.type; Layout.preferredWidth: 60 }
                            Label { text: modelData.value; Layout.fillWidth: true; font.bold: true }
                            Label {
                                text: modelData.state; Layout.preferredWidth: 80
                                color: modelData.valid ? "#2ecc71" : "#e74c3c"
                            }
                        }
                    }
                }
            }

            // live log stream
            GroupBox {
                title: qsTr("运行日志(系统 + 操作审计)")
                Layout.fillWidth: true
                Layout.fillHeight: true
                ListView {
                    anchors.fill: parent
                    clip: true
                    model: gw.logs
                    delegate: RowLayout {
                        width: ListView.view.width
                        spacing: 8
                        Label { text: modelData.ts; color: "#888"; font.family: "monospace" }
                        Label {
                            text: modelData.badge
                            Layout.preferredWidth: 44
                            color: modelData.kind === "operation" ? "#9b59b6" : "#3498db"
                            font.bold: true
                        }
                        Label { text: modelData.category + (modelData.source ? "/" + modelData.source : ""); color: "#666"; Layout.preferredWidth: 150; elide: Text.ElideRight }
                        Label { text: modelData.text; Layout.fillWidth: true; elide: Text.ElideRight }
                    }
                }
            }
        }
    }
}
