import QtQuick

import QGroundControl
import QGroundControl.Controls

Item {
    id: root

    required property color color
    required property color borderColor
    required property real borderWidth
    required property color textColor

    width: ScreenTools.defaultFontPixelHeight * 1.6
    height: width

    Rectangle {
        anchors.centerIn: parent
        width: parent.width * 0.72
        height: width
        rotation: 45
        color: root.color
        border.color: root.borderColor
        border.width: root.borderWidth
    }
    QGCLabel {
        anchors.centerIn: parent
        text: qsTr("R")
        color: root.textColor
        font.bold: true
        font.pointSize: ScreenTools.smallFontPointSize
    }
}
