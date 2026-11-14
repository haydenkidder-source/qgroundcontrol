import QtQuick
import QtQuick.Controls
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap

/// Rally Point map visuals
Item {
    id: _root
    z: QGroundControl.zOrderMapItems

    property var    planStyle: null
    readonly property bool _styled: planStyle && planStyle.enabled

    property var    map
    property var    myRallyPointController
    property bool   interactive:            false   ///< true: user can interact with items
    property bool   planView:               false   ///< true: visuals showing in plan view

    property bool   _interactive:           interactive
    property var    _rallyPointsComponent
    property bool   _rallyPointsSupported:  myRallyPointController.supported
    property var    _rallyPoints:           myRallyPointController.points

    Component.onCompleted: {
        _rallyPointsComponent = rallyPointsComponent.createObject(map)
    }

    Component.onDestruction: {
        _rallyPointsComponent.destroy()
    }

    Component {
        id: dragAreaComponent

        MissionItemIndicatorDrag {
            mapControl:     _root.map
            itemCoordinate: rallyPointObject.coordinate
            visible:        rallyPointObject === myRallyPointController.currentRallyPoint && _root.interactive

            property var rallyPointObject

            // The drag only moves the point. Its altitude is read live because the drag breaks the itemCoordinate binding
            onItemCoordinateChanged: rallyPointObject.coordinate = QtPositioning.coordinate(itemCoordinate.latitude, itemCoordinate.longitude, rallyPointObject.coordinate.altitude)

            // A PX4 AMSL altitude is the ground height under the point, so it is read again where the point lands
            onDragStop: {
                const vehicle = myRallyPointController.masterController.controllerVehicle
                if (vehicle && vehicle.px4Firmware && rallyPointObject.altitudeFrame === QGroundControl.AltitudeFrameAbsolute) {
                    rallyPointObject.useTerrainAltitude()
                }
            }
        }
    }

    Component {
        id: rallyPointComponent

        MapQuickItem {
            id:             itemIndicator
            objectName: "planRallyPoint"
            readonly property real vehicleId: _root.planStyle ? _root.planStyle.vehicleId : 0
            readonly property bool checked: markerLoader.item ? markerLoader.item.checked : false
            readonly property string markerShape: markerLoader.item && "markerShape" in markerLoader.item
                                                  ? markerLoader.item.markerShape : "circle"
            readonly property color markerColor: markerLoader.item
                                                 ? ("markerShape" in markerLoader.item ? markerLoader.item.markerColor
                                                                                      : markerLoader.item.renderedBodyColor)
                                                 : "transparent"
            readonly property color markerBorderColor: markerLoader.item
                                                       ? ("markerShape" in markerLoader.item ? markerLoader.item.markerBorderColor
                                                                                            : markerLoader.item.renderedBorderColor)
                                                       : "transparent"
            readonly property real markerBorderWidth: markerLoader.item
                                                     ? ("markerShape" in markerLoader.item ? markerLoader.item.markerBorderWidth
                                                                                          : markerLoader.item.renderedBorderWidth)
                                                     : 0
            readonly property string label: markerLoader.item ? markerLoader.item.label : ""
            anchorPoint.x:  sourceItem.anchorPointX
            anchorPoint.y:  sourceItem.anchorPointY
            z:              QGroundControl.zOrderMapItems
            opacity:        _root.opacity

            property var rallyPointObject

            sourceItem: Loader {
                id: markerLoader
                readonly property real anchorPointX: item ? item.anchorPointX : 0
                readonly property real anchorPointY: item ? item.anchorPointY : 0
                sourceComponent: _root._styled ? styledMarker : stockMarker

                Component {
                    id: styledMarker
                    RallyPointMarker {
                        color: _root._styled ? _root.planStyle.lineColor : "transparent"
                        borderColor: _root._styled ? _root.planStyle.markerBorderColor : "transparent"
                        borderWidth: _root._styled ? _root.planStyle.markerBorderWidth : 0
                        textColor: _root._styled ? _root.planStyle.markerTextColor : "transparent"
                        checked: _editingLayer == _layerRally ? rallyPointObject === myRallyPointController.currentRallyPoint : false
                        interactive: _root.interactive
                        onClicked: {
                            if (mainWindow.allowViewSwitch()) {
                                myRallyPointController.currentRallyPoint = rallyPointObject
                            }
                        }
                    }
                }
                Component {
                    id: stockMarker
                    MissionItemIndexLabel {
                        id:                 itemIndexLabel
                        label:              qsTr("R", "rally point map item label")
                        checked:            _editingLayer == _layerRally ? rallyPointObject === myRallyPointController.currentRallyPoint : false
                        highlightSelected:  true
                        onClicked: {
                            if (mainWindow.allowViewSwitch()) {
                                myRallyPointController.currentRallyPoint = rallyPointObject
                            }
                        }
                    }
                }
            }
        }
    }

    // Add all rally points to the map
    Component {
        id: rallyPointsComponent

        Repeater {
            model: _rallyPoints

            delegate: Item {
                opacity:        _root.opacity
                property var _visuals: [ ]

                Component.onCompleted: {
                    var rallyPointIndicator = rallyPointComponent.createObject(map)
                    rallyPointIndicator.coordinate = Qt.binding(function() { return object.coordinate })
                    rallyPointIndicator.rallyPointObject = Qt.binding(function() { return object })
                    map.addMapItem(rallyPointIndicator)
                    _visuals.push(rallyPointIndicator)

                    var dragArea = dragAreaComponent.createObject(map, { "itemIndicator": rallyPointIndicator, "rallyPointObject": object })
                    _visuals.push(dragArea)
                }

                Component.onDestruction: {
                    for (var i=0; i<_visuals.length; i++) {
                        _visuals[i].destroy()
                    }
                    _visuals = [ ]
                }
            }
        }
    }

}
