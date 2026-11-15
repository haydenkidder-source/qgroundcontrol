import QtQuick
import QtQuick.Controls
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap

/// Structure Scan Complex Mission Item visuals
Item {
    id: _root

    property var planStyle: null
    readonly property bool _styled: planStyle && planStyle.enabled

    property var map        ///< Map control to place item in
    property var vehicle

    property var _missionItem:      object
    property var _structurePolygon: object.structurePolygon
    property var _flightPolygon:    object.flightPolygon
    property bool interactive:      parent.interactive

    signal clicked(int sequenceNumber)

    function _addVisualElements() {
        objMgr.createObjects([entryPointComponent, exitPointComponent], map, true /* parentObjectIsMap */)
    }

    Component.onCompleted: {
        _addVisualElements()
    }

    QGCDynamicObjectManager { id: objMgr }

    QGCMapPolygonVisuals {
        planStyle:          _root.planStyle
        patternOutline:     true
        boundaryOpacity:    _root.opacity
        mapControl:         map
        mapPolygon:         _structurePolygon
        interactive:        _missionItem.isCurrentItem && _root.interactive
        borderWidth:        1
        borderColor:        "black"
        interiorColor:      _root._styled ? _root.planStyle.lineColor : "green"
        altColor:           "red"
        interiorOpacity:    0.5 * _root.opacity
    }

    QGCMapPolygonVisuals {
        mapControl:         map
        mapPolygon:         _flightPolygon
        interactive:        false
        borderWidth:        _root._styled ? _root.planStyle.missionLineWidth : 2
        borderColor:        _root._styled ? _root.planStyle.lineColor : "white"
        interiorOpacity:    _root.opacity
    }

    // Entry point
    Component {
        id: entryPointComponent

        MapQuickItem {
            objectName:     "planPatternEntryMarker"
            anchorPoint.x:  sourceItem.anchorPointX
            anchorPoint.y:  sourceItem.anchorPointY
            z:              QGroundControl.zOrderMapItems
            coordinate:     _missionItem.coordinate
            visible:        _missionItem.exitCoordinate.isValid && (_root._styled || _root.interactive)
            opacity:        _root._styled ? _root.opacity : 1

            sourceItem: MissionItemIndexLabel {
                objectName: "planPatternMarker"
                planStyle:  _root.planStyle
                index:      _missionItem.sequenceNumber
                label:      qsTr("Entry")
                checked:    _missionItem.isCurrentItem
                onClicked:  if (!_root._styled || _root.interactive) _root.clicked(_missionItem.sequenceNumber)
            }
        }
    }

    // Exit point
    Component {
        id: exitPointComponent

        MapQuickItem {
            objectName:     "planPatternExitMarker"
            anchorPoint.x:  sourceItem.anchorPointX
            anchorPoint.y:  sourceItem.anchorPointY
            z:              QGroundControl.zOrderMapItems
            coordinate:     _missionItem.exitCoordinate
            visible:        _missionItem.exitCoordinate.isValid && (_root._styled || _root.interactive)
            opacity:        _root._styled ? _root.opacity : 1

            sourceItem: MissionItemIndexLabel {
                objectName: "planPatternMarker"
                planStyle:  _root.planStyle
                index:      _missionItem.lastSequenceNumber
                label:      qsTr("Exit")
                checked:    _missionItem.isCurrentItem
                onClicked:  if (!_root._styled || _root.interactive) _root.clicked(_missionItem.sequenceNumber)
            }
        }
    }
}
