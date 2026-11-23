#include "KMLHelper.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtXml/QDomDocument>

#include "KMLSchemaValidator.h"
#include "QGCLoggingCategory.h"
#include "StrictPolygon.h"

QGC_LOGGING_CATEGORY(KMLHelperLog, "Utilities.KMLHelper")

namespace KMLHelper
{
    QDomDocument _loadFile(const QString &kmlFile, QString &errorString);
    bool _parseCoordinateString(const QString &coordinatesString, QList<QGeoCoordinate> &coords, QString &errorString);
    void _filterVertices(QList<QGeoCoordinate> &vertices, double filterMeters, int minVertices);
    void _checkAltitudeMode(const QDomNode &geometryNode, const QString &geometryType, int index);

    constexpr const char *_errorPrefix = QT_TRANSLATE_NOOP("KMLHelper", "KML file load failed. %1");
}

QDomDocument KMLHelper::_loadFile(const QString &kmlFile, QString &errorString)
{
    errorString.clear();

    QFile file(kmlFile);
    if (!file.exists()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "File not found: %1").arg(kmlFile));
        return QDomDocument();
    }

    if (!file.open(QIODevice::ReadOnly)) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "Unable to open file: %1 error: %2").arg(kmlFile, file.errorString()));
        return QDomDocument();
    }

    QDomDocument doc;
    const QDomDocument::ParseResult result = doc.setContent(&file, QDomDocument::ParseOption::Default);
    if (!result) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "Unable to parse KML file: %1 error: %2 line: %3").arg(kmlFile).arg(result.errorMessage).arg(result.errorLine));
        return QDomDocument();
    }

    return doc;
}

bool KMLHelper::_parseCoordinateString(const QString &coordinatesString, QList<QGeoCoordinate> &coords, QString &errorString)
{
    coords.clear();
    const QString simplified = coordinatesString.simplified();
    if (simplified.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "Empty coordinates string"));
        return false;
    }

    const QStringList rgCoordinateStrings = simplified.split(' ');
    for (const QString &coordinateString : rgCoordinateStrings) {
        if (coordinateString.isEmpty()) {
            continue;
        }
        const QStringList rgValueStrings = coordinateString.split(',');
        if (rgValueStrings.size() < 2) {
            qCWarning(KMLHelperLog) << "Invalid coordinate format, expected lon,lat[,alt]:" << coordinateString;
            continue;
        }
        bool lonOk = false, latOk = false;
        const double lon = rgValueStrings[0].toDouble(&lonOk);
        const double lat = rgValueStrings[1].toDouble(&latOk);
        if (!lonOk || !latOk) {
            qCWarning(KMLHelperLog) << "Failed to parse coordinate values:" << coordinateString;
            continue;
        }
        if (lat < -90.0 || lat > 90.0) {
            qCWarning(KMLHelperLog) << "Latitude out of range [-90, 90]:" << lat << "in:" << coordinateString;
            continue;
        }
        if (lon < -180.0 || lon > 180.0) {
            qCWarning(KMLHelperLog) << "Longitude out of range [-180, 180]:" << lon << "in:" << coordinateString;
            continue;
        }
        double alt = 0.0;
        if (rgValueStrings.size() >= 3) {
            alt = rgValueStrings[2].toDouble();
        }
        coords.append(QGeoCoordinate(lat, lon, alt));
    }

    if (coords.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "No valid coordinates found"));
        return false;
    }
    return true;
}

void KMLHelper::_filterVertices(QList<QGeoCoordinate> &vertices, double filterMeters, int minVertices)
{
    if (filterMeters <= 0 || vertices.count() <= minVertices) {
        return;
    }

    int i = 0;
    while (i < (vertices.count() - 1)) {
        if ((vertices.count() > minVertices) && (vertices[i].distanceTo(vertices[i + 1]) < filterMeters)) {
            vertices.removeAt(i + 1);
        } else {
            i++;
        }
    }
}

