import QtQuick
import QtLocation
import QtPositioning

import QGroundControl

/// The MissionLineView control is used to add lines between mission items
MapItemView {
    id: root

    property var planStyle: null
    property bool showSpecialVisual: false
    delegate: MapPolyline {
        id: route
        objectName: "planMissionRoute"

        readonly property real vehicleId: root.planStyle ? root.planStyle.vehicleId : 0
        readonly property string layerKind: "route"
        property alias renderedColor: route.line.color
        property alias renderedWidth: route.line.width
        readonly property bool terrainCollision: _terrainCollision

        line.width: root.planStyle && root.planStyle.enabled ? root.planStyle.missionLineWidth : 3
        // Note: Special visuals for ROI are hacked out for now since they are not working correctly
        line.color: _terrainCollision ?
                        "red" :
                        (root.planStyle && root.planStyle.enabled ? root.planStyle.lineColor :
                         (false/*showSpecialVisual*/ ? "green" : QGroundControl.globalPalette.mapMissionTrajectory))
        z:          QGroundControl.zOrderWaypointLines
        path:       _calcMissionLinePath()

        property bool _terrainCollision:    object && object.terrainCollision
        property bool _showSpecialVisual:   Boolean(object && showSpecialVisual && object.specialVisual)

        readonly property real _maxSegmentLengthM: 50000 // 50 km

        function _calcMissionLinePath() {
            if (!object || !object.coordinate1.isValid || !object.coordinate2.isValid) {
                return []
            }

            var coord1 = object.coordinate1
            var coord2 = object.coordinate2

            var distance = coord1.distanceTo(coord2)
            if (distance <= _maxSegmentLengthM) {
                return [coord1, coord2]
            }

            // For longer distances, draw great circle path
            var pathPoints = [coord1]
            var numSegments = Math.ceil(distance / _maxSegmentLengthM)

            for (var i = 1; i < numSegments; i++) {
                var segmentDist = (i * distance) / numSegments
                var interpolatedCoord = coord1.atDistanceAndAzimuth(segmentDist, coord1.azimuthTo(coord2))
                pathPoints.push(interpolatedCoord)
            }

            pathPoints.push(coord2)
            return pathPoints
        }
    }
}
