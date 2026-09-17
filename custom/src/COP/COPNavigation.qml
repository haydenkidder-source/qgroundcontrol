pragma ComponentBehavior: Bound

import QtQuick
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
                        QGCButton {
                            required property var object
                            text: object.label
                            highlighted: COPController.selectedSysid === object.sysid
                            onClicked: root.select(object.sysid)
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

    Component.onCompleted: COPController.initialize(VehicleRoleController)
}
