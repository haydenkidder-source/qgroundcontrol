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
    pageDescription:    qsTr("Set up each vehicle and its radio links in one place. A vehicle is its MAVLink " +
                              "system ID (sysid), an ArduPilot type and an optional nickname; QGC reads the " +
                              "sysid from the vehicle itself, so adding it here only remembers which sysid is " +
                              "which vehicle for labels and tabs. Each vehicle can have several named links " +
                              "(e.g. \"RFD900x\", \"Microhard 2450\"). Pick an existing link configuration or " +
                              "create a new one, then connect or disconnect it here. Give every vehicle its own " +
                              "UDP port so the telemetry of two vehicles never shares a socket.")

    property var _controller:   VehicleRoleController
    property var _linkManager:  QGroundControl.linkManager
    property var _vehicles:     QGroundControl.multiVehicleManager.vehicles

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

    // Connected vehicles whose sysid has no saved entry yet, each with the type detected from its
    // own MAV_TYPE. Re-evaluated when a vehicle connects or goes away and when entries are added or
    // removed; an entry's sysid never changes, so those are the only inputs.
    property var _vehiclesToSetUp: {
        const saved = {}
        for (let i = 0; i < _controller.roleEntries.count; i++) {
            saved[_controller.roleEntries.get(i).sysid] = true
        }
        const result = []
        for (let i = 0; i < _vehicles.count; i++) {
            const vehicle = _vehicles.get(i)
            if (vehicle && !saved[vehicle.id]) {
                result.push({ sysid: vehicle.id, suggestedRole: _controller.suggestedRole(vehicle) })
            }
        }
        return result
    }

    readonly property string _unlinkedChoice:  qsTr("(unlinked)")
    readonly property string _createNewChoice: qsTr("Create New Link…")
    readonly property string _pickTypeText:    qsTr("Pick a type…")

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

    function _isVehicleConnected(sysid) {
        for (let i = 0; i < _vehicles.count; i++) {
            if (_vehicles.get(i).id === sysid) {
                return true
            }
        }
        return false
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

        QGCFlickable {
            objectName:         "vehicleLinksFlickable"
            width:              vehicleLinksPage.availableWidth
            height:             vehicleLinksPage.availableHeight
            contentWidth:       width
            contentHeight:      pageColumn.height + ScreenTools.defaultFontPixelHeight
            flickableDirection: Flickable.VerticalFlick

            ColumnLayout {
                id:         pageColumn
                width:      parent.width
                spacing:    ScreenTools.defaultFontPixelHeight

                QGCLabel {
                    objectName:         "noVehiclesConnectedHint"
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    visible:            _vehicles.count === 0
                    text:               qsTr("No vehicles connected.")
                }

                // One-click add for vehicles that are already talking to QGC but not remembered yet.
                ColumnLayout {
                    objectName:         "vehiclesToSetUp"
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelHeight / 2
                    visible:            _vehiclesToSetUp.length > 0

                    QGCLabel {
                        font.bold:  true
                        text:       qsTr("Connected vehicles not set up yet")
                    }

                    Repeater {
                        model: _vehiclesToSetUp

                        RowLayout {
                            id:                 setUpRow
                            required property var modelData
                            objectName:         "setUpRow_" + modelData.sysid
                            Layout.fillWidth:   true
                            Layout.leftMargin:  ScreenTools.defaultFontPixelWidth * 2
                            spacing:            ScreenTools.defaultFontPixelWidth

                            QGCLabel {
                                Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 10
                                text:                   qsTr("sysid %1").arg(setUpRow.modelData.sysid)
                            }

                            QGCComboBox {
                                id:             setUpRoleCombo
                                objectName:     "setUpRole_" + setUpRow.modelData.sysid
                                model:          _controller.availableRoles
                                currentIndex:   _controller.availableRoles.indexOf(setUpRow.modelData.suggestedRole)
                                displayText:    currentIndex === -1 ? vehicleLinksPage._pickTypeText : currentText
                            }

                            QGCTextField {
                                id:                     setUpNameField
                                objectName:             "setUpNickname_" + setUpRow.modelData.sysid
                                readonly property bool taken: _controller.isNicknameTaken(text, setUpRow.modelData.sysid)
                                Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 20
                                placeholderText:        qsTr("Nickname (optional)")
                            }

                            QGCButton {
                                objectName: "setUpAdd_" + setUpRow.modelData.sysid
                                text:       qsTr("Add Vehicle")
                                enabled:    setUpRoleCombo.currentIndex >= 0 && !setUpNameField.taken
                                onClicked:  _controller.addEntry(setUpRow.modelData.sysid, setUpRoleCombo.currentText,
                                                                 setUpNameField.text, 0)
                            }

                            QGCLabel {
                                visible:    setUpNameField.taken
                                color:      QGroundControl.globalPalette.warningText
                                text:       qsTr("Nickname already used by another vehicle")
                            }
                        }
                    }
                }

                QGCLabel {
                    objectName:         "noVehiclesSetUpHint"
                    Layout.fillWidth:   true
                    wrapMode:           Text.WordWrap
                    visible:            _controller.roleEntries.count === 0
                    text:               qsTr("No vehicles set up yet. Add each vehicle below, then give it a link.")
                }

                Repeater {
                    model: _controller.roleEntries

                    ColumnLayout {
                        id:                 entrySection
                        objectName:         "vehicleCard_" + _entry.sysid
                        Layout.fillWidth:   true
                        spacing:            ScreenTools.defaultFontPixelHeight / 2

                        property int  _entryIndex: index
                        property var  _entry:      object

                        Rectangle {
                            Layout.fillWidth:   true
                            height:             1
                            color:              qgcPal.colorGrey
                            opacity:            0.5
                        }

                        RowLayout {
                            Layout.fillWidth:   true
                            spacing:            ScreenTools.defaultFontPixelWidth

                            QGCLabel {
                                font.bold:  true
                                text:       qsTr("sysid %1").arg(_entry.sysid)
                            }

                            QGCComboBox {
                                objectName:     "vehicleRole_" + _entry.sysid
                                model:          _controller.availableRoles
                                currentIndex:   _controller.availableRoles.indexOf(_entry.role)
                                displayText:    currentIndex === -1 ? vehicleLinksPage._pickTypeText : currentText
                                onActivated:    (index) => _controller.setRole(_entryIndex, model[index])
                            }

                            QGCTextField {
                                id:                     entryNameField
                                objectName:             "vehicleNickname_" + _entry.sysid
                                readonly property bool taken: _controller.isNicknameTaken(text, _entry.sysid)
                                Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 20
                                text:                   _entry.name
                                placeholderText:        qsTr("Nickname (optional)")
                                onEditingFinished:      { if (!taken) { _controller.setName(_entryIndex, text) } }
                            }

                            QGCLabel {
                                color:  vehicleLinksPage._isVehicleConnected(_entry.sysid) ? qgcPal.colorGreen
                                                                                           : qgcPal.colorGrey
                                text:   vehicleLinksPage._isVehicleConnected(_entry.sysid) ? qsTr("Vehicle connected")
                                                                                           : qsTr("Vehicle not connected")
                            }

                            Item { Layout.fillWidth: true }

                            QGCButton {
                                objectName: "vehicleRemove_" + _entry.sysid
                                text:       qsTr("Remove Vehicle")
                                onClicked:  _controller.removeEntry(_entryIndex)
                            }
                        }

                        QGCLabel {
                            Layout.fillWidth:   true
                            wrapMode:           Text.WordWrap
                            visible:            entryNameField.taken
                            color:              QGroundControl.globalPalette.warningText
                            text:               qsTr("Nickname already used by another vehicle")
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
                                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 16
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
                                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 12
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
                                        objectName: "vehicleRemoveLink_" + _entry.sysid + "_" + _linkIndex
                                        text:       qsTr("Remove")
                                        onClicked:  _controller.removeLink(_entryIndex, _linkIndex)
                                    }
                                }
                            }

                            RowLayout {
                                spacing: ScreenTools.defaultFontPixelWidth

                                QGCTextField {
                                    id:                     newLinkLabelField
                                    objectName:             "vehicleNewLinkName_" + _entry.sysid
                                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 20
                                    placeholderText:        qsTr("New link name (e.g. RFD900x)")
                                }

                                QGCButton {
                                    objectName: "vehicleAddLink_" + _entry.sysid
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

                Rectangle {
                    Layout.fillWidth:   true
                    height:             1
                    color:              qgcPal.colorGrey
                            opacity:            0.5
                }

                // For vehicles that are not connected yet, e.g. one that has not been built or powered up.
                RowLayout {
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCLabel { text: qsTr("Add a vehicle:") }

                    QGCTextField {
                        id:                     newSysidField
                        objectName:             "newVehicleSysid"
                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 8
                        placeholderText:        qsTr("sysid")
                        validator:              IntValidator { bottom: 1; top: 255 }
                    }

                    QGCComboBox {
                        id:         newRoleCombo
                        objectName: "newVehicleRole"
                        model:      _controller.availableRoles
                    }

                    QGCTextField {
                        id:                     newNameField
                        objectName:             "newVehicleNickname"
                        readonly property bool taken: _controller.isNicknameTaken(text, parseInt(newSysidField.text) || 0)
                        Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 20
                        placeholderText:        qsTr("Nickname (optional)")
                    }

                    QGCButton {
                        objectName: "newVehicleAdd"
                        text:       qsTr("Add Vehicle")
                        enabled:    newSysidField.acceptableInput && !newNameField.taken
                        onClicked: {
                            _controller.addEntry(parseInt(newSysidField.text), newRoleCombo.currentText,
                                                 newNameField.text, 0)
                            newSysidField.text = ""
                            newNameField.text = ""
                        }
                    }

                    QGCLabel {
                        visible:    newNameField.taken
                        color:      QGroundControl.globalPalette.warningText
                        text:       qsTr("Nickname already used by another vehicle")
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
