import QtQuick

import QGroundControl
import QGroundControl.Controls

Item {
    id: rootItem
    objectName: "onScreenGimbalController"
    anchors.fill: parent

    required property bool cameraTrackingEnabled

    property var _activeVehicle: QGroundControl.multiVehicleManager.activeVehicle
    property var _gimbalController: _activeVehicle ? _activeVehicle.gimbalController : undefined
    property var _dragController
    property var _activeGimbal: _gimbalController ? _gimbalController.activeGimbal : undefined
    property bool _gimbalAvailable: _activeGimbal != undefined
    property var _gimbalControllerSettings: QGroundControl.settingsManager.gimbalControllerSettings
    property bool _shouldProcessClicks: _gimbalControllerSettings.enableOnScreenControl.value && _activeGimbal && !cameraTrackingEnabled ? true : false

    property real _mouseX: 0
    property real _mouseY: 0
    property real _dragStartNormX: 0
    property real _dragStartNormY: 0

    function _toNormX(mouseX) { return  ((mouseX / width)  * 2) - 1 }
    function _toNormY(mouseY) { return -((mouseY / height) * 2) + 1 }

    function mouseClicked(mouseX, mouseY) {
        if (!_shouldProcessClicks) {
            return
        }
        if (_gimbalControllerSettings.clickAndDrag.rawValue) {
            return
        }
        if (rootItem._gimbalAvailable) {
            _gimbalController.gimbalOnScreenControl(_toNormX(mouseX), _toNormY(mouseY), true, false, false)
        }
    }

    function mouseDragStart(mouseX, mouseY) {
        if (!_shouldProcessClicks) {
            return
        }
        if (!_gimbalControllerSettings.clickAndDrag.rawValue) {
            return
        }
        _mouseX = mouseX
        _mouseY = mouseY
        _dragStartNormX = _toNormX(mouseX)
        _dragStartNormY = _toNormY(mouseY)
        _dragController = _gimbalController
        sendRateTimer.start()
    }

    function mouseDragPositionChanged(mouseX, mouseY) {
        _mouseX = mouseX
        _mouseY = mouseY
    }

    function mouseDragEnd() {
        sendRateTimer.stop()
        _dragController = undefined
    }

    Timer {
        id: sendRateTimer
        objectName: "gimbalSendRateTimer"
        interval: 100
        repeat: true
        onTriggered: {
            if (!rootItem._dragController || rootItem._dragController !== rootItem._gimbalController) {
                rootItem.mouseDragEnd()
                return
            }
            if (rootItem._gimbalAvailable) {
                var dx = rootItem._toNormX(rootItem._mouseX) - rootItem._dragStartNormX
                var dy = rootItem._toNormY(rootItem._mouseY) - rootItem._dragStartNormY
                rootItem._dragController.gimbalOnScreenControl(dx, dy, false, true, true)
            }
        }
    }
}
