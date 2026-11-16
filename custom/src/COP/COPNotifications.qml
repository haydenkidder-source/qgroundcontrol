pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

import QGC
import QGroundControl
import QGroundControl.Controls

Rectangle {
    id: root
    objectName: "copNotifications"
    implicitWidth: ScreenTools.defaultFontPixelWidth * 64
    implicitHeight: ((COPController.messages.length > 0 || _serialNoticeCount > 0) && !_minimized)
                    ? notificationLayout.implicitHeight + ScreenTools.defaultFontPixelWidth * 2 : 0
    width: implicitWidth
    height: implicitHeight
    clip: true
    color: QGroundControl.globalPalette.windowCard
    border.color: (COPController.unacknowledged || _serialNoticeCount > 0)
                  ? QGroundControl.globalPalette.colorOrange : QGroundControl.globalPalette.windowPanel
    border.width: ScreenTools.defaultFontPixelWidth / 3
    property var  hostWindow
    property bool _minimized:         false
    property int  _serialNoticeCount: 0

    function refreshSerialNotices() {
        let pending = 0
        const notices = QGroundControl.linkManager.serialNotices
        for (let i = 0; i < notices.count; ++i) {
            const notice = notices.get(i)
            if (notice && notice.active && !notice.acknowledged) {
                ++pending
            }
        }
        const hadPending = _serialNoticeCount > 0
        _serialNoticeCount = pending
        if (pending > 0) {
            _minimized = false
            minimizeTimer.stop()
        } else if (hadPending && !COPController.unacknowledged && COPController.messages.length > 0) {
            minimizeTimer.restart()
        }
    }

    function runSerialAction(noticeId: string, action: string) {
        if (action === "Dismiss") {
            QGroundControl.linkManager.dismissSerialNotice(noticeId)
        } else if (hostWindow && hostWindow.allowViewSwitch()) {
            if (action === "OpenCommLinks") {
                hostWindow.showSettingsTool("Comm Links")
            } else if (action === "OpenGnssReceiver") {
                hostWindow.showSettingsTool("GNSS Receiver")
            }
        }
    }

    Connections {
        target: QGroundControl.linkManager
        function onSerialNoticesChanged() {
            root.refreshSerialNotices()
        }
        function onSerialNoticeRaised() {
            root.refreshSerialNotices()
            COPController.alert()
        }
    }

    Connections {
        target: COPController
        function onMessagesChanged() {
            if (COPController.unacknowledged) {
                root._minimized = false
                minimizeTimer.stop()
            }
        }
    }

    Timer {
        id: minimizeTimer
        objectName: "copNotificationMinimizeTimer"
        interval: 10000
        onTriggered: {
            if (root._serialNoticeCount === 0 && !COPController.unacknowledged) {
                root._minimized = true
            }
        }
    }

    ColumnLayout {
        id: notificationLayout
        anchors.fill: parent
        anchors.margins: ScreenTools.defaultFontPixelWidth
        RowLayout {
            Layout.fillWidth: true
            QGCLabel {
                Layout.fillWidth: true
                elide: Text.ElideRight
                objectName: "copNotificationHeading"
                text: qsTr("Notifications (%1)").arg(COPController.messages.length + root._serialNoticeCount)
            }
            QGCButton {
                objectName: "copNotificationAcknowledge"
                text: qsTr("Acknowledge")
                enabled: COPController.unacknowledged
                onClicked: {
                    COPController.acknowledge()
                    if (root._serialNoticeCount === 0) {
                        minimizeTimer.restart()
                    }
                }
            }
        }
        QGCFlickable {
            id:                     serialNoticesView
            objectName:             "copSerialNoticesView"
            Layout.fillWidth:       true
            Layout.preferredHeight: Math.min(serialColumn.implicitHeight, ScreenTools.defaultFontPixelHeight * 12)
            visible:                root._serialNoticeCount > 0
            contentHeight:          serialColumn.implicitHeight
            flickableDirection:     Flickable.VerticalFlick
            clip:                   true

            Column {
                id:      serialColumn
                width:   serialNoticesView.width
                spacing: ScreenTools.defaultFontPixelHeight / 2

                Repeater {
                    model: QGroundControl.linkManager.serialNotices
                    Column {
                        id: serialRow
                        required property var object
                        objectName: "copSerialNotice_" + object.id
                        width:      serialColumn.width
                        visible:    object.active && !object.acknowledged
                        spacing:    ScreenTools.defaultFontPixelHeight / 4

                        QGCLabel {
                            objectName: "copSerialNoticeText_" + serialRow.object.id
                            width:      serialRow.width
                            wrapMode:   Text.Wrap
                            textFormat: Text.PlainText
                            text:       serialRow.object.text
                        }
                        Flow {
                            width:   serialRow.width
                            spacing: ScreenTools.defaultFontPixelWidth
                            Repeater {
                                model: serialRow.object.actions
                                QGCButton {
                                    required property string modelData
                                    objectName: "copSerialAction_" + serialRow.object.id + "_" + modelData
                                    text: modelData === "OpenCommLinks" ? qsTr("Open Comm Links")
                                          : modelData === "OpenGnssReceiver" ? qsTr("Open GNSS Receiver")
                                          : qsTr("Dismiss")
                                    onClicked: root.runSerialAction(serialRow.object.id, modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
        QGCLabel {
            Layout.fillWidth: true
            visible: COPController.droppedMessages > 0
            wrapMode: Text.WordWrap
            text: qsTr("%1 older notifications omitted").arg(COPController.droppedMessages)
        }
        ListView {
            objectName: "copNotificationHistory"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            Layout.preferredHeight: Math.min(contentHeight, ScreenTools.defaultFontPixelHeight * 5)
            clip: true
            spacing: ScreenTools.defaultFontPixelHeight / 4
            model: COPController.messages
            delegate: QGCLabel {
                required property var modelData
                width: ListView.view.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: Qt.formatTime(modelData.time, "hh:mm:ss") + "  " + modelData.text
            }
        }
    }

    Component.onCompleted: refreshSerialNotices()
}
