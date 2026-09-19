pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtMultimedia

import QGC
import QGroundControl
import QGroundControl.Controls

Flickable {
    id: root
    contentWidth: width
    contentHeight: grid.implicitHeight
    flickableDirection: Flickable.VerticalFlick
    clip: true
    readonly property bool hasConnectedVehicle: {
        for (let i = 0; i < COPController.vehicles.count; ++i) {
            const entry = COPController.vehicles.get(i)
            if (entry && entry.connected) {
                return true
            }
        }
        return false
    }

    QGCLabel {
        width: root.width
        visible: !root.hasConnectedVehicle
        wrapMode: Text.WordWrap
        text: qsTr("Connect a vehicle to view live video")
    }

    GridLayout {
        id: grid
        width: root.width
        columns: width > ScreenTools.defaultFontPixelWidth * 70 ? 2 : 1
        Repeater {
            model: COPController.vehicles
            ColumnLayout {
                id: tile
                required property var object
                visible: object.connected
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                QGCLabel {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: tile.object.label
                }
                QGCTextField {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: tile.object.videoUri
                    placeholderText: qsTr("Video URI (empty: vehicle advertised stream)")
                    onEditingFinished: tile.object.videoUri = text
                }
                VideoOutput {
                    id: output
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: width * 9 / 16
                    fillMode: VideoOutput.PreserveAspectFit
                    COPVideoSession {
                        id: session
                        uri: tile.object.videoUri
                        vehicle: tile.object.vehicle
                        enabled: tile.object.connected && root.visible && COPController.selectedSysid === 0
                        output: output
                    }
                    QGCLabel {
                        anchors.centerIn: parent
                        width: parent.width
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        visible: !session.decoding
                        text: session.status
                    }
                }
            }
        }
    }
}