void KMLHelper::_checkAltitudeMode(const QDomNode &geometryNode, const QString &geometryType, int index)
{
    // Validate altitudeMode using schema-derived rules
    // QGC treats all coordinates as absolute (AMSL), so warn if a different mode is specified
    const QDomNode altModeNode = geometryNode.namedItem("altitudeMode");
    if (!altModeNode.isNull()) {
        const QString mode = altModeNode.toElement().text();
        if (mode.isEmpty()) {
            return;
        }
        const auto *validator = KMLSchemaValidator::instance();
        const QString location = QStringLiteral("(line %1)").arg(altModeNode.lineNumber());
        if (!validator->isValidEnumValue("altitudeModeEnumType", mode)) {
            qCWarning(KMLHelperLog) << geometryType << index << location << "has invalid altitudeMode:" << mode
                                    << "- valid values are:" << validator->validEnumValues("altitudeModeEnumType").join(", ");
        } else if (mode != "absolute") {
            qCWarning(KMLHelperLog) << geometryType << index << location << "uses altitudeMode:" << mode
                                    << "- QGC will treat coordinates as absolute (AMSL)";
        }
    }
}

ShapeFileHelper::ShapeType KMLHelper::determineShapeType(const QString &kmlFile, QString &errorString)
{
    using ShapeType = ShapeFileHelper::ShapeType;

    const QDomDocument domDocument = KMLHelper::_loadFile(kmlFile, errorString);
    if (!errorString.isEmpty()) {
        return ShapeType::Error;
    }

    const QDomNodeList rgNodesPolygon = domDocument.elementsByTagName("Polygon");
    if (!rgNodesPolygon.isEmpty()) {
        return ShapeType::Polygon;
    }

    const QDomNodeList rgNodesLineString = domDocument.elementsByTagName("LineString");
    if (!rgNodesLineString.isEmpty()) {
        return ShapeType::Polyline;
    }

    const QDomNodeList rgNodesPoint = domDocument.elementsByTagName("Point");
    if (!rgNodesPoint.isEmpty()) {
        return ShapeType::Point;
    }

    errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "No supported type found in KML file."));
    return ShapeType::Error;
}

int KMLHelper::getEntityCount(const QString &kmlFile, QString &errorString)
{
    const QDomDocument domDocument = KMLHelper::_loadFile(kmlFile, errorString);
    if (!errorString.isEmpty()) {
        return 0;
    }

    int count = 0;
    count += domDocument.elementsByTagName("Polygon").count();
    count += domDocument.elementsByTagName("LineString").count();
    count += domDocument.elementsByTagName("Point").count();
    return count;
}

