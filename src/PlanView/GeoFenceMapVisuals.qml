import QtQuick
import QtQuick.Controls
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FlightMap

/// GeoFence map visuals
Item {
    id: _root
    z: QGroundControl.zOrderMapItems

    property var    planStyle: null
    readonly property bool _styled: planStyle && planStyle.enabled

    property var    map
    property var    myGeoFenceController
    property bool   interactive:            false   ///< true: user can interact with items
    property bool   planView:               false   ///< true: visuals showing in plan view
    property var    homePosition

    property var    _breachReturnPointComponent
    property var    _breachReturnDragComponent
    property var    _paramCircleFenceComponent
    property var    _polygons:                  myGeoFenceController.polygons
    property var    _circles:                   myGeoFenceController.circles
    property color  _borderColor:               "orange"
    property int    _borderWidthInclusion:      2
    property int    _borderWidthExclusion:      0
    property color  _interiorColorExclusion:    "orange"
    property color  _interiorColorInclusion:    "transparent"
    property real   _interiorOpacityExclusion:  0.2 * opacity
    property real   _interiorOpacityInclusion:  1 * opacity

    function addPolygon(inclusionPolygon) {
        // Initial polygon is inset to take 2/3rds space
        var rect = Qt.rect(map.centerViewport.x, map.centerViewport.y, map.centerViewport.width, map.centerViewport.height)
        rect.x += (rect.width * 0.25) / 2
        rect.y += (rect.height * 0.25) / 2
        rect.width *= 0.75
        rect.height *= 0.75

        var centerCoord =       map.toCoordinate(Qt.point(rect.x + (rect.width / 2), rect.y + (rect.height / 2)),   false /* clipToViewPort */)
        var topLeftCoord =      map.toCoordinate(Qt.point(rect.x, rect.y),                                          false /* clipToViewPort */)
        var topRightCoord =     map.toCoordinate(Qt.point(rect.x + rect.width, rect.y),                             false /* clipToViewPort */)
        var bottomLeftCoord =   map.toCoordinate(Qt.point(rect.x, rect.y + rect.height),                            false /* clipToViewPort */)
        var bottomRightCoord =  map.toCoordinate(Qt.point(rect.x + rect.width, rect.y + rect.height),               false /* clipToViewPort */)

        // Initial polygon has max width and height of 3000 meters
        var halfWidthMeters =   Math.min(topLeftCoord.distanceTo(topRightCoord), 3000) / 2
        var halfHeightMeters =  Math.min(topLeftCoord.distanceTo(bottomLeftCoord), 3000) / 2
        topLeftCoord =      centerCoord.atDistanceAndAzimuth(halfWidthMeters, -90).atDistanceAndAzimuth(halfHeightMeters, 0)
        topRightCoord =     centerCoord.atDistanceAndAzimuth(halfWidthMeters, 90).atDistanceAndAzimuth(halfHeightMeters, 0)
        bottomLeftCoord =   centerCoord.atDistanceAndAzimuth(halfWidthMeters, -90).atDistanceAndAzimuth(halfHeightMeters, 180)
        bottomRightCoord =  centerCoord.atDistanceAndAzimuth(halfWidthMeters, 90).atDistanceAndAzimuth(halfHeightMeters, 180)

        console.log(map.center)
        console.log(topLeftCoord)
        console.log(bottomRightCoord)

        if (inclusionPolygon) {
            myGeoFenceController.addInclusion(topLeftCoord, bottomRightCoord)
        } else {
            myGeoFenceController.addExclusion(topLeftCoord, bottomRightCoord)
        }
    }

    Component.onCompleted: {
        _breachReturnPointComponent = breachReturnPointComponent.createObject(map)
        map.addMapItem(_breachReturnPointComponent)
        _breachReturnDragComponent = breachReturnDragComponent.createObject(map, { "itemIndicator": _breachReturnPointComponent })
        _paramCircleFenceComponent = paramCircleFenceComponent.createObject(map)
        map.addMapItem(_paramCircleFenceComponent)
    }

    Component.onDestruction: {
        _breachReturnPointComponent.destroy()
        _breachReturnDragComponent.destroy()
        _paramCircleFenceComponent.destroy()
    }

    // By default the parent for Instantiator.delegate item is the Instatiator itself. By there is a bug
    // in Qt which will cause a crash if this delete item has Menu item within it. Since the Menu item
    // doesn't like having a non-visual item as parent. This is likely related to hybrid QQuickWidtget+QML
    // Hence Qt folks are going to care. In order to workaround you have to parent the item to _root Item instead.
    Instantiator {
        model: _polygons

        delegate : QGCMapPolygonVisuals {
            parent:             _root
            mapControl:         map
            planStyle:          _root.planStyle
            boundaryKind:       "polygon"
            inclusion:          object.inclusion
            boundaryOpacity:    _root.opacity
            mapPolygon:         object
            borderWidth:        object.inclusion ? _borderWidthInclusion : _borderWidthExclusion
            borderColor:        _borderColor
            interiorColor:      object.inclusion ? _interiorColorInclusion : _interiorColorExclusion
            interiorOpacity:    object.inclusion ? _interiorOpacityInclusion : _interiorOpacityExclusion
            interactive:        _root.interactive && mapPolygon && mapPolygon.interactive
        }
    }

    Instantiator {
        model: _circles

        delegate : QGCMapCircleVisuals {
            parent:             _root
            mapControl:         map
            planStyle:          _root.planStyle
            boundaryKind:       "circle"
            inclusion:          object.inclusion
            boundaryOpacity:    _root.opacity
            mapCircle:          object
            borderWidth:        object.inclusion ? _borderWidthInclusion : _borderWidthExclusion
            borderColor:        _borderColor
            interiorColor:      object.inclusion ? _interiorColorInclusion : _interiorColorExclusion
            interiorOpacity:    object.inclusion ? _interiorOpacityInclusion : _interiorOpacityExclusion
            interactive:         _root.interactive && mapCircle && mapCircle.interactive
        }
    }

    // Circular geofence specified from parameter
    Component {
        id: paramCircleFenceComponent

        MapCircle {
            id: parameterFill
            objectName: "planFenceFill"
            readonly property real vehicleId: _root.planStyle ? _root.planStyle.vehicleId : 0
            readonly property string boundaryKind: "parameterCircle"
            readonly property bool inclusion: true
            color:          _interiorColorInclusion
            opacity:        _interiorOpacityInclusion
            border.color:   _borderColor
            border.width:   _root._styled ? 0 : _borderWidthInclusion
            center:         homePosition
            radius:         _radius
            visible:        homePosition.isValid && _radius > 0

            property real _radius: myGeoFenceController.paramCircularFence

            Loader {
                active: _root._styled
                sourceComponent: MapBoundaryOutline {
                    objectName: "planKeepInOutline"
                    readonly property real vehicleId: _root.planStyle ? _root.planStyle.vehicleId : 0
                    readonly property string boundaryKind: "parameterCircle"
                    mapControl: _root.map
                    vertices: circleVertices(parameterFill.center, parameterFill.radius)
                    lineColor: _root._styled ? _root.planStyle.lineColor : "transparent"
                    lineWidth: _root._styled ? _root.planStyle.boundaryLineWidth : 0
                    dashPattern: _root._styled ? _root.planStyle.keepInDashPattern : []
                    opacity: _root.opacity
                    visible: parameterFill.visible
                    z: parameterFill.z + 1
                    coalesceProjection: true
                    Component.onCompleted: _root.map.addMapItem(this)
                    Component.onDestruction: {
                        if (_root.map) {
                            _root.map.removeMapItem(this)
                        }
                    }
                }
            }
        }
    }

    Component {
        id: breachReturnDragComponent

        MissionItemIndicatorDrag {
            mapControl:     map
            itemCoordinate: myGeoFenceController.breachReturnPoint
            visible:        _root.interactive

            onItemCoordinateChanged: myGeoFenceController.breachReturnPoint = itemCoordinate
        }
    }


    // Breach return point
    Component {
        id: breachReturnPointComponent

        MapQuickItem {
            objectName: "planBreachReturnPoint"
            readonly property real vehicleId: _root.planStyle ? _root.planStyle.vehicleId : 0
            readonly property color markerColor: sourceItem.renderedBodyColor
            readonly property color markerBorderColor: sourceItem.renderedBorderColor
            readonly property real markerBorderWidth: sourceItem.renderedBorderWidth
            readonly property color markerTextColor: sourceItem.renderedTextColor
            readonly property string label: sourceItem.label
            anchorPoint.x:  sourceItem.anchorPointX
            anchorPoint.y:  sourceItem.anchorPointY
            z:              QGroundControl.zOrderMapItems
            coordinate:     myGeoFenceController.breachReturnPoint
            opacity:        _root.opacity

            sourceItem: MissionItemIndexLabel {
                planStyle:  _root.planStyle
                label:      qsTr("B", "Breach Return Point item indicator")
                checked:    true
            }
        }
    }
}
