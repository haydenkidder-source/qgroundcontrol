import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QGC
import QGroundControl
import QGroundControl.Controls
import QGroundControl.PlanView

Rectangle {
    id: _root

    property var planMasterController
    property string statusHint: ""

    property real _controllerProgressPct: planMasterController.missionController.progressPct

    signal toolbarButtonClicked()

    width: parent.width
    height: ScreenTools.toolbarHeight
    color: qgcPal.toolbarBackground

    QGCPalette { id: qgcPal }

    /// Bottom single pixel divider
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "black"
        visible: qgcPal.globalTheme === QGCPalette.Light
    }

    QGCToolBarButton {
        id: qgcButton
        objectName: "toolbar_qgcLogo"
        height: parent.height
        icon.source: "/res/QGCLogoFull.svg"
        logo: true
        onClicked: mainWindow.showToolSelectDialog()
    }

    QGCFlickable {
        id: toolsFlickable
        anchors.bottomMargin: 1
        anchors.left: qgcButton.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        contentWidth: toolbarContents.width
        flickableDirection: Flickable.HorizontalFlick

        Row {
            id: toolbarContents
            height: toolsFlickable.height

            Row {
                id: planIndicator
                height: parent.height
                spacing: ScreenTools.defaultFontPixelWidth / 2
                leftPadding: ScreenTools.defaultFontPixelWidth
                visible: _planId !== 0 || _target !== null

                readonly property real _planId: planMasterController ? planMasterController.planVehicleId : 0
                readonly property var _target: planMasterController && !planMasterController.offline
                                               ? planMasterController.managerVehicle : null
                readonly property real _targetId: _target ? _target.id : 0
                readonly property bool _compact: !!_target && _planId === _targetId
                readonly property var _policy: QGroundControl.corePlugin ? QGroundControl.corePlugin.planStyle : null
                readonly property int _revision: _policy ? _policy.revision : 0

                function vehicleName(id) {
                    const revision = _revision
                    if (typeof COPController !== "undefined" && COPController && COPController.vehicles) {
                        for (let i = 0; i < COPController.vehicles.count; i++) {
                            const entry = COPController.vehicles.get(i)
                            if (entry && entry.sysid === id) {
                                return entry.label
                            }
                        }
                    }
                    return qsTr("Vehicle %1").arg(id)
                }

                function vehicleColor(id) {
                    const revision = _revision
                    const vehicleColor = _policy ? _policy.vehicleColor(id) : undefined
                    return vehicleColor && vehicleColor.valid ? vehicleColor : QGroundControl.globalPalette.mapMissionTrajectory
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: ScreenTools.defaultFontPixelHeight / 2
                    height: width
                    radius: width / 2
                    color: planSource.cueColor
                }
                QGCLabel {
                    id: planSource
                    objectName: "planToolbarPlanSource"
                    property color cueColor: planIndicator.vehicleColor(planIndicator._planId)
                    anchors.verticalCenter: parent.verticalCenter
                    font.bold: true
                    text: planIndicator._compact
                          ? qsTr("Editing plan for: %1").arg(planIndicator.vehicleName(planIndicator._planId))
                          : planIndicator._planId ? qsTr("Plan: %1").arg(planIndicator.vehicleName(planIndicator._planId))
                                                  : qsTr("Plan: not from a vehicle")
                }
                QGCLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: uploadTarget.visible
                    text: "·"
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: uploadTarget.visible
                    width: ScreenTools.defaultFontPixelHeight / 2
                    height: width
                    radius: width / 2
                    color: uploadTarget.cueColor
                }
                QGCLabel {
                    id: uploadTarget
                    objectName: "planToolbarUploadTarget"
                    property color cueColor: planIndicator.vehicleColor(planIndicator._targetId)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !!planIndicator._target && !planIndicator._compact
                    font.bold: true
                    text: planIndicator._target ? qsTr("Upload to: %1").arg(planIndicator.vehicleName(planIndicator._targetId)) : ""
                }
            }

            PlanToolBarIndicators {
                id: toolIndicators
                height: parent.height
                planMasterController: _root.planMasterController
                statusHint: _root.statusHint
                onToolbarButtonClicked: _root.toolbarButtonClicked()
            }
        }
    }

    // Small mission download progress bar
    Rectangle {
        id: progressBar
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        height: 4
        width: _controllerProgressPct * parent.width
        color: qgcPal.colorGreen
        visible: false

        onVisibleChanged: {
            if (visible) {
                largeProgressBar._userHide = false
            }
        }
    }

    // Large mission download progress bar
    Rectangle {
        id: largeProgressBar

        property bool _userHide: false
        property bool _showLargeProgress: progressBar.visible && !_userHide && qgcPal.globalTheme === QGCPalette.Light

        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: parent.height
        color: qgcPal.window
        visible: _showLargeProgress

        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: _controllerProgressPct * parent.width
            color: qgcPal.colorGreen
        }

        QGCLabel {
            anchors.centerIn: parent
            text: qsTr("Syncing Mission")
            font.pointSize: ScreenTools.largeFontPointSize
            visible: _controllerProgressPct !== 1
        }

        QGCLabel {
            anchors.centerIn: parent
            text: qsTr("Done")
            font.pointSize: ScreenTools.largeFontPointSize
            visible: _controllerProgressPct === 1
        }

        QGCLabel {
            property real _margin: ScreenTools.defaultFontPixelWidth / 2

            anchors.margins: _margin
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: qsTr("Click anywhere to hide")
        }

        MouseArea {
            anchors.fill: parent
            onClicked: largeProgressBar._userHide = true
        }

        Connections {
            target: QGroundControl.multiVehicleManager
            function onActiveVehicleChanged(activeVehicle) { largeProgressBar._userHide = false }
        }
    }

    Timer {
        id: resetProgressTimer
        interval: 3000
        onTriggered: progressBar.visible = false
    }

    // Progress bar
    Connections {
        target: planMasterController.missionController

        function onProgressPctChanged(progressPct) {
            if (progressPct === 1) {
                if (_root.visible) {
                    resetProgressTimer.start()
                } else {
                    progressBar.visible = false
                }
            } else if (progressPct > 0) {
                progressBar.visible = true
            }
        }
    }
}
