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

    // Flattened, reactive views over whichever vehicles currently have their COP "Plan" overlay
    // toggled on (COPNavigation.qml), one flat list per map-item type so each can be rendered with
    // a single MapItemView, each entry still carrying that vehicle's color to draw with.
    function _overlayVehicles() {
        const result = []
        for (let i = 0; i < COPController.vehicles.count; i++) {
            const vehicle = COPController.vehicles.get(i)
            if (vehicle.planOverlayVisible) {
                result.push(vehicle)
            }
        }
        return result
    }

    function _overlayMissionPaths() {
        const result = []
        for (const vehicle of root._overlayVehicles()) {
            if (vehicle.missionCoordinates.length > 1) {
                result.push({ path: vehicle.missionCoordinates, color: vehicle.color })
            }
        }
        return result
    }

    function _overlayFencePolygons() {
        const result = []
        for (const vehicle of root._overlayVehicles()) {
            for (const polygon of vehicle.fencePolygons) {
                result.push({ path: polygon.path, color: vehicle.color })
            }
        }
        return result
    }

    function _overlayFenceCircles() {
        const result = []
        for (const vehicle of root._overlayVehicles()) {
            for (const circle of vehicle.fenceCircles) {
                result.push({ center: circle.center, radius: circle.radius, color: vehicle.color })
            }
        }
        return result
    }

    function _overlayRallyPoints() {
        const result = []
        for (const vehicle of root._overlayVehicles()) {
            for (const point of vehicle.rallyPoints) {
                result.push({ point: point, color: vehicle.color })
            }
        }
        return result
    }

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
                // Read-only per-vehicle plan overlay (COPNavigation.qml's "Plan" toggle), drawn
                // below the vehicle markers below. Fence/rally items first so the mission polyline
                // and vehicle markers show up on top of them.
                MapItemView {
                    model: root._overlayFencePolygons()
                    delegate: MapPolygon {
                        required property var modelData
                        path: modelData.path
                        color: Qt.rgba(modelData.color.r, modelData.color.g, modelData.color.b, 0.15)
                        border.color: modelData.color
                        border.width: 2
                    }
                }
                MapItemView {
                    model: root._overlayFenceCircles()
                    delegate: MapCircle {
                        required property var modelData
                        center: modelData.center
                        radius: modelData.radius
                        color: Qt.rgba(modelData.color.r, modelData.color.g, modelData.color.b, 0.15)
                        border.color: modelData.color
                        border.width: 2
                    }
                }
                MapItemView {
                    model: root._overlayRallyPoints()
                    delegate: MapCircle {
                        required property var modelData
                        center: modelData.point
                        radius: 15
                        color: modelData.color
                        border.color: QGroundControl.globalPalette.window
                        border.width: 1
                    }
                }
                MapItemView {
                    model: root._overlayMissionPaths()
                    delegate: MapPolyline {
                        required property var modelData
                        path: modelData.path
                        line.color: modelData.color
                        line.width: 2
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
                            border.color: vehicleMarker.object.color
                            border.width: 2
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
