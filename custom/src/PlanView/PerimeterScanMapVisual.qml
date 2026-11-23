import QtQuick
import QtQuick.Controls
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap
import QGroundControl.PlanView

/// \brief Map visuals for PerimeterScanComplexItem.
///
/// Shows an interactive, editable polygon using the standard QGCMapPolygonVisuals
/// component, which automatically provides the full Polygon Tools toolbar
/// (Basic, Circular, Trace, Load KML, …) when the item is selected.
///
/// A flight-path polyline traces the perimeter in waypoint order so the user
/// can see the direction of travel.

Item {
    id: _root

    property var  planStyle: null
    readonly property bool _styled: planStyle && planStyle.enabled

    property var  map
    property var  vehicle
    property bool interactive: true

    signal clicked(int sequenceNumber)

    property var  _missionItem:  object
    property var  _polygon:      object.perimeterPolygon
    property bool _currentItem:  object.isCurrentItem
    property var  _arrowView:    null
    readonly property var _arrowPath: {
        const path = _polygon.path
        const dragPath = _polygon.dragPath
        return _polygon.vertexDrag ? dragPath : path
    }

    // ----- polygon editing visuals (provides the Polygon Tools toolbar) -----
    QGCMapPolygonVisuals {
        id:               polygonVisuals
        planStyle:        _root.planStyle
        patternOutline:   true
        boundaryOpacity:  _root.opacity
        fillObjectName:    "planPerimeterFill"
        mapControl:       map
        mapPolygon:       _polygon
        interactive:      _currentItem && _root.interactive
        borderWidth:      2
        borderColor:      "#00cc44"
        interiorColor:    _root._styled ? _root.planStyle.lineColor : "#00cc44"
        interiorOpacity:  0.08 * _root.opacity
    }

    // ----- flight-path polyline (perimeter traversal order) ----------------
    // Shown only when the item is selected so the map stays uncluttered.
    Component {
        id: flightPathComponent

        MapPolyline {
            objectName: "planPerimeterTraversal"
            line.color: _root._styled ? _root.planStyle.lineColor : "#00cc44"
            line.width: _root._styled ? _root.planStyle.missionLineWidth : 2
            visible:    _currentItem
            opacity:    _root.opacity

            // Build the closed path from the polygon vertices.
            path: {
                const pts = _polygon.path
                if (pts.length < 2) return []
                // Append first vertex at the end to close the loop visually.
                return pts.concat([pts[0]])
            }
        }
    }

    Component {
        id: directionArrowsComponent

        MapItemView {
            model: _root._arrowPath.length >= 3 ? _root._arrowPath.length : 0
            delegate: MapLineArrow {
                required property int index
                objectName:    "perimeterDirectionArrow_" + index
                fromCoord:     _root._arrowPath[index] || QtPositioning.coordinate()
                toCoord:       _root._arrowPath[(index + 1) % _root._arrowPath.length] || QtPositioning.coordinate()
                arrowPosition: 2
                arrowColor:    _root._styled ? _root.planStyle.lineColor : "#00cc44"
                mapControl:    _root.map
                visible:       _currentItem
                opacity:       _root.opacity
            }
        }
    }

    // ----- entry-point marker ----------------------------------------------
    Component {
        id: entryPointComponent

        MapQuickItem {
            objectName: "planPerimeterEntry"
            anchorPoint.x: sourceItem.width  / 2
            anchorPoint.y: sourceItem.height / 2
            coordinate:    _missionItem.coordinate
            visible:       _currentItem && _polygon.isValid
            opacity:       _root.opacity
            z:             QGroundControl.zOrderMapItems + 1

            sourceItem: MissionItemIndexLabel {
                objectName: "planPerimeterMarker"
                planStyle:   _root.planStyle
                checked:     true
                index:       _missionItem.sequenceNumber
                label:       ""
                onClicked:   _root.clicked(_missionItem.sequenceNumber)
            }
        }
    }

    QGCDynamicObjectManager { id: objMgr }

    Component.onCompleted: {
        _arrowView = directionArrowsComponent.createObject(map)
        map.addMapItemView(_arrowView)
        objMgr.createObjects(
            [flightPathComponent, entryPointComponent],
            map,
            true /* parentObjectIsMap */)
    }

    Component.onDestruction: {
        if (_arrowView) {
            map.removeMapItemView(_arrowView)
            _arrowView.destroy()
        }
        objMgr.destroyObjects()
    }
}