bool KMLHelper::loadStrictPolygonsFromFile(const QString& fileName, QList<QList<QGeoCoordinate>>& polygons,
                                           QString& errorString)
{
    polygons.clear();
    errorString.clear();
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        errorString = QStringLiteral("Unable to open KML file: %1").arg(file.errorString());
        return false;
    }
    if (file.size() == 0) {
        errorString = QStringLiteral("No polygon geometry found");
        return false;
    }
    QDomDocument document;
    const auto result = document.setContent(&file, QDomDocument::ParseOption::Default);
    if (!result) {
        errorString = QStringLiteral("Unable to parse KML: %1 line %2").arg(result.errorMessage).arg(result.errorLine);
        return false;
    }

    QList<QList<QGeoCoordinate>> staged;
    int placemarkNumber = 0;
    int polygonNumber = 0;
    const QStringList otherGeometry = {QStringLiteral("Point"),
                                       QStringLiteral("LineString"),
                                       QStringLiteral("LinearRing"),
                                       QStringLiteral("Model"),
                                       QStringLiteral("Track"),
                                       QStringLiteral("MultiTrack"),
                                       QStringLiteral("GeometryCollection"),
                                       QStringLiteral("MultiPoint"),
                                       QStringLiteral("MultiLineString"),
                                       QStringLiteral("MultiPolygon")};
    std::function<bool(const QDomNode&, QString)> visit;
    visit = [&](const QDomNode& node, QString placemark) {
        if (!node.isElement()) {
            return true;
        }
        const QString tag = node.nodeName().section(QLatin1Char(':'), -1);
        if (tag == QLatin1String("Placemark")) {
            ++placemarkNumber;
            placemark = QStringLiteral("placemark %1").arg(placemarkNumber);
            const QString name = node.namedItem(QStringLiteral("name")).toElement().text();
            if (!name.isEmpty()) {
                placemark += QStringLiteral(" (%1)").arg(name);
            }
        }
        if (tag == QLatin1String("Polygon")) {
            ++polygonNumber;
            const QString location = QStringLiteral("%1 polygon %2 line %3")
                                         .arg(placemark.isEmpty() ? QStringLiteral("KML") : placemark)
                                         .arg(polygonNumber)
                                         .arg(node.lineNumber());
            const auto childNamed = [](const QDomNode& parent, const QString& name) {
                for (QDomNode child = parent.firstChild(); !child.isNull(); child = child.nextSibling()) {
                    if (child.isElement() && child.nodeName().section(QLatin1Char(':'), -1) == name) {
                        return child;
                    }
                }
                return QDomNode();
            };
            std::function<bool(const QDomNode&)> checkPolygonChildren;
            checkPolygonChildren = [&](const QDomNode& parent) {
                for (QDomNode child = parent.firstChild(); !child.isNull(); child = child.nextSibling()) {
                    if (!child.isElement()) {
                        continue;
                    }
                    const QString childTag = child.nodeName().section(QLatin1Char(':'), -1);
                    if (childTag == QLatin1String("innerBoundaryIs")) {
                        errorString = location + QStringLiteral(": innerBoundaryIs hole is not supported");
                        return false;
                    }
                    if (childTag != QLatin1String("LinearRing") && otherGeometry.contains(childTag)) {
                        errorString = QStringLiteral("%1 line %2: unsupported geometry %3")
                                          .arg(location)
                                          .arg(child.lineNumber())
                                          .arg(childTag);
                        return false;
                    }
                    if (!checkPolygonChildren(child)) {
                        return false;
                    }
                }
                return true;
            };
            if (!checkPolygonChildren(node)) {
                return false;
            }
            const QDomNode outer = childNamed(node, QStringLiteral("outerBoundaryIs"));
            const QDomNode linearRing = childNamed(outer, QStringLiteral("LinearRing"));
            const QDomNode coordinates = childNamed(linearRing, QStringLiteral("coordinates"));
            const QString contents = coordinates.toElement().text().simplified();
            if (coordinates.isNull() || contents.isEmpty()) {
                errorString = location + QStringLiteral(": polygon has no coordinates");
                return false;
            }
            QList<QGeoCoordinate> ring;
            const QStringList tuples = contents.split(QLatin1Char(' '));
            for (qsizetype i = 0; i < tuples.size(); ++i) {
                const QStringList values = tuples[i].split(QLatin1Char(','));
                bool lonOk = false;
                bool latOk = false;
                bool altOk = true;
                const double lon = values.value(0).toDouble(&lonOk);
                const double lat = values.value(1).toDouble(&latOk);
                double altitude = 0;
                if (values.size() == 3) {
                    altitude = values[2].toDouble(&altOk);
                }
                if (values.size() < 2 || values.size() > 3 || !lonOk || !latOk || !altOk || !std::isfinite(altitude) ||
                    !StrictPolygon::validPosition(lat, lon)) {
                    errorString = QStringLiteral("%1 coordinates line %2 vertex %3: invalid coordinate tuple %4")
                                      .arg(location)
                                      .arg(coordinates.lineNumber())
                                      .arg(i + 1)
                                      .arg(tuples[i]);
                    return false;
                }
                // The polygon model stores horizontal positions; altitude is validated but discarded.
                ring.append(QGeoCoordinate(lat, lon));
            }
            QString reason;
            if (!StrictPolygon::normalize(ring, reason)) {
                errorString = QStringLiteral("%1 ring: %2").arg(location, reason);
                return false;
            }
            staged.append(ring);
            return true;
        }
        if (!placemark.isEmpty() && otherGeometry.contains(tag)) {
            errorString =
                QStringLiteral("%1 line %2: unsupported geometry %3").arg(placemark).arg(node.lineNumber()).arg(tag);
            return false;
        }
        for (QDomNode child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (!visit(child, placemark)) {
                return false;
            }
        }
        return true;
    };
    if (!visit(document.documentElement(), QString())) {
        return false;
    }
    if (staged.isEmpty()) {
        errorString = QStringLiteral("No polygon geometry found");
        return false;
    }
    polygons = std::move(staged);
    return true;
}

