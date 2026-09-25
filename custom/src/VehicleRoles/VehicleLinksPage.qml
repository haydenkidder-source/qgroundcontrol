import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGC
import QGroundControl
import QGroundControl.Controls
import QGroundControl.AnalyzeView

AnalyzePage {
    id:                 vehicleLinksPage
    pageComponent:      pageComponent
    pageDescription:    qsTr("Each vehicle can have several named radio links (e.g. \"RFD900x\", " +
                              "\"Microhard 2450\", \"WFB-NG\"), any of which may or may not be " +
                              "connected right now. Pick an existing link configuration for a named " +
                              "link, or create a new one, then connect/disconnect it here. Vehicles " +
                              "and their sysids are assigned on the Vehicle Roles page.")

    property var _controller: VehicleRoleController
    property var _linkManager: QGroundControl.linkManager

    // Declared once at file scope (rather than per-row) so every nested Repeater delegate below
    // can reference the same "qgcPal" id - ids declared inside a delegate are local to that one
    // instance and are not visible from sibling rows or from the enclosing entry section.
    QGCPalette { id: qgcPal }

    // Recomputed whenever the link manager's configuration list changes (add/remove/rename), so
    // every row's picker stays in sync without each row re-scanning the list itself.
    property var _linkConfigNames: {
        const names = []
        const configs = _linkManager.linkConfigurations
        for (let i = 0; i < configs.count; i++) {
            if (!configs.get(i).dynamic) {
                names.push(configs.get(i).name)
            }
        }
        return names
    }

    readonly property string _unlinkedChoice:  qsTr("(unlinked)")
    readonly property string _createNewChoice: qsTr("Create New Link…")

    function _configForName(name) {
        if (!name) {
            return null
        }
        const configs = _linkManager.linkConfigurations
        for (let i = 0; i < configs.count; i++) {
            if (configs.get(i).name === name) {
                return configs.get(i)
            }
        }
        return null
    }

    function _openCreateLinkDialog(entryIndex, linkIndex, entryLabel, linkLabel) {
        const suggestedName = (entryLabel + " " + linkLabel).trim()
        const editingConfig = _linkManager.createConfiguration(
                    ScreenTools.isSerialAvailable ? LinkConfiguration.TypeSerial : LinkConfiguration.TypeUdp,
                    suggestedName)
        newLinkDialogFactory.open({ entryIndex: entryIndex, linkIndex: linkIndex, editingConfig: editingConfig })
    }

    Component {
        id: pageComponent

        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight

            QGCLabel {
                visible:    _controller.roleEntries.count === 0
                text:       qsTr("No vehicle roles assigned yet - add one on the Vehicle Roles page first.")
            }

            Repeater {
                model: _controller.roleEntries

                ColumnLayout {
                    id:                 entrySection
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelHeight / 2

                    property int  _entryIndex: index
                    property var  _entry:      object

                    Rectangle {
                        Layout.fillWidth:   true
                        height:             1
                        color:              qgcPal.windowShadeLight
                        visible:            _entryIndex > 0
                    }

                    RowLayout {
                        spacing: ScreenTools.defaultFontPixelWidth

                        QGCLabel {
                            font.bold:  true
                            text:       qsTr("sysid %1 · %2").arg(_entry.sysid).arg(_entry.role)
                        }

                        QGCLabel {
                            text:       _entry.name
                            visible:    _entry.name !== ""
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth:   true
                        Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2

                        Repeater {
                            model: _entry.links

                            RowLayout {
                                Layout.fillWidth:   true
                                spacing:            ScreenTools.defaultFontPixelWidth

                                property int  _linkIndex:      index
                                property var  _linkObject:     object
                                property var  _resolvedConfig: vehicleLinksPage._configForName(
                                                                    _linkObject.linkConfigName)
                                property bool _connected:      _resolvedConfig !== null && _resolvedConfig.linkActive

                                QGCLabel {
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 16
                                    text:                   _linkObject.label
                                }

                                QGCComboBox {
                                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 26
                                    model:                  [vehicleLinksPage._unlinkedChoice].concat(
                                                                 vehicleLinksPage._linkConfigNames).concat(
                                                                 [vehicleLinksPage._createNewChoice])
                                    currentIndex: {
                                        const configIndex = vehicleLinksPage._linkConfigNames.indexOf(
                                                                 _linkObject.linkConfigName)
                                        return configIndex >= 0 ? configIndex + 1 : 0
                                    }
                                    onActivated: (idx) => {
                                        const choice = model[idx]
                                        if (choice === vehicleLinksPage._createNewChoice) {
                                            vehicleLinksPage._openCreateLinkDialog(
                                                        _entryIndex, _linkIndex, _entry.role, _linkObject.label)
                                        } else if (choice === vehicleLinksPage._unlinkedChoice) {
                                            _controller.setLinkConfigName(_entryIndex, _linkIndex, "")
                                        } else {
                                            _controller.setLinkConfigName(_entryIndex, _linkIndex, choice)
                                        }
                                    }
                                }

                                QGCLabel {
                                    Layout.preferredWidth: ScreenTools.defaultFontPixelWidth * 12
                                    color:                  _connected ? qgcPal.colorGreen : qgcPal.colorGrey
                                    text:                   _connected ? qsTr("Connected") : qsTr("Disconnected")
                                }

                                QGCButton {
                                    text:       _connected ? qsTr("Disconnect") : qsTr("Connect")
                                    enabled:    _resolvedConfig !== null
                                    onClicked: {
                                        if (_connected) {
                                            _linkManager.disconnectLinkConfiguration(_resolvedConfig)
                                        } else {
                                            _linkManager.createConnectedLink(_resolvedConfig)
                                        }
                                    }
                                }

                                QGCButton {
                                    text:       qsTr("Remove")
                                    onClicked:  _controller.removeLink(_entryIndex, _linkIndex)
                                }
                            }
                        }

                        RowLayout {
                            spacing: ScreenTools.defaultFontPixelWidth

                            QGCTextField {
                                id:                     newLinkLabelField
                                Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 20
                                placeholderText:        qsTr("New link name (e.g. RFD900x)")
                            }

                            QGCButton {
                                text:       qsTr("Add Link")
                                enabled:    newLinkLabelField.text !== ""
                                onClicked: {
                                    _controller.addLink(_entryIndex, newLinkLabelField.text, "")
                                    newLinkLabelField.text = ""
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    QGCPopupDialogFactory {
        id: newLinkDialogFactory

        dialogComponent: newLinkDialogComponent
    }

    Component {
        id: newLinkDialogComponent

        QGCPopupDialog {
            title:                  qsTr("New Link Configuration")
            buttons:                Dialog.Save | Dialog.Cancel
            acceptButtonEnabled:    nameField.text !== ""

            property int entryIndex
            property int linkIndex
            property var editingConfig

            onAccepted: {
                linkSettingsLoader.item.saveSettings()
                editingConfig.name = nameField.text
                editingConfig.dynamic = false
                QGroundControl.linkManager.endCreateConfiguration(editingConfig)
                _controller.setLinkConfigName(entryIndex, linkIndex, editingConfig.name)
            }

            onRejected: QGroundControl.linkManager.cancelConfigurationEditing(editingConfig)

            ColumnLayout {
                spacing: ScreenTools.defaultFontPixelHeight / 2

                RowLayout {
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCLabel { text: qsTr("Name") }
                    QGCTextField {
                        id:                 nameField
                        Layout.fillWidth:   true
                        text:               editingConfig.name
                        placeholderText:    qsTr("Enter name")
                    }
                }

                QGCCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               qsTr("Automatically Connect on Start")
                    checked:            editingConfig.autoConnect
                    onCheckedChanged:   editingConfig.autoConnect = checked
                }

                LabelledComboBox {
                    label:                  qsTr("Type")
                    model:                  QGroundControl.linkManager.linkTypeStrings
                    Component.onCompleted:  comboBox.currentIndex = editingConfig.linkType

                    onActivated: (index) => {
                        if (index !== editingConfig.linkType) {
                            const name = nameField.text
                            editingConfig = QGroundControl.linkManager.createConfiguration(index, name)
                        }
                    }
                }

                Loader {
                    id:             linkSettingsLoader
                    source:         editingConfig && editingConfig.settingsURL ? editingConfig.settingsURL : ""
                    asynchronous:   true

                    property var subEditConfig:         editingConfig
                    property int _firstColumnWidth:     ScreenTools.defaultFontPixelWidth * 12
                    property int _secondColumnWidth:    ScreenTools.defaultFontPixelWidth * 30
                    property int _rowSpacing:           ScreenTools.defaultFontPixelHeight / 2
                    property int _colSpacing:           ScreenTools.defaultFontPixelWidth / 2

                    onStatusChanged: {
                        if (status === Loader.Error) {
                            console.warn("Failed to load link settings page:", source)
                        }
                    }
                }
            }
        }
    }
}
