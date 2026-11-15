import QtQuick

import QGroundControl

QtObject {
    id: root

    // QML int is signed; real preserves the full quint32 system-id range.
    property real vehicleId: 0
    property bool supported: true

    readonly property var policy: QGroundControl.corePlugin.planStyle
    readonly property bool enabled: policy !== null && supported
    readonly property var vehicleColor: {
        if (!policy) {
            return undefined
        }
        // The registry may be attached after this context was created.
        const revision = policy.revision
        return revision >= 0 ? policy.vehicleColor(vehicleId) : undefined
    }
    readonly property color lineColor: vehicleColor && vehicleColor.valid
                                       ? vehicleColor : QGroundControl.globalPalette.mapMissionTrajectory
    readonly property real missionLineWidth: policy ? policy.missionLineWidth : 0
    readonly property real boundaryLineWidth: policy ? policy.boundaryLineWidth : 0
    readonly property var keepInDashPattern: policy ? policy.keepInDashPattern : []
    readonly property var keepOutDashPattern: policy ? policy.keepOutDashPattern : []
    readonly property var keepOutFillColor: policy ? policy.keepOutFillColor : undefined
    readonly property var markerBorderColor: policy ? policy.markerBorderColor : undefined
    readonly property real markerBorderWidth: policy ? policy.markerBorderWidth : 0
    readonly property var markerTextColor: policy ? policy.markerTextColor : undefined
    readonly property string rallyShape: policy ? policy.rallyShape : ""

    objectName: "planStyleContext"
}
