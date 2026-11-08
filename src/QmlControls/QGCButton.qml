import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

/// Standard push button control:
///     If there is both an icon and text the icon will be to the left of the text
///     If icon only, icon will be centered
Button {
    property bool primary: false
    property bool showBorder: true
    property real backRadius: ScreenTools.defaultBorderRadius
    property real heightFactor: small ? 0.2 : 0.3
    property string iconSource: ""
    property real fontWeight: Font.Normal // default for qml Text
    property real pointSize: small ? ScreenTools.smallFontPointSize : ScreenTools.defaultFontPointSize

    /// Compact variant for secondary actions in dense areas (indicator drop-downs, list headers).
    /// Only changes the defaults of heightFactor, pointSize and horizontal padding and drops the minimum button height
    /// (the minimum width still applies); each default can still be overridden explicitly.
    property bool small: false

    property alias wrapMode: text.wrapMode
    property alias horizontalAlignment: text.horizontalAlignment
    property color backgroundColor: primary ? qgcPal.accent : qgcPal.windowCard
    property alias textColor: text.color
    // Transparent means use upstream styling; COP supplies an explicit vehicle/selection color.
    property alias highlightColor: highlightRect.color

    id: control
    hoverEnabled: !ScreenTools.isMobile
    topPadding: _verticalPadding
    bottomPadding: _verticalPadding
    leftPadding: _horizontalPadding
    rightPadding: _horizontalPadding
    focusPolicy: Qt.ClickFocus
    font.family: ScreenTools.normalFontFamily
    text: ""

    property bool _showHighlight: enabled && (pressed || checked || (_hasCustomHighlight && highlighted))
    readonly property bool _hasCustomHighlight: highlightColor.a > 0
    readonly property color _highlightColor: _hasCustomHighlight ? highlightColor : qgcPal.accent
    readonly property real _highlightLuminance: 0.2126 * _linearChannel(highlightColor.r)
                                             + 0.7152 * _linearChannel(highlightColor.g)
                                             + 0.0722 * _linearChannel(highlightColor.b)
    property int _horizontalPadding: ScreenTools.defaultFontPixelWidth * (small ? 1 : 2)
    property int _textVerticalPadding: Math.round(ScreenTools.defaultFontPixelHeight * heightFactor)
    property real _textOnlyHeight: Math.max(small ? 0 : ScreenTools.implicitButtonHeight, text.implicitHeight + _textVerticalPadding * 2)
    property real _iconHeight: Math.round(_textOnlyHeight * 0.75)
    property int _verticalPadding: iconSource === "" ? _textVerticalPadding : Math.max(0, Math.round((_textOnlyHeight - _iconHeight) / 2))
    // Solid fills (primary or a caller supplied backgroundColor) keep their color and show state with an overlay instead
    property bool _solidFill: primary || !Qt.colorEqual(backgroundColor, qgcPal.windowCard)

    function _linearChannel(value: real): real {
        return value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4)
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: control.enabled }

    background: Rectangle {
        id: backRect
        radius: backRadius
        implicitWidth: ScreenTools.implicitButtonWidth
        implicitHeight: control.small ? control.implicitContentHeight + control.topPadding + control.bottomPadding : ScreenTools.implicitButtonHeight
        border.width: showBorder ? 1 : 0
        border.color: primary || _showHighlight || (control.enabled && control.hovered) ? control._highlightColor : qgcPal.borderSubtle
        color: !control._solidFill && control._showHighlight ? qgcPal.accentSubtle : control.backgroundColor

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: qgcPal.accentText
            visible: control._solidFill
            opacity: control._showHighlight ? 0.25 : (control.enabled && control.hovered ? 0.12 : 0)
        }

        Rectangle {
            id: highlightRect
            anchors.fill: parent
            anchors.margins: parent.border.width
            radius: Math.max(0, parent.radius - anchors.margins)
            color: "transparent"
            // Keep the controlled-vehicle cue as prominent as the fork's former solid fill.
            opacity: control._hasCustomHighlight && control._showHighlight ? 1
                     : (control._hasCustomHighlight && control.enabled && control.hovered ? 0.2 : 0)
        }
    }

    contentItem: RowLayout {
        spacing: ScreenTools.defaultFontPixelWidth

        QGCColoredImage {
            id: icon
            Layout.alignment: Qt.AlignHCenter
            source: control.iconSource
            height: _iconHeight
            width: height
            color: text.color
            fillMode: Image.PreserveAspectFit
            sourceSize.height: height
            visible: control.iconSource !== ""
        }

        QGCLabel {
            id: text
            Layout.alignment: Qt.AlignHCenter
            text: control.text
            font.pointSize: control.pointSize
            font.family: control.font.family
            font.weight: fontWeight
            // Vehicle colors are arbitrary: choose the higher-contrast foreground on the solid cue.
            color: control._hasCustomHighlight && control._showHighlight
                   ? (control._highlightLuminance > 0.179 ? "black" : "white")
                   : (primary ? qgcPal.accentText : qgcPal.textPrimary)
            visible: control.text !== ""
        }
    }
}
