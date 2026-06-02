import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    visible: true
    width: 1040
    height: 700
    title: qsTr("Core 日志与数据库 — 演示面板")

    header: TabBar {
        id: tabs
        TabButton { text: qsTr("数据源 Sources") }
        TabButton { text: qsTr("日志 Logs") }
        TabButton { text: qsTr("历史 History") }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: tabs.currentIndex
        SourcesTab {}
        LogsTab {}
        HistoryTab {}
    }
}
