pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGC
import QGroundControl
import QGroundControl.Controls

Rectangle {
    id: root
    implicitHeight: layout.implicitHeight
    color: QGroundControl.globalPalette.windowShade

    property var hostWindow
    readonly property var selected: COPController.selected
    readonly property var activeVehicle: QGroundControl.multiVehicleManager.activeVehicle
    readonly property bool controlling: selected && selected.connected && selected.vehicle === activeVehicle

    function select(sysid: int) {
        if (hostWindow && hostWindow.allowViewSwitch()) {
            COPController.selectVehicle(sysid)
            hostWindow.showFlyView()
        }
    }

    // Best-guess ArduPilot vehicle type from the connected vehicle's own MAV_TYPE, offered as the
    // default when a sysid has no role assigned yet - the operator can still override it.
    function suggestedRoleFor(vehicle) {
        if (!vehicle) {
            return ""
        }
        if (vehicle.rover) {
            return "Rover"
        }
        if (vehicle.sub) {
            return "Sub"
        }
        if (vehicle.multiRotor) {
            return "Copter"
        }
        if (vehicle.fixedWing || vehicle.vtol) {
            return "Plane"
        }
        return ""
    }

    ColumnLayout {
        id: layout
        width: parent.width
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Flickable {
                Layout.fillWidth: true
                implicitHeight: tabs.implicitHeight
                contentWidth: tabs.implicitWidth
                clip: true
                flickableDirection: Flickable.HorizontalFlick

                RowLayout {
                    id: tabs
                    QGCButton {
                        text: qsTr("COP")
                        highlighted: COPController.selectedSysid === 0
                        onClicked: root.select(0)
                    }
                    Repeater {
                        model: COPController.vehicles
                        RowLayout {
                            id: vehicleTab
                            required property var object
                            spacing: ScreenTools.defaultFontPixelWidth / 2
                            // Matches this vehicle's marker color on the map, so operators learn
                            // to associate a tab with its color at a glance.
                            Rectangle {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelHeight / 2
                                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight / 2
                                radius: width / 2
                                color: vehicleTab.object.color
                            }
                            QGCButton {
                                text: vehicleTab.object.label
                                highlighted: COPController.selectedSysid === vehicleTab.object.sysid
                                onClicked: root.select(vehicleTab.object.sysid)
                            }
                            QGCButton {
                                text: qsTr("Plan")
                                checkable: true
                                checked: vehicleTab.object.planOverlayVisible
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Show %1's mission, geofence and rally points on the COP map")
                                              .arg(vehicleTab.object.label)
                                onToggled: vehicleTab.object.planOverlayVisible = checked
                            }
                        }
                    }
                }
            }
            QGCButton {
                text: qsTr("Tools")
                onClicked: {
                    if (root.hostWindow) {
                        root.hostWindow.showToolSelectDialog()
                    }
                }
            }
        }
        RowLayout {
            visible: root.selected !== null || COPController.pendingSysid !== 0
            Layout.fillWidth: true
            QGCLabel {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: !root.selected ? "" : root.controlling
                      ? qsTr("%1 — active vehicle").arg(root.selected.label)
                      : root.selected.connected ? qsTr("%1 — connected, not active").arg(root.selected.label)
                                              : qsTr("%1 — disconnected, last received state").arg(root.selected.label)
            }
            QGCButton {
                text: qsTr("Name / role…")
                enabled: root.selected !== null
                onClicked: {
                    if (root.selected && root.hostWindow && root.hostWindow.allowViewSwitch()) {
                        editVehicleDialog.open({ sysid: root.selected.sysid,
                                                  suggestedRole: root.suggestedRoleFor(root.selected.vehicle) })
                    }
                }
            }
            QGCButton {
                primary: true
                text: COPController.pendingSysid === COPController.selectedSysid
                      ? qsTr("Waiting for connection…") : qsTr("Assume Control")
                enabled: root.selected !== null && !root.controlling
                         && COPController.pendingSysid !== COPController.selectedSysid
                onClicked: {
                    if (root.hostWindow && root.hostWindow.allowViewSwitch()) {
                        COPController.assumeControl()
                    }
                }
            }
            QGCButton {
                text: qsTr("Cancel pending control")
                visible: COPController.pendingSysid !== 0
                onClicked: COPController.cancelControl()
            }
        }
    }

    QGCPopupDialogFactory {
        id: editVehicleDialog
        dialogComponent: editVehicleComponent
    }

    Component {
        id: editVehicleComponent

        QGCPopupDialog {
            id: dialog
            required property int sysid
            property string suggestedRole: ""
            title: qsTr("Vehicle %1 — name and role").arg(sysid)
            buttons: Dialog.Save | Dialog.Cancel
            acceptButtonEnabled: roleCombo.currentIndex >= 0
            onAccepted: VehicleRoleController.addEntry(sysid, roleCombo.currentText, nickname.text, VehicleRoleController.portForSysid(sysid))

            ColumnLayout {
                width: Math.min(ScreenTools.defaultFontPixelWidth * 40, dialog.maxContentAvailableWidth)
                QGCLabel { text: qsTr("Nickname") }
                QGCTextField {
                    id: nickname
                    Layout.fillWidth: true
                    text: VehicleRoleController.nameForSysid(dialog.sysid)
                    placeholderText: qsTr("Nickname (optional)")
                }
                QGCLabel { text: qsTr("Role (vehicle type)") }
                QGCComboBox {
                    id: roleCombo
                    Layout.fillWidth: true
                    model: VehicleRoleController.availableRoles
                    // Prefer the already-saved role; for a not-yet-assigned vehicle, default to the
                    // type detected from its own MAV_TYPE rather than leaving nothing selected.
                    currentIndex: {
                        const savedRole = VehicleRoleController.roleForSysid(dialog.sysid)
                        const saved = VehicleRoleController.availableRoles.indexOf(savedRole)
                        return saved >= 0 ? saved : VehicleRoleController.availableRoles.indexOf(dialog.suggestedRole)
                    }
                }
            }
        }
    }

    Component.onCompleted: COPController.initialize(VehicleRoleController)
}
