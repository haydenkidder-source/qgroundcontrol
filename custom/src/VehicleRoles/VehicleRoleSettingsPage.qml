import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGC
import QGroundControl
import QGroundControl.Controls
import QGroundControl.AnalyzeView

AnalyzePage {
    id:                 vehicleRolesPage
    pageComponent:      pageComponent
    pageDescription:    qsTr("Assign each vehicle's MAVLink system ID (sysid) to one of the program's three " +
                              "vehicles, with an optional nickname. QGC already reads sysid automatically " +
                              "(the same ID Mission Planner uses to tell autopilots apart); this just " +
                              "remembers which sysid is which vehicle, for display purposes. The optional " +
                              "port field records that vehicle's assigned ground-station UDP port (see the " +
                              "field radio setup notes); \"Create Link\" is a one-time convenience that builds " +
                              "a matching autoconnect UDP link for you. Experienced users can still build or " +
                              "edit links directly under Settings > Comm Links at any time.")

    property var _controller: VehicleRoleController

    function _linkExistsForName(name) {
        const configs = QGroundControl.linkManager.linkConfigurations
        for (let i = 0; i < configs.count; i++) {
            if (configs.get(i).name === name) {
                return true
            }
        }
        return false
    }

    Component {
        id: pageComponent

        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight

            QGCLabel {
                text:       qsTr("Connected vehicles:")
                visible:    QGroundControl.multiVehicleManager.vehicles.count > 0
            }

            RowLayout {
                spacing: ScreenTools.defaultFontPixelWidth
                visible: QGroundControl.multiVehicleManager.vehicles.count > 0

                Repeater {
                    model: QGroundControl.multiVehicleManager.vehicles

                    QGCLabel {
                        text: qsTr("sysid %1").arg(object.id)
                    }
                }
            }

            QGCLabel {
                visible:    QGroundControl.multiVehicleManager.vehicles.count === 0
                text:       qsTr("No vehicles connected.")
            }

            QGCLabel {
                text: qsTr("Assigned roles:")
            }

            ColumnLayout {
                Layout.fillWidth: true

                Repeater {
                    model: _controller.roleEntries

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: ScreenTools.defaultFontPixelWidth

                        QGCLabel {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 10
                            text:                   qsTr("sysid %1").arg(object.sysid)
                        }

                        QGCComboBox {
                            model:          _controller.availableRoles
                            currentIndex:   _controller.availableRoles.indexOf(object.role)
                            onActivated:    (index) => _controller.setRole(index_, model[index])

                            // 'index' from the Repeater delegate is shadowed by ComboBox's own
                            // onActivated index parameter, so capture the row index separately.
                            property int index_: index
                        }

                        QGCTextField {
                            Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 20
                            text:                   object.name
                            placeholderText:        qsTr("Nickname (optional)")
                            onEditingFinished:      _controller.setName(index, text)
                        }

                        QGCTextField {
                            id:                     portField
                            Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 8
                            text:                   object.port > 0 ? object.port.toString() : ""
                            placeholderText:        qsTr("Port")
                            validator:              IntValidator { bottom: 0; top: 65535 }
                            onEditingFinished:      _controller.setPort(index, text === "" ? 0 : parseInt(text))
                        }

                        QGCButton {
                            text:       _linkExistsForName(object.role) ? qsTr("Link Exists") : qsTr("Create Link")
                            enabled:    object.port > 0 && !_linkExistsForName(object.role)
                            visible:    object.port > 0
                            onClicked:  _controller.createLinkForEntry(index)
                        }

                        QGCButton {
                            text:       qsTr("Remove")
                            onClicked:  _controller.removeEntry(index)
                        }
                    }
                }
            }

            QGCLabel {
                visible:    _controller.roleEntries.count === 0
                text:       qsTr("No roles assigned yet.")
            }

            RowLayout {
                spacing: ScreenTools.defaultFontPixelWidth

                QGCLabel { text: qsTr("Add:") }

                QGCTextField {
                    id:                     newSysidField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 8
                    placeholderText:        qsTr("sysid")
                    validator:              IntValidator { bottom: 1; top: 255 }
                }

                QGCComboBox {
                    id:     newRoleCombo
                    model:  _controller.availableRoles
                }

                QGCTextField {
                    id:                     newNameField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 20
                    placeholderText:        qsTr("Nickname (optional)")
                }

                QGCTextField {
                    id:                     newPortField
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 8
                    placeholderText:        qsTr("Port (optional)")
                    validator:              IntValidator { bottom: 0; top: 65535 }
                }

                QGCButton {
                    text:       qsTr("Add")
                    enabled:    newSysidField.acceptableInput
                    onClicked: {
                        _controller.addEntry(parseInt(newSysidField.text), newRoleCombo.currentText, newNameField.text,
                                              newPortField.text === "" ? 0 : parseInt(newPortField.text))
                        newSysidField.text = ""
                        newNameField.text = ""
                        newPortField.text = ""
                    }
                }
            }
        }
    }
}
