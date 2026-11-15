import QtQuick
import QtLocation
import QtPositioning

import QGroundControl
import QGroundControl.Controls

/// Home position map visual for MissionSettingsItem
MissionItemMapVisualBase {
    id: control

    indicatorComponent: homeIndicatorComponent

    Component {
        id: homeIndicatorComponent

        MapQuickItem {
            objectName: "planHomeMarker"
            readonly property real vehicleId: control.planStyle ? control.planStyle.vehicleId : 0
            property alias renderedBorderColor: homeAccent.border.color
            coordinate: control._missionItem.coordinate
            visible: control._missionItem.specifiesCoordinate
            z: QGroundControl.zOrderMapItems
            opacity: control.opacity
            anchorPoint.x: _homeImage.width / 2
            anchorPoint.y: _homeImage.height / 2

            sourceItem: Image {
                id: _homeImage
                source: "qrc:///qmlimages/MapHome.svg"

                property real _smallRadiusRaw: Math.ceil((ScreenTools.defaultFontPixelHeight * ScreenTools.smallFontPointRatio) / 2)
                property real _smallRadius:    _smallRadiusRaw + ((_smallRadiusRaw % 2 == 0) ? 1 : 0)

                width: _smallRadius * 2
                height: width
                sourceSize.width: width
                sourceSize.height: height
                fillMode: Image.PreserveAspectFit

                Rectangle {
                    id: homeAccent
                    objectName: "planHomeAccent"
                    readonly property real vehicleId: control.planStyle ? control.planStyle.vehicleId : 0
                    property alias renderedBorderColor: homeAccent.border.color

                    anchors.centerIn: parent
                    width: parent.width + border.width * 4
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.color: control.planStyle && control.planStyle.enabled ? control.planStyle.lineColor : "transparent"
                    border.width: control.planStyle && control.planStyle.enabled ? control.planStyle.markerBorderWidth : 0
                    visible: control.planStyle && control.planStyle.enabled
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: if (control.interactive) control.clicked(control._missionItem.sequenceNumber)
                }
            }
        }
    }
}
