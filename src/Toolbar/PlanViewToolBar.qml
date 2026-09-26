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
    width: parent.width
    height: ScreenTools.toolbarHeight
    color: qgcPal.toolbarBackground

    property var planMasterController
    property bool showRallyPointsHelp: false

    signal toolbarButtonClicked()

    property var _activeVehicle: QGroundControl.multiVehicleManager.activeVehicle
    property real _controllerProgressPct: planMasterController.missionController.progressPct

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

    // Makes it painfully obvious which vehicle's plan is being edited - critical in a multi-vehicle
    // fleet, since PlanView otherwise silently follows whichever vehicle is active.
    RowLayout {
        id: activeVehicleIndicator
        anchors.left: qgcButton.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: ScreenTools.defaultFontPixelWidth
        spacing: ScreenTools.defaultFontPixelWidth / 2
        visible: _activeVehicle !== null

        // COPController is this fork's only source of a vehicle's assigned color; falls back to
        // null (drawn as neutral gray below) until COP has seen this vehicle.
        property var _copVehicle: {
            if (!_activeVehicle) {
                return null
            }
            for (let i = 0; i < COPController.vehicles.count; i++) {
                const vehicle = COPController.vehicles.get(i)
                if (vehicle.vehicle === _activeVehicle) {
                    return vehicle
                }
            }
            return null
        }

        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: ScreenTools.defaultFontPixelHeight / 2
            Layout.preferredHeight: ScreenTools.defaultFontPixelHeight / 2
            radius: width / 2
            color: activeVehicleIndicator._copVehicle ? activeVehicleIndicator._copVehicle.color : qgcPal.colorGrey
        }
        QGCLabel {
            Layout.alignment: Qt.AlignVCenter
            font.bold: true
            // visible: false above doesn't stop this binding from evaluating, so _activeVehicle
            // must still be null-checked even though the label is hidden once it's gone.
            text: qsTr("Editing plan for: %1").arg(activeVehicleIndicator._copVehicle
                                                    ? activeVehicleIndicator._copVehicle.label
                                                    : (_activeVehicle ? _activeVehicle.vehicleTypeString : ""))
        }
    }

    QGCFlickable {
        id: toolsFlickable
        anchors.bottomMargin: 1
        anchors.left: activeVehicleIndicator.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        contentWidth: toolIndicators.width
        flickableDirection: Flickable.HorizontalFlick

        PlanToolBarIndicators {
            id: toolIndicators
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            planMasterController: _root.planMasterController
            showRallyPointsHelp: _root.showRallyPointsHelp
            onToolbarButtonClicked: _root.toolbarButtonClicked()
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
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: parent.height
        color: qgcPal.window
        visible: _showLargeProgress

        property bool _userHide: false
        property bool _showLargeProgress: progressBar.visible && !_userHide && qgcPal.globalTheme === QGCPalette.Light

        Connections {
            target: QGroundControl.multiVehicleManager
            function onActiveVehicleChanged(activeVehicle) { largeProgressBar._userHide = false }
        }

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
            anchors.margins: _margin
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: qsTr("Click anywhere to hide")

            property real _margin: ScreenTools.defaultFontPixelWidth / 2
        }

        MouseArea {
            anchors.fill: parent
            onClicked: largeProgressBar._userHide = true
        }
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

    Timer {
        id: resetProgressTimer
        interval: 3000
        onTriggered: progressBar.visible = false
    }
}
