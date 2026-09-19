pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtPositioning
import QtLocation

import QGC
import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap

Rectangle {
    id: root
    color: QGroundControl.globalPalette.window
    visible: COPController.selectedSysid === 0 || !selected || !selected.connected
             || selected.vehicle !== QGroundControl.multiVehicleManager.activeVehicle

    property var hostWindow
    readonly property var selected: COPController.selected
    property bool centered: false
    readonly property bool overview: COPController.selectedSysid === 0

    COPReferencePoint { id: reference }

    // Consume map input while the retained-state page covers the active vehicle's FlyView.
    DeadMouseArea { anchors.fill: parent }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: ScreenTools.defaultFontPixelWidth
        QGCLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.overview ? qsTr("Common Operating Picture")
                  : root.selected ? qsTr("%1 · Last update: %2 · Flight mode: %3")
                    .arg(root.selected.label)
                    .arg(isNaN(root.selected.lastSeen.getTime()) ? qsTr("No telemetry received this session")
                                                              : Qt.formatDateTime(root.selected.lastSeen))
                    .arg(root.selected.flightMode || qsTr("Unknown")) : qsTr("No vehicle selected")
        }
        RowLayout {
            id: regions
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            FlightMap {
                id: map
                objectName: "copMap"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.minimumHeight: 0
                Layout.preferredWidth: root.width * 0.6
                clip: true
                mapName: "COP"
                allowVehicleLocationCenter: false
                MapQuickItem {
                    coordinate: QtPositioning.coordinate(reference.latitude, reference.longitude, reference.altitude)
                    visible: root.overview && reference.valid
                    sourceItem: QGCLabel {
                        text: reference.source
                        color: QGroundControl.globalPalette.colorOrange
                    }
                }
                MapItemView {
                    model: COPController.vehicles
                    delegate: MapQuickItem {
                        id: vehicleMarker
                        required property var object
                        coordinate: object.coordinate
                        visible: coordinate.isValid && (root.overview ? object.connected : object === root.selected)
                        anchorPoint.x: marker.width / 2
                        anchorPoint.y: marker.height
                        Connections {
                            target: vehicleMarker.object
                            function onStateChanged() {
                                if (!root.centered && vehicleMarker.coordinate.isValid) {
                                    map.center = vehicleMarker.coordinate
                                    root.centered = true
                                }
                            }
                        }
                        sourceItem: Rectangle {
                            id: marker
                            width: label.implicitWidth + ScreenTools.defaultFontPixelWidth * 2
                            height: label.implicitHeight + ScreenTools.defaultFontPixelHeight
                            color: QGroundControl.globalPalette.window
                            border.color: QGroundControl.globalPalette.colorBlue
                            QGCLabel {
                                id: label
                                anchors.centerIn: parent
                                text: vehicleMarker.object.label
                            }
                        }
                    }
                }
            }
            ColumnLayout {
                visible: root.overview
                Layout.minimumWidth: 0
                Layout.minimumHeight: 0
                Layout.preferredWidth: root.width * 0.4
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                QGCLabel {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: qsTr("Connected vehicle video")
                }
                // Video tiles are installed independently of the active vehicle's FlyView receiver.
                Loader {
                    objectName: "copVideoRegion"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumWidth: 0
                    Layout.minimumHeight: 0
                    source: "COPVideoGrid.qml"
                }
            }
        }
        QGCButton {
            text: qsTr("Fit vehicles")
            onClicked: map.fitViewportToVisibleMapItems()
        }
    }
    Connections {
        target: COPController
        function onSelectionChanged() {
            if (root.selected && root.selected.coordinate.isValid) {
                map.center = root.selected.coordinate
            }
        }
    }
}
