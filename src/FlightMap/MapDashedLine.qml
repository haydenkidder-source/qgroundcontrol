import QtQuick
import QtQuick.Shapes

// Dashed polyline in item coordinates. Qt Location polygons, circles and lines can only draw solid borders.
// Keep the points within the item's bounds: Qt's dash stroker drops dashes outside them.
Shape {
    id: root

    property var points: []
    property color color: "white"
    property real lineWidth: 3
    required property var dashPattern   ///< Alternating dash and gap lengths, in multiples of lineWidth

    ShapePath {
        strokeColor: root.color
        strokeWidth: root.lineWidth
        strokeStyle: ShapePath.DashLine
        dashPattern: root.dashPattern
        fillColor: "transparent"

        PathPolyline { path: root.points }
    }
}
