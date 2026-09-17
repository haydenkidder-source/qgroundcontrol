pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import QGC
import QGroundControl
import QGroundControl.Controls

Rectangle {
    id: root
    implicitHeight: ScreenTools.defaultFontPixelHeight * 5
    color: QGroundControl.globalPalette.windowShade
    border.color: COPController.unacknowledged ? QGroundControl.globalPalette.colorOrange
                                               : QGroundControl.globalPalette.windowShadeDark
    border.width: ScreenTools.defaultFontPixelWidth / 3
    property var hostWindow

    RowLayout {
        anchors.fill: parent
        anchors.margins: ScreenTools.defaultFontPixelWidth
        ColumnLayout {
            QGCLabel { text: qsTr("Notifications (%1)").arg(COPController.messages.length) }
            QGCLabel {
                visible: COPController.droppedMessages > 0
                text: qsTr("%1 older notifications omitted").arg(COPController.droppedMessages)
            }
            QGCButton {
                text: qsTr("Acknowledge")
                enabled: COPController.unacknowledged
                onClicked: COPController.acknowledge()
            }
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
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
            QGCLabel {
                anchors.centerIn: parent
                visible: COPController.messages.length === 0
                text: qsTr("No operator notifications")
            }
        }
    }
    Connections {
        target: QGroundControl
        function onShowMessageDialogRequested(owner, title, text, buttons, acceptFunction, closeFunction) {
            COPController.notify(title + ": " + text)
        }
    }
}
