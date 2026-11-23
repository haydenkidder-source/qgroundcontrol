import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.FactControls

QGCGroupBox {
    id: _root
    heading: qsTr("Links")

    property var _linkManager: QGroundControl.linkManager

    Repeater {
        model: _linkManager.linkConfigurations

        RowLayout {
            Layout.fillWidth:   true
            visible:            !object.dynamic

            QGCLabel {
                Layout.fillWidth:   true
                text:               object.name
            }
            QGCColoredImage {
                height:                 ScreenTools.minTouchPixels
                width:                  height
                sourceSize.height:      height
                fillMode:               Image.PreserveAspectFit
                mipmap:                 true
                smooth:                 true
                color:                  qgcPalEdit.textPrimary
                source:                 "/res/pencil.svg"
                enabled:                !object.link

                QGCPalette {
                    id: qgcPalEdit
                    colorGroupEnabled: parent.enabled
                }

                QGCMouseArea {
                    fillItem: parent
                    onClicked: {
                        var editingConfig = _linkManager.startConfigurationEditing(object)
                        linkDialogFactory.open({ editingConfig: editingConfig, originalConfig: object })
                    }
                }
            }
            QGCColoredImage {
                height:                 ScreenTools.minTouchPixels
                width:                  height
                sourceSize.height:      height
                fillMode:               Image.PreserveAspectFit
                mipmap:                 true
                smooth:                 true
                color:                  qgcPalDelete.textPrimary
                source:                 "/res/TrashDelete.svg"

                QGCPalette {
                    id: qgcPalDelete
                    colorGroupEnabled: parent.enabled
                }

                QGCMouseArea {
                    fillItem:   parent
                    onClicked:  QGroundControl.showMessageDialog(
                                    _root,
                                    qsTr("Delete Link"),
                                    qsTr("Are you sure you want to delete '%1'?").arg(object.name),
                                    Dialog.Ok | Dialog.Cancel,
                                    function () {
                                        _linkManager.removeConfiguration(object)
                                    })
                }
            }
            QGCButton {
                text:       object.linkActive ? qsTr("Disconnect") : qsTr("Connect")
                onClicked: {
                    if (object.linkActive) {
                        _linkManager.disconnectLinkConfiguration(object)
                    } else {
                        _linkManager.createConnectedLink(object)
                    }
                }
            }
        }
    }

    LabelledButton {
        objectName: "stockAddLink"
        label:      qsTr("Add New Link")
        buttonText: qsTr("Add")

        onClicked: {
            var editingConfig = _linkManager.createConfiguration(ScreenTools.isSerialAvailable ? LinkConfiguration.TypeSerial : LinkConfiguration.TypeUdp, "")
            linkDialogFactory.open({ editingConfig: editingConfig, originalConfig: null })
        }
    }

    QGCPopupDialogFactory {
        id: linkDialogFactory

        dialogComponent: linkDialogComponent
    }

    Component {
        id: linkDialogComponent

        QGCPopupDialog {
            id:                     linkDialog
            title:                  originalConfig ? qsTr("Edit Link") : qsTr("Add New Link")
            buttons:                Dialog.Save | Dialog.Cancel
            acceptButtonEnabled:    nameField.text !== "" && nameValid && linkSettingsLoader.status === Loader.Ready

            property var originalConfig
            property var editingConfig
            readonly property bool nameValid: {
                const configs = _linkManager.linkConfigurations
                // Track renames as well as changes to the configuration list.
                for (let i = 0; i < configs.count; i++) {
                    configs.get(i).name
                }
                return _linkManager.isConfigurationNameValid(nameField.text, originalConfig)
            }

            onAccepted: {
                if (!editingConfig || !linkSettingsLoader.item) {
                    preventClose = true
                    return
                }
                linkSettingsLoader.item.saveSettings()
                editingConfig.name = nameField.text
                if (originalConfig) {
                    linkSettingsLoader.active = false
                    preventClose = !_linkManager.endConfigurationEditing(originalConfig, editingConfig)
                    if (preventClose) {
                        linkSettingsLoader.active = true
                    } else {
                        editingConfig = null
                    }
                } else {
                    editingConfig.dynamic = false
                    preventClose = !_linkManager.endCreateConfiguration(editingConfig)
                    if (!preventClose) {
                        linkSettingsLoader.active = false
                        editingConfig = null
                    }
                }
            }

            onRejected: {
                const config = editingConfig
                linkSettingsLoader.active = false
                editingConfig = null
                Qt.callLater(function() { _linkManager.cancelConfigurationEditing(config) })
            }

            ColumnLayout {
                spacing: ScreenTools.defaultFontPixelHeight / 2

                RowLayout {
                    Layout.fillWidth:   true
                    spacing:            ScreenTools.defaultFontPixelWidth

                    QGCLabel { text: qsTr("Name") }
                    QGCTextField {
                        id:                 nameField
                        objectName:         "stockLinkName"
                        property var dialog: linkDialog
                        Layout.fillWidth:   true
                        text:               editingConfig ? editingConfig.name : ""
                        placeholderText:    qsTr("Enter name")
                    }
                }

                QGCLabel {
                    objectName: "stockLinkNameTaken"
                    visible: nameField.text !== "" && !nameValid
                    color: QGroundControl.globalPalette.errorText
                    text: qsTr("Name already in use")
                }

                QGCCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               qsTr("Automatically Connect on Start")
                    checked:            editingConfig ? editingConfig.autoConnect : false
                    onCheckedChanged:   { if (editingConfig) editingConfig.autoConnect = checked }
                }

                QGCCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               qsTr("High Latency")
                    checked:            editingConfig ? editingConfig.highLatency : false
                    onCheckedChanged:   { if (editingConfig) editingConfig.highLatency = checked }
                }

                LabelledComboBox {
                    objectName:             "stockLinkType"
                    label:                  qsTr("Type")
                    enabled:                originalConfig == null
                    model:                  _linkManager.linkTypeStrings
                    Component.onCompleted:  comboBox.currentIndex = editingConfig.linkType

                    onActivated: (index) => {
                        if (index !== editingConfig.linkType) {
                            var previousConfig = editingConfig
                            linkSettingsLoader.active = false
                            editingConfig = _linkManager.createConfiguration(index, nameField.text)
                            linkSettingsLoader.active = true
                            Qt.callLater(function() { _linkManager.cancelConfigurationEditing(previousConfig) })
                        }
                    }
                }

                Loader {
                    id:     linkSettingsLoader
                    objectName: "stockLinkSettingsLoader"
                    source: editingConfig && editingConfig.settingsURL ? editingConfig.settingsURL : ""
                    asynchronous: true

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
