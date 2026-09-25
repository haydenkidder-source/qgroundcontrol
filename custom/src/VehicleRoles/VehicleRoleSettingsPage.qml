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
    pageDescription:    qsTr("Assign each vehicle's MAVLink system ID (sysid) to its ArduPilot vehicle type " +
                              "(role), with an optional nickname. QGC already reads sysid automatically " +
                              "(the same ID Mission Planner uses to tell autopilots apart); this just " +
                              "remembers which sysid is which vehicle, grouped by type, for display purposes. " +
                              "The optional port field records that vehicle's assigned ground-station UDP " +
                              "port (see the field radio setup notes); \"Create Link\" is a one-time " +
                              "convenience that builds a matching autoconnect UDP link for you. Experienced " +
                              "users can still build or edit links directly under Settings > Comm Links at " +
                              "any time.")

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

    // Returns [{ index, entry }] for every roleEntries row currently assigned the given type,
    // preserving the underlying model index each row needs for setRole/setName/setPort/removeEntry.
    // Re-evaluated whenever roleEntries' count or any entry's role changes, since both are read here.
    function _entriesForRole(role) {
        const result = []
        for (let i = 0; i < _controller.roleEntries.count; i++) {
            const entry = _controller.roleEntries.get(i)
            if (entry.role === role) {
                result.push({ index: i, entry: entry })
            }
        }
        return result
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
                text: qsTr("Assigned vehicles, by type:")
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: ScreenTools.defaultFontPixelHeight

                Repeater {
                    model: _controller.availableRoles

                    ColumnLayout {
                        id: roleSection
                        required property string modelData
                        readonly property var entries: vehicleRolesPage._entriesForRole(modelData)
                        Layout.fillWidth: true
                        visible: entries.length > 0

                        QGCLabel {
                            text: roleSection.modelData
                            font.bold: true
                        }

                        Repeater {
                            model: roleSection.entries

                            RowLayout {
                                id: entryRow
                                required property var modelData
                                readonly property var object: modelData.entry
                                readonly property int rowIndex: modelData.index
                                Layout.fillWidth: true
                                Layout.leftMargin: ScreenTools.defaultFontPixelWidth * 2
                                spacing: ScreenTools.defaultFontPixelWidth

                                QGCLabel {
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 10
                                    text:                   qsTr("sysid %1").arg(entryRow.object.sysid)
                                }

                                QGCComboBox {
                                    model:          _controller.availableRoles
                                    currentIndex:   _controller.availableRoles.indexOf(entryRow.object.role)
                                    onActivated:    (index) => _controller.setRole(entryRow.rowIndex, model[index])
                                }

                                QGCTextField {
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 20
                                    text:                   entryRow.object.name
                                    placeholderText:        qsTr("Nickname (optional)")
                                    onEditingFinished:      _controller.setName(entryRow.rowIndex, text)
                                }

                                QGCTextField {
                                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 8
                                    text:                   entryRow.object.port > 0 ? entryRow.object.port.toString() : ""
                                    placeholderText:        qsTr("Port")
                                    validator:              IntValidator { bottom: 0; top: 65535 }
                                    onEditingFinished:      _controller.setPort(entryRow.rowIndex,
                                                                                text === "" ? 0 : parseInt(text))
                                }

                                QGCButton {
                                    text:       _linkExistsForName(entryRow.object.role) ? qsTr("Link Exists") : qsTr("Create Link")
                                    enabled:    entryRow.object.port > 0 && !_linkExistsForName(entryRow.object.role)
                                    visible:    entryRow.object.port > 0
                                    onClicked:  _controller.createLinkForEntry(entryRow.rowIndex)
                                }

                                QGCButton {
                                    text:       qsTr("Remove")
                                    onClicked:  _controller.removeEntry(entryRow.rowIndex)
                                }
                            }
                        }
                    }
                }

                // Entries loaded with a role that's no longer recognized (e.g. an old call-sign
                // save) land here with an empty role until the operator picks a real type above.
                ColumnLayout {
                    id: unassignedSection
                    readonly property var entries: vehicleRolesPage._entriesForRole("")
                    Layout.fillWidth: true
                    visible: entries.length > 0

                    QGCLabel {
                        text: qsTr("Unassigned")
                        font.bold: true
                    }

                    Repeater {
                        model: unassignedSection.entries

                        RowLayout {
                            id: unassignedRow
                            required property var modelData
                            readonly property var object: modelData.entry
                            readonly property int rowIndex: modelData.index
                            Layout.fillWidth: true
                            Layout.leftMargin: ScreenTools.defaultFontPixelWidth * 2
                            spacing: ScreenTools.defaultFontPixelWidth

                            QGCLabel {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 10
                                text:                   qsTr("sysid %1").arg(unassignedRow.object.sysid)
                            }

                            QGCComboBox {
                                model:          _controller.availableRoles
                                currentIndex:   -1
                                displayText:    currentIndex === -1 ? qsTr("Pick a type…") : currentText
                                onActivated:    (index) => _controller.setRole(unassignedRow.rowIndex, model[index])
                            }

                            QGCTextField {
                                Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 20
                                text:                   unassignedRow.object.name
                                placeholderText:        qsTr("Nickname (optional)")
                                onEditingFinished:      _controller.setName(unassignedRow.rowIndex, text)
                            }

                            QGCButton {
                                text:       qsTr("Remove")
                                onClicked:  _controller.removeEntry(unassignedRow.rowIndex)
                            }
                        }
                    }
                }
            }

            QGCLabel {
                visible:    _controller.roleEntries.count === 0
                text:       qsTr("No vehicles assigned yet.")
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