bool KMLHelper::loadPolygonsFromFile(const QString &kmlFile, QList<QList<QGeoCoordinate>> &polygons, QString &errorString, double filterMeters)
{
    errorString.clear();
    polygons.clear();

    const QDomDocument domDocument = KMLHelper::_loadFile(kmlFile, errorString);
    if (!errorString.isEmpty()) {
        return false;
    }

    const QDomNodeList rgNodes = domDocument.elementsByTagName("Polygon");
    if (rgNodes.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "Unable to find Polygon node in KML"));
        return false;
    }

    for (int nodeIdx = 0; nodeIdx < rgNodes.count(); nodeIdx++) {
        const QDomNode polygonNode = rgNodes.item(nodeIdx);
        _checkAltitudeMode(polygonNode, "Polygon", nodeIdx);

        const QDomNode coordinatesNode = polygonNode.namedItem("outerBoundaryIs").namedItem("LinearRing").namedItem("coordinates");
        if (coordinatesNode.isNull()) {
            qCWarning(KMLHelperLog) << "Polygon" << nodeIdx << QStringLiteral("(line %1)").arg(polygonNode.lineNumber())
                                    << "missing coordinates node, skipping";
            continue;
        }

        QList<QGeoCoordinate> rgCoords;
        if (!_parseCoordinateString(coordinatesNode.toElement().text(), rgCoords, errorString)) {
            qCWarning(KMLHelperLog) << "Polygon" << nodeIdx << QStringLiteral("(line %1)").arg(coordinatesNode.lineNumber())
                                    << "failed to parse coordinates:" << errorString;
            errorString.clear();
            continue;
        }

        if (rgCoords.count() < 3) {
            qCWarning(KMLHelperLog) << "Polygon" << nodeIdx << QStringLiteral("(line %1)").arg(polygonNode.lineNumber())
                                    << "has fewer than 3 vertices, skipping";
            continue;
        }

        // Remove duplicate closing vertex (KML polygons repeat first vertex at end)
        if (rgCoords.count() > 3 && rgCoords.first().latitude() == rgCoords.last().latitude() &&
            rgCoords.first().longitude() == rgCoords.last().longitude()) {
            rgCoords.removeLast();
        }

        // Determine winding, reverse if needed. QGC wants clockwise winding
        double sum = 0;
        for (int i = 0; i < rgCoords.count(); i++) {
            const QGeoCoordinate &coord1 = rgCoords[i];
            const QGeoCoordinate &coord2 = rgCoords[(i + 1) % rgCoords.count()];
            sum += (coord2.longitude() - coord1.longitude()) * (coord2.latitude() + coord1.latitude());
        }
        if (sum < 0.0) {
            std::reverse(rgCoords.begin(), rgCoords.end());
        }

        _filterVertices(rgCoords, filterMeters, 3);
        polygons.append(rgCoords);
    }

    if (polygons.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "No valid polygons found in KML file"));
        return false;
    }

    return true;
}

