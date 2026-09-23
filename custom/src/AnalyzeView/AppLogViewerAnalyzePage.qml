pragma ComponentBehavior: Bound

import QtQuick

import QGroundControl.AnalyzeView
import QGroundControl.AppSettings

AnalyzePage {
    id: root

    pageComponent: pageComponent
    pageDescription: qsTr("View and filter live application logs for diagnostics.")
    allowPopout: true

    Component {
        id: pageComponent

        AppLogging {
            width: root.availableWidth
            height: root.availableHeight
        }
    }
}
