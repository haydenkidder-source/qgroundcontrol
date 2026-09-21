pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import QGC
import QGroundControl
import QGroundControl.Controls

Rectangle {
    id: root
    objectName: "copNotifications"
    implicitWidth: ScreenTools.defaultFontPixelWidth * 64
    implicitHeight: (COPController.messages.length > 0 && !_minimized)
                    ? notificationLayout.implicitHeight + ScreenTools.defaultFontPixelWidth * 2 : 0
    width: implicitWidth
    height: implicitHeight
    clip: true
    color: QGroundControl.globalPalette.windowShade
    border.color: COPController.unacknowledged ? QGroundControl.globalPalette.colorOrange
                                               : QGroundControl.globalPalette.windowShadeDark
    border.width: ScreenTools.defaultFontPixelWidth / 3
    property var hostWindow
    property bool _minimized: false

    Connections {
        target: COPController
        function onMessagesChanged() {
            if (COPController.unacknowledged) {
                root._minimized = false
                minimizeTimer.stop()
            }
        }
    }

    Timer {
        id: minimizeTimer
        interval: 10000
        onTriggered: root._minimized = true
    }

    ColumnLayout {
        id: notificationLayout
        anchors.fill: parent
        anchors.margins: ScreenTools.defaultFontPixelWidth
        RowLayout {
            Layout.fillWidth: true
            QGCLabel {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: qsTr("Notifications (%1)").arg(COPController.messages.length)
            }
            QGCButton {
                text: qsTr("Acknowledge")
                enabled: COPController.unacknowledged
                onClicked: {
                    COPController.acknowledge()
                    minimizeTimer.restart()
                }
            }
        }
        QGCLabel {
            Layout.fillWidth: true
            visible: COPController.droppedMessages > 0
            wrapMode: Text.WordWrap
            text: qsTr("%1 older notifications omitted").arg(COPController.droppedMessages)
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 5)
            clip: true
            spacing: ScreenTools.defaultFontPixelHeight / 4
            model: COPController.messages
            delegate: QGCLabel {
                required property var modelData
                width: ListView.view.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: Qt.formatTime(modelData.time, "hh:mm:ss") + "  " + modelData.text
            }
        }
    }
}
