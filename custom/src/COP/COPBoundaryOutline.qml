pragma ComponentBehavior: Bound

import QtQuick
import QtLocation
import QtPositioning

import QGroundControl.FlightMap

// Dashed outline of a fence boundary on the map. The outline is a Shape anchored at the first vertex, so panning
// moves it with the map and only a change of zoom or size re-projects it.
MapQuickItem {
    id: root

    property FlightMap mapControl
    property var vertices: []           ///< QGeoCoordinate ring, first vertex not repeated
    property color lineColor: "white"
    required property var dashPattern

    property var _screenPoints: []
    property size _extent: Qt.size(0, 0)

    // Qt's dash stroker drops dashes outside the Shape's own bounds, so the points are shifted to lie within them and
    // the anchor point moves the Shape back over the first vertex.
    function _project() {
        const offsets = []
        let left = 0
        let top = 0
        let right = 0
        let bottom = 0
        if (mapControl && mapControl.mapReady && vertices.length > 1) {
            const origin = mapControl.fromCoordinate(vertices[0], false /* clipToViewPort */)
            if (Number.isFinite(origin.x) && Number.isFinite(origin.y)) {
                for (const vertex of vertices) {
                    const point = mapControl.fromCoordinate(vertex, false /* clipToViewPort */)
                    if (Number.isFinite(point.x) && Number.isFinite(point.y)) {
                        const x = point.x - origin.x
                        const y = point.y - origin.y
                        offsets.push(Qt.point(x, y))
                        left = Math.min(left, x)
                        top = Math.min(top, y)
                        right = Math.max(right, x)
                        bottom = Math.max(bottom, y)
                    }
                }
            }
        }
        const points = offsets.map(offset => Qt.point(offset.x - left, offset.y - top))
        if (points.length > 0) {
            points.push(Qt.point(-left, -top))
        }
        anchorPoint = Qt.point(-left, -top)
        _extent = Qt.size(right - left, bottom - top)
        _screenPoints = points
    }

    coordinate: vertices.length > 0 ? vertices[0] : QtPositioning.coordinate()

    onVerticesChanged: _project()
    onMapControlChanged: _project()

    Component.onCompleted: _project()

    Connections {
        target: root.mapControl

        function onMapReadyChanged() { root._project() }
        function onZoomLevelChanged() { root._project() }
        function onWidthChanged() { root._project() }
        function onHeightChanged() { root._project() }
    }

    sourceItem: COPDashedLine {
        width: root._extent.width
        height: root._extent.height
        color: root.lineColor
        dashPattern: root.dashPattern
        points: root._screenPoints
    }
}