bool KMLHelper::loadPolylinesFromFile(const QString &kmlFile, QList<QList<QGeoCoordinate>> &polylines, QString &errorString, double filterMeters)
{
    errorString.clear();
    polylines.clear();

    const QDomDocument domDocument = KMLHelper::_loadFile(kmlFile, errorString);
    if (!errorString.isEmpty()) {
        return false;
    }

    const QDomNodeList rgNodes = domDocument.elementsByTagName("LineString");
    if (rgNodes.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "Unable to find LineString node in KML"));
        return false;
    }

    for (int nodeIdx = 0; nodeIdx < rgNodes.count(); nodeIdx++) {
        const QDomNode lineStringNode = rgNodes.item(nodeIdx);
        _checkAltitudeMode(lineStringNode, "LineString", nodeIdx);

        const QDomNode coordinatesNode = lineStringNode.namedItem("coordinates");
        if (coordinatesNode.isNull()) {
            qCWarning(KMLHelperLog) << "LineString" << nodeIdx << QStringLiteral("(line %1)").arg(lineStringNode.lineNumber())
                                    << "missing coordinates node, skipping";
            continue;
        }

        QList<QGeoCoordinate> rgCoords;
        if (!_parseCoordinateString(coordinatesNode.toElement().text(), rgCoords, errorString)) {
            qCWarning(KMLHelperLog) << "LineString" << nodeIdx << QStringLiteral("(line %1)").arg(coordinatesNode.lineNumber())
                                    << "failed to parse coordinates:" << errorString;
            errorString.clear();
            continue;
        }

        if (rgCoords.count() < 2) {
            qCWarning(KMLHelperLog) << "LineString" << nodeIdx << QStringLiteral("(line %1)").arg(lineStringNode.lineNumber())
                                    << "has fewer than 2 vertices, skipping";
            continue;
        }

        _filterVertices(rgCoords, filterMeters, 2);
        polylines.append(rgCoords);
    }

    if (polylines.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "No valid polylines found in KML file"));
        return false;
    }

    return true;
}

bool KMLHelper::loadPointsFromFile(const QString &kmlFile, QList<QGeoCoordinate> &points, QString &errorString)
{
    errorString.clear();
    points.clear();

    const QDomDocument domDocument = KMLHelper::_loadFile(kmlFile, errorString);
    if (!errorString.isEmpty()) {
        return false;
    }

    const QDomNodeList rgNodes = domDocument.elementsByTagName("Point");
    if (rgNodes.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "Unable to find Point node in KML"));
        return false;
    }

    for (int nodeIdx = 0; nodeIdx < rgNodes.count(); nodeIdx++) {
        const QDomNode pointNode = rgNodes.item(nodeIdx);
        _checkAltitudeMode(pointNode, "Point", nodeIdx);

        const QDomNode coordinatesNode = pointNode.namedItem("coordinates");
        if (coordinatesNode.isNull()) {
            qCWarning(KMLHelperLog) << "Point" << nodeIdx << QStringLiteral("(line %1)").arg(pointNode.lineNumber())
                                    << "missing coordinates node, skipping";
            continue;
        }

        QList<QGeoCoordinate> coords;
        if (!_parseCoordinateString(coordinatesNode.toElement().text(), coords, errorString)) {
            qCWarning(KMLHelperLog) << "Point" << nodeIdx << QStringLiteral("(line %1)").arg(coordinatesNode.lineNumber())
                                    << "failed to parse coordinates:" << errorString;
            errorString.clear();
            continue;
        }

        if (!coords.isEmpty()) {
            points.append(coords.first());
        }
    }

    if (points.isEmpty()) {
        errorString = QCoreApplication::translate("KMLHelper", _errorPrefix).arg(QCoreApplication::translate("KML", "No valid points found in KML file"));
        return false;
    }

    return true;
}
