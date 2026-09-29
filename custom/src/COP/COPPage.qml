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
    // Keep-out zones are red whatever the vehicle color, so "forbidden" never depends on which vehicle owns it.
    readonly property color _keepOutColor: "#E53935"
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
                result.push({ path: polygon.path, inclusion: polygon.inclusion, color: vehicle.color })
            }
        }
        return result
    }

    function _overlayFenceCircles() {
        const result = []
        for (const vehicle of root._overlayVehicles()) {
            for (const circle of vehicle.fenceCircles) {
                result.push({ center: circle.center, radius: circle.radius, inclusion: circle.inclusion, color: vehicle.color })
            }
        }
        return result
    }

    // One entry per route point, so each waypoint gets a marker and the first/last are labelled S and E.
    function _overlayMissionPoints() {
        const result = []
        for (const vehicle of root._overlayVehicles()) {
            const path = vehicle.missionCoordinates
            for (let i = 0; i < path.length; i++) {
                result.push({ point: path[i], color: vehicle.color, start: i === 0,
                              end: i === path.length - 1 && path.length > 1 })
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
                // Read-only per-vehicle plan overlay (COPNavigation.qml's "Plan" toggle). The owning vehicle
                // is always the outline/route color; the layer type is told apart by shape and fill:
                //   route      - thick line with waypoint dots, S = start, E = end
                //   keep-in    - outline only (stay inside)
                //   keep-out   - red fill (stay out)
                //   rally      - diamond
                // Drawn bottom to top: fences, rally, route, waypoints, then the vehicle markers below.
                MapItemView {
                    model: root._overlayFencePolygons()
                    delegate: MapPolygon {
                        required property var modelData
                        path: modelData.path
                        color: modelData.inclusion ? "transparent"
                               : Qt.rgba(root._keepOutColor.r, root._keepOutColor.g, root._keepOutColor.b, 0.35)
                        border.color: modelData.color
                        border.width: 3
                    }
                }
                MapItemView {
                    model: root._overlayFenceCircles()
                    delegate: MapCircle {
                        required property var modelData
                        center: modelData.center
                        radius: modelData.radius
                        color: modelData.inclusion ? "transparent"
                               : Qt.rgba(root._keepOutColor.r, root._keepOutColor.g, root._keepOutColor.b, 0.35)
                        border.color: modelData.color
                        border.width: 3
                    }
                }
                MapItemView {
                    model: root._overlayRallyPoints()
                    delegate: MapQuickItem {
                        id: rallyItem
                        required property var modelData
                        coordinate: rallyItem.modelData.point
                        anchorPoint.x: rallyMarker.width / 2
                        anchorPoint.y: rallyMarker.height / 2
                        sourceItem: Item {
                            id: rallyMarker
                            width: ScreenTools.defaultFontPixelHeight * 1.6
                            height: width
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width * 0.72
                                height: width
                                rotation: 45
                                color: rallyItem.modelData.color
                                border.color: "white"
                                border.width: 2
                            }
                            QGCLabel {
                                anchors.centerIn: parent
                                text: qsTr("R")
                                color: "black"
                                font.bold: true
                                font.pointSize: ScreenTools.smallFontPointSize
                            }
                        }
                    }
                }
                MapItemView {
                    model: root._overlayMissionPaths()
                    delegate: MapPolyline {
                        required property var modelData
                        path: modelData.path
                        line.color: modelData.color
                        line.width: 4
                    }
                }
                MapItemView {
                    model: root._overlayMissionPoints()
                    delegate: MapQuickItem {
                        id: waypointItem
                        required property var modelData
                        coordinate: waypointItem.modelData.point
                        anchorPoint.x: dot.width / 2
                        anchorPoint.y: dot.height / 2
                        sourceItem: Rectangle {
                            id: dot
                            readonly property bool labelled: waypointItem.modelData.start || waypointItem.modelData.end
                            width: ScreenTools.defaultFontPixelHeight * (labelled ? 1.4 : 0.75)
                            height: width
                            radius: width / 2
                            color: waypointItem.modelData.color
                            border.color: "white"
                            border.width: 2
                            QGCLabel {
                                anchors.centerIn: parent
                                visible: dot.labelled
                                text: waypointItem.modelData.start ? qsTr("S") : qsTr("E")
                                color: "black"
                                font.bold: true
                                font.pointSize: ScreenTools.smallFontPointSize
                            }
                        }
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
                Rectangle {
                    id: legend
                    objectName: "copOverlayLegend"
                    visible: root._overlayVehicles().length > 0
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: ScreenTools.defaultFontPixelWidth
                    width: legendColumn.implicitWidth + ScreenTools.defaultFontPixelWidth * 2
                    height: legendColumn.implicitHeight + ScreenTools.defaultFontPixelWidth * 2
                    radius: ScreenTools.defaultFontPixelWidth / 2
                    color: Qt.rgba(QGroundControl.globalPalette.window.r, QGroundControl.globalPalette.window.g,
                                   QGroundControl.globalPalette.window.b, 0.88)
                    ColumnLayout {
                        id: legendColumn
                        anchors.centerIn: parent
                        spacing: ScreenTools.defaultFontPixelHeight / 6
                        Repeater {
                            model: root._overlayVehicles()
                            delegate: RowLayout {
                                required property var modelData
                                spacing: ScreenTools.defaultFontPixelWidth
                                Rectangle {
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelHeight
                                    Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 0.6
                                    color: modelData.color
                                }
                                QGCLabel { text: modelData.label; font.pointSize: ScreenTools.smallFontPointSize }
                            }
                        }
                        RowLayout {
                            spacing: ScreenTools.defaultFontPixelWidth
                            Rectangle {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelHeight
                                Layout.preferredHeight: 4
                                color: QGroundControl.globalPalette.text
                            }
                            QGCLabel { text: qsTr("Route (S start, E end)"); font.pointSize: ScreenTools.smallFontPointSize }
                        }
                        RowLayout {
                            spacing: ScreenTools.defaultFontPixelWidth
                            Rectangle {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelHeight
                                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 0.6
                                color: "transparent"
                                border.color: QGroundControl.globalPalette.text
                                border.width: 3
                            }
                            QGCLabel { text: qsTr("Keep-in fence (stay inside)"); font.pointSize: ScreenTools.smallFontPointSize }
                        }
                        RowLayout {
                            spacing: ScreenTools.defaultFontPixelWidth
                            Rectangle {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelHeight
                                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 0.6
                                color: Qt.rgba(root._keepOutColor.r, root._keepOutColor.g, root._keepOutColor.b, 0.35)
                                border.color: QGroundControl.globalPalette.text
                                border.width: 3
                            }
                            QGCLabel { text: qsTr("Keep-out zone (stay outside)"); font.pointSize: ScreenTools.smallFontPointSize }
                        }
                        RowLayout {
                            spacing: ScreenTools.defaultFontPixelWidth
                            Item {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelHeight
                                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: parent.width * 0.72
                                    height: width
                                    rotation: 45
                                    color: QGroundControl.globalPalette.text
                                }
                            }
                            QGCLabel { text: qsTr("Rally point"); font.pointSize: ScreenTools.smallFontPointSize }
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
