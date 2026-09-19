// Custom builds can override this file to add custom guided actions.

import QtQml

import QGroundControl

QtObject {
    id: _root
    readonly property int actionCustomButton: _guidedController.customActionStart + 0
    readonly property string customButtonTitle: qsTr("Custom")
    readonly property string customButtonMessage: qsTr("Example of a custom action.")

    // Rover manual takeover (Decision 1, pure Design B - no fail-open bypass; the rover's own
    // link-loss failsafe and BendyRuler obstacle avoidance in Auto already cover the emergency
    // cases a bypass would exist for). Deliberately does NOT check multi-GCS control permission
    // (gcsControlStatusFlags_TakeoverAllowed) - that answers "which ground station commands this
    // vehicle," a different problem from this single-GCS program's "should this operator drive
    // right now."
    readonly property int actionAssumeRoverControl: _guidedController.customActionStart + 1
    readonly property int actionReturnToAuto: _guidedController.customActionStart + 2
    readonly property string assumeRoverControlTitle: qsTr("Assume Rover Control")
    readonly property string assumeRoverControlMessage: qsTr("Switch the rover to manual driving control?")
    readonly property string returnToAutoTitle: qsTr("Return to Auto")
    readonly property string returnToAutoMessage: qsTr("Return the rover to autonomous Auto mode?")

    // Looked up directly rather than relying on ambient _guidedController/_activeVehicle scoping,
    // since this object is a nested child of GuidedActionsController.qml, not a standalone file
    // that declares its own "property var _guidedController: globals.guidedControllerFlyView".
    readonly property var _activeVehicle: QGroundControl.multiVehicleManager.activeVehicle

    property var _confirmationVehicle: null
    property var _confirmationDialog: null
    property int _confirmationAction: -1

    readonly property Timer _assumeRoverControlTimer: Timer {
        interval: 3000
        repeat: false

        property var _vehicle: null

        function _reset() {
            stop()
            _vehicle = null
        }

        function _warn() {
            const message = qsTr("The change to Manual mode was not confirmed. " +
                                 "Verify the rover's actual state before assuming manual control.")
            QGroundControl.corePlugin.operatorNotification(qsTr("Vehicle %1: %2").arg(_vehicle ? _vehicle.id : "?").arg(message))
            _reset()
            QGroundControl.showMessageDialog(mainWindow, _root.assumeRoverControlTitle, message)
        }

        onTriggered: _warn()

        readonly property Connections _modeConnection: Connections {
            target: _root._assumeRoverControlTimer._vehicle

            function onFlightModeChanged(flightMode) {
                if (target && !target.vehicleLinkManager.communicationLost && flightMode === "Manual") {
                    _root._assumeRoverControlTimer._reset()
                }
            }
        }
    }

    readonly property Timer _returnToAutoTimer: Timer {
        interval: 3000
        repeat: false

        property var _vehicle: null

        function _reset() {
            stop()
            _vehicle = null
        }

        function _warn() {
            const message = qsTr("The change to Auto mode was not confirmed. " +
                                 "Verify the rover's actual state before assuming autonomous control has resumed.")
            QGroundControl.corePlugin.operatorNotification(qsTr("Vehicle %1: %2").arg(_vehicle ? _vehicle.id : "?").arg(message))
            _reset()
            QGroundControl.showMessageDialog(mainWindow, _root.returnToAutoTitle, message)
        }

        onTriggered: _warn()

        readonly property Connections _modeConnection: Connections {
            target: _root._returnToAutoTimer._vehicle

            function onFlightModeChanged(flightMode) {
                if (target && !target.vehicleLinkManager.communicationLost && flightMode === "Auto") {
                    _root._returnToAutoTimer._reset()
                }
            }
        }
    }

    on_ActiveVehicleChanged: {
        if (_confirmationDialog) {
            if (_confirmationDialog.action === _confirmationAction) {
                _confirmationDialog.confirmCancelled()
            }
            _confirmationDialog = null
        }
        _confirmationVehicle = null
        // Changing vehicles cannot confirm an outstanding request on the previous rover.
        if (_assumeRoverControlTimer && _assumeRoverControlTimer.running) {
            _assumeRoverControlTimer._warn()
        }
        if (_returnToAutoTimer && _returnToAutoTimer.running) {
            _returnToAutoTimer._warn()
        }
    }

    function customConfirmAction(actionCode, actionData, mapIndicator, confirmDialog) {
        switch (actionCode) {
        case actionCustomButton:
            confirmDialog.hideTrigger = true
            confirmDialog.title = customButtonTitle
            confirmDialog.message = customButtonMessage
            break
        case actionAssumeRoverControl:
            confirmDialog.hideTrigger = true
            confirmDialog.title = assumeRoverControlTitle
            confirmDialog.message = assumeRoverControlMessage
            break
        case actionReturnToAuto:
            confirmDialog.hideTrigger = true
            confirmDialog.title = returnToAutoTitle
            confirmDialog.message = returnToAutoMessage
            break
        default:
            return false // false = action not handled here
        }

        if (actionCode === actionAssumeRoverControl || actionCode === actionReturnToAuto) {
            _confirmationVehicle = _activeVehicle
            _confirmationDialog = confirmDialog
            _confirmationAction = actionCode
        } else {
            _confirmationVehicle = null
            _confirmationDialog = null
        }
        return true // true = action handled here
    }

    function customExecuteAction(actionCode, actionData, sliderOutputValue, optionCheckedode) {
        if (actionCode === actionAssumeRoverControl || actionCode === actionReturnToAuto) {
            const targetMatches = _confirmationVehicle && _confirmationVehicle === _activeVehicle
                && _confirmationAction === actionCode
            _confirmationVehicle = null
            _confirmationDialog = null
            if (!targetMatches) {
                return true
            }
        }
        switch (actionCode) {
        case actionCustomButton:
            QGroundControl.showMessageDialog(mainWindow, "Custom Action", "Custom action executed.")
            break
        case actionAssumeRoverControl:
            if (!_activeVehicle) {
                break
            }
            if (!_activeVehicle.rover) {
                QGroundControl.showMessageDialog(mainWindow, assumeRoverControlTitle, qsTr("Active vehicle is not a rover."))
                break
            }
            if (_activeVehicle.vehicleLinkManager.communicationLost) {
                QGroundControl.showMessageDialog(mainWindow, assumeRoverControlTitle, qsTr("Cannot assume control: no telemetry from the rover."))
                break
            }
            if (_activeVehicle.flightModes.indexOf("Manual") === -1) {
                QGroundControl.showMessageDialog(mainWindow, assumeRoverControlTitle, qsTr("Rover does not report a Manual flight mode."))
                break
            }
            _returnToAutoTimer._reset()
            _assumeRoverControlTimer._reset()
            _assumeRoverControlTimer._vehicle = _activeVehicle
            // Watch before sending so even an immediate confirmation is observed.
            _assumeRoverControlTimer.start()
            _activeVehicle.flightMode = "Manual"
            break
        case actionReturnToAuto:
            if (!_activeVehicle) {
                break
            }
            if (!_activeVehicle.rover) {
                QGroundControl.showMessageDialog(mainWindow, returnToAutoTitle, qsTr("Active vehicle is not a rover."))
                break
            }
            if (_activeVehicle.vehicleLinkManager.communicationLost) {
                QGroundControl.showMessageDialog(mainWindow, returnToAutoTitle, qsTr("Cannot return to Auto: no telemetry from the rover."))
                break
            }
            if (_activeVehicle.flightModes.indexOf("Auto") === -1) {
                QGroundControl.showMessageDialog(mainWindow, returnToAutoTitle, qsTr("Rover does not report an Auto flight mode."))
                break
            }
            _assumeRoverControlTimer._reset()
            _returnToAutoTimer._reset()
            _returnToAutoTimer._vehicle = _activeVehicle
            _returnToAutoTimer.start()
            _activeVehicle.flightMode = "Auto"
            break
        default:
            return false // false = action not handled here
        }

        return true // true = action handled here
    }
}
