import QtQuick

import QGroundControl
import QGroundControl.Controls

Item {
    id: root

    required property color color
    required property color borderColor
    required property real borderWidth
    required property color textColor

    property bool checked: false
    property bool interactive: false
    readonly property string markerShape: "diamond"
    readonly property real anchorPointX: width / 2
    readonly property real anchorPointY: height / 2
    property alias markerColor: diamond.color
    property alias markerBorderColor: diamond.border.color
    property alias markerBorderWidth: diamond.border.width
    property alias label: rallyLabel.text
    signal clicked()

    width: ScreenTools.defaultFontPixelHeight * 1.6
    height: width

    Rectangle {
        objectName: "planRallySelectionHalo"
        anchors.centerIn: parent
        width: root.width * 2
        height: width
        radius: width / 2
        color: "transparent"
        border.color: Qt.rgba(1, 1, 1, 0.5)
        border.width: root.borderWidth
        visible: root.checked
    }
    Rectangle {
        id: diamond
        anchors.centerIn: parent
        width: parent.width * 0.72
        height: width
        rotation: 45
        color: root.color
        border.color: root.borderColor
        border.width: root.borderWidth
    }
    QGCLabel {
        id: rallyLabel
        anchors.centerIn: parent
        text: qsTr("R")
        color: root.textColor
        font.bold: true
        font.pointSize: ScreenTools.smallFontPointSize
    }
    QGCMouseArea {
        anchors.fill: parent
        enabled: root.interactive
        onClicked: root.clicked()
    }
}
