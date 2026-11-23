#include "ShapeTest.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTextStream>

#include "KMLDomDocument.h"
#include "KMLSchemaValidator.h"
#include "ShapeFileHelper.h"
#include "shapefil.h"

namespace {

bool loadFirstPolygon(const QString& file, QList<QGeoCoordinate>& vertices, QString& errorString, double filterMeters)
{
    QList<QList<QGeoCoordinate>> polygons;
    if (!ShapeFileHelper::loadPolygonsFromFile(file, polygons, errorString, filterMeters)) {
        return false;
    }
    if (polygons.isEmpty()) {
        errorString = QStringLiteral("No polygons found");
        return false;
    }
    vertices = polygons.first();
    return true;
}

bool loadFirstPolyline(const QString& file, QList<QGeoCoordinate>& vertices, QString& errorString, double filterMeters)
{
    QList<QList<QGeoCoordinate>> polylines;
    if (!ShapeFileHelper::loadPolylinesFromFile(file, polylines, errorString, filterMeters)) {
        return false;
    }
    if (polylines.isEmpty()) {
        errorString = QStringLiteral("No polylines found");
        return false;
    }
    vertices = polylines.first();
    return true;
}

}  // namespace

QString ShapeTest::_copyRes(const QString& dirPath, const QString& name)
{
    const QString dstPath = QDir(dirPath).filePath(name);
    (void)QFile::remove(dstPath);
    const QString resPath = QStringLiteral(":/unittest/%1").arg(name);
    (void)QFile(resPath).copy(dstPath);
    return dstPath;
}

void ShapeTest::_writePrjFile(const QString& path, const QString& content)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << content;
    }
}

QString ShapeTest::_writeKmlFile(const QString& dirPath, const QString& name, const QString& content)
{
    const QString path = QDir(dirPath).filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << content;
    }
    return path;
}

void ShapeTest::_testLoadPolylineFromSHP()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "pline.shp");
    (void)_copyRes(tempDir.path(), "pline.dbf");
    (void)_copyRes(tempDir.path(), "pline.shx");
    (void)_copyRes(tempDir.path(), "pline.prj");
    QString errorString;
    QList<QGeoCoordinate> rgCoords;
    // Use 0 for filterMeters to disable vertex filtering for test
    QVERIFY(loadFirstPolyline(shpFile, rgCoords, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QVERIFY(rgCoords.count() > 0);
}

void ShapeTest::_testLoadPolylineFromKML()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "polyline.kml");
    QString errorString;
    QList<QGeoCoordinate> rgCoords;
    QVERIFY(loadFirstPolyline(shpFile, rgCoords, errorString, ShapeFileHelper::kDefaultVertexFilterMeters));
    QVERIFY(errorString.isEmpty());
    QVERIFY(rgCoords.count() > 0);
}

void ShapeTest::_testLoadPolygonFromSHP()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "polygon.shp");
    (void)_copyRes(tempDir.path(), "polygon.dbf");
    (void)_copyRes(tempDir.path(), "polygon.shx");
    (void)_copyRes(tempDir.path(), "polygon.prj");
    QString errorString;
    QList<QGeoCoordinate> rgCoords;
    // Use 0 for filterMeters to disable vertex filtering for test
    QVERIFY(loadFirstPolygon(shpFile, rgCoords, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QVERIFY(rgCoords.count() >= 3);
}

void ShapeTest::_testLoadPolygonFromKML()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "polygon.kml");
    QString errorString;
    QList<QGeoCoordinate> rgCoords;
    QVERIFY(loadFirstPolygon(shpFile, rgCoords, errorString, ShapeFileHelper::kDefaultVertexFilterMeters));
    QVERIFY(errorString.isEmpty());
    QVERIFY(rgCoords.count() >= 3);
}

void ShapeTest::_testLoadPolygonsFromSHP()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "polygon.shp");
    (void)_copyRes(tempDir.path(), "polygon.dbf");
    (void)_copyRes(tempDir.path(), "polygon.shx");
    (void)_copyRes(tempDir.path(), "polygon.prj");
    QString errorString;
    QList<QList<QGeoCoordinate>> polygons;
    // Use 0 for filterMeters to disable vertex filtering for test
    QVERIFY(ShapeFileHelper::loadPolygonsFromFile(shpFile, polygons, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QVERIFY(polygons.count() >= 1);
    QVERIFY(polygons.first().count() >= 3);
}

void ShapeTest::_testLoadPolylinesFromSHP()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "pline.shp");
    (void)_copyRes(tempDir.path(), "pline.dbf");
    (void)_copyRes(tempDir.path(), "pline.shx");
    (void)_copyRes(tempDir.path(), "pline.prj");
    QString errorString;
    QList<QList<QGeoCoordinate>> polylines;
    // Use 0 for filterMeters to disable vertex filtering for test
    QVERIFY(ShapeFileHelper::loadPolylinesFromFile(shpFile, polylines, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QVERIFY(polylines.count() >= 1);
    QVERIFY(polylines.first().count() >= 2);
}

void ShapeTest::_testLoadPolygonsFromKML()
{
    QTemporaryDir tempDir;
    const QString kmlFile = _copyRes(tempDir.path(), "polygon.kml");
    QString errorString;
    QList<QList<QGeoCoordinate>> polygons;
    QVERIFY(ShapeFileHelper::loadPolygonsFromFile(kmlFile, polygons, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QVERIFY(polygons.count() >= 1);
    QVERIFY(polygons.first().count() >= 3);
}

void ShapeTest::_testLoadPolylinesFromKML()
{
    QTemporaryDir tempDir;
    const QString kmlFile = _copyRes(tempDir.path(), "polyline.kml");
    QString errorString;
    QList<QList<QGeoCoordinate>> polylines;
    QVERIFY(ShapeFileHelper::loadPolylinesFromFile(kmlFile, polylines, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QVERIFY(polylines.count() >= 1);
    QVERIFY(polylines.first().count() >= 2);
}

void ShapeTest::_testGetEntityCount()
{
    QTemporaryDir tempDir;
    // Test SHP entity count
    const QString shpFile = _copyRes(tempDir.path(), "polygon.shp");
    (void)_copyRes(tempDir.path(), "polygon.dbf");
    (void)_copyRes(tempDir.path(), "polygon.shx");
    (void)_copyRes(tempDir.path(), "polygon.prj");
    QString errorString;
    const int shpCount = ShapeFileHelper::getEntityCount(shpFile, errorString);
    QVERIFY(errorString.isEmpty());
    QVERIFY(shpCount >= 1);
    // Test KML polygon entity count
    const QString kmlPolygonFile = _copyRes(tempDir.path(), "polygon.kml");
    const int kmlPolygonCount = ShapeFileHelper::getEntityCount(kmlPolygonFile, errorString);
    QVERIFY(errorString.isEmpty());
    QCOMPARE(kmlPolygonCount, 1);
    // Test KML polyline entity count
    const QString kmlPolylineFile = _copyRes(tempDir.path(), "polyline.kml");
    const int kmlPolylineCount = ShapeFileHelper::getEntityCount(kmlPolylineFile, errorString);
    QVERIFY(errorString.isEmpty());
    QCOMPARE(kmlPolylineCount, 1);
}

void ShapeTest::_testDetermineShapeType()
{
    QTemporaryDir tempDir;
    // Test polygon type detection
    const QString polygonFile = _copyRes(tempDir.path(), "polygon.shp");
    (void)_copyRes(tempDir.path(), "polygon.dbf");
    (void)_copyRes(tempDir.path(), "polygon.shx");
    (void)_copyRes(tempDir.path(), "polygon.prj");
    QString errorString;
    QCOMPARE(ShapeFileHelper::determineShapeType(polygonFile, errorString), ShapeFileHelper::ShapeType::Polygon);
    QVERIFY(errorString.isEmpty());
    // Test polyline type detection
    const QString polylineFile = _copyRes(tempDir.path(), "pline.shp");
    (void)_copyRes(tempDir.path(), "pline.dbf");
    (void)_copyRes(tempDir.path(), "pline.shx");
    (void)_copyRes(tempDir.path(), "pline.prj");
    QCOMPARE(ShapeFileHelper::determineShapeType(polylineFile, errorString), ShapeFileHelper::ShapeType::Polyline);
    QVERIFY(errorString.isEmpty());
    // Test KML type detection
    const QString kmlFile = _copyRes(tempDir.path(), "polygon.kml");
    QCOMPARE(ShapeFileHelper::determineShapeType(kmlFile, errorString), ShapeFileHelper::ShapeType::Polygon);
    QVERIFY(errorString.isEmpty());
}

void ShapeTest::_testUnsupportedProjectionError()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "polygon.shp");
    (void)_copyRes(tempDir.path(), "polygon.dbf");
    (void)_copyRes(tempDir.path(), "polygon.shx");
    // Write an unsupported projection PRJ file
    const QString prjPath = tempDir.filePath("polygon.prj");
    _writePrjFile(prjPath, "PROJCS[\"NAD_1983_StatePlane_California_III\",GEOGCS[\"GCS_North_American_1983\"]]");
    QString errorString;
    QList<QGeoCoordinate> rgCoords;
    QVERIFY(!loadFirstPolygon(shpFile, rgCoords, errorString, ShapeFileHelper::kDefaultVertexFilterMeters));
    QVERIFY(errorString.contains("NAD_1983_StatePlane_California_III"));
    QVERIFY(errorString.contains("WGS84"));
}

void ShapeTest::_testLoadFromQtResource()
{
    // Test loading directly from Qt resource path (tests QFile hooks)
    // Note: This requires the test resources to include all required files (.shp, .shx, .prj)
    QString errorString;
    QList<QGeoCoordinate> rgCoords;
    // Test KML from resource (KML already uses QFile internally)
    QVERIFY(
        loadFirstPolygon(":/unittest/polygon.kml", rgCoords, errorString, ShapeFileHelper::kDefaultVertexFilterMeters));
    QVERIFY(errorString.isEmpty());
    QVERIFY(rgCoords.count() >= 3);
}

void ShapeTest::_testVertexFiltering()
{
    QTemporaryDir tempDir;
    const QString shpFile = _copyRes(tempDir.path(), "polygon.shp");
    (void)_copyRes(tempDir.path(), "polygon.dbf");
    (void)_copyRes(tempDir.path(), "polygon.shx");
    (void)_copyRes(tempDir.path(), "polygon.prj");
    QString errorString;
    QList<QGeoCoordinate> unfilteredCoords;
    QList<QGeoCoordinate> filteredCoords;
    // Load without filtering
    QVERIFY(loadFirstPolygon(shpFile, unfilteredCoords, errorString, 0));
    QVERIFY(errorString.isEmpty());
    // Load with filtering (default 5m)
    QVERIFY(loadFirstPolygon(shpFile, filteredCoords, errorString, ShapeFileHelper::kDefaultVertexFilterMeters));
    QVERIFY(errorString.isEmpty());
    // Filtered should have same or fewer vertices than unfiltered
    QVERIFY(filteredCoords.count() <= unfilteredCoords.count());
    // Both should have at least minimum valid polygon (3 vertices)
    QVERIFY(unfilteredCoords.count() >= 3);
    QVERIFY(filteredCoords.count() >= 3);
}

void ShapeTest::_testKMLVertexFiltering()
{
    QTemporaryDir tempDir;
    // Create KML with closely spaced vertices (< 5m apart at equator)
    // 0.00001 degrees ≈ 1.1m at equator
    const QString kmlContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Placemark>
    <Polygon>
      <outerBoundaryIs>
        <LinearRing>
          <coordinates>
            0.0,0.0,0 0.00001,0.0,0 0.00002,0.0,0 0.00003,0.0,0 0.001,0.001,0 0.0,0.001,0 0.0,0.0,0
          </coordinates>
        </LinearRing>
      </outerBoundaryIs>
    </Polygon>
  </Placemark>
</kml>)";
    const QString kmlFile = _writeKmlFile(tempDir.path(), "dense_polygon.kml", kmlContent);
    QString errorString;
    QList<QGeoCoordinate> unfilteredCoords;
    QList<QGeoCoordinate> filteredCoords;
    // Load without filtering
    QVERIFY(loadFirstPolygon(kmlFile, unfilteredCoords, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QCOMPARE(unfilteredCoords.count(), 6);  // 7 coords - 1 duplicate closing = 6
    // Load with filtering (default 5m)
    QVERIFY(loadFirstPolygon(kmlFile, filteredCoords, errorString, ShapeFileHelper::kDefaultVertexFilterMeters));
    QVERIFY(errorString.isEmpty());
    // Filtered should have fewer vertices (the closely spaced ones should be filtered)
    QVERIFY(filteredCoords.count() < unfilteredCoords.count());
    QVERIFY(filteredCoords.count() >= 3);  // Minimum valid polygon
}

void ShapeTest::_testKMLAltitudeParsing()
{
    QTemporaryDir tempDir;
    // Create KML with altitude values
    const QString kmlContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Placemark>
    <Polygon>
      <outerBoundaryIs>
        <LinearRing>
          <coordinates>
            -122.0,37.0,100.5 -122.0,38.0,200.0 -121.0,38.0,150.0 -121.0,37.0,175.5 -122.0,37.0,100.5
          </coordinates>
        </LinearRing>
      </outerBoundaryIs>
    </Polygon>
  </Placemark>
</kml>)";
    const QString kmlFile = _writeKmlFile(tempDir.path(), "altitude_polygon.kml", kmlContent);
    QString errorString;
    QList<QGeoCoordinate> coords;
    QVERIFY(loadFirstPolygon(kmlFile, coords, errorString, 0));
    QVERIFY(errorString.isEmpty());
    QCOMPARE(coords.count(), 4);  // 5 coords - 1 duplicate closing = 4
    // Verify altitudes were parsed correctly
    QCOMPARE(coords[0].altitude(), 100.5);
    QCOMPARE(coords[1].altitude(), 200.0);
    QCOMPARE(coords[2].altitude(), 150.0);
    QCOMPARE(coords[3].altitude(), 175.5);
    // Verify lat/lon as well
    QCOMPARE(coords[0].latitude(), 37.0);
    QCOMPARE(coords[0].longitude(), -122.0);
}

void ShapeTest::_testKMLCoordinateValidation()
{
    QTemporaryDir tempDir;
    // Create KML with some invalid coordinates mixed with valid ones
    // Invalid: lat=91 (out of range), lon=200 (out of range), swapped lat/lon
    const QString kmlContent = R"(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Placemark>
    <Polygon>
      <outerBoundaryIs>
        <LinearRing>
          <coordinates>
            -122.0,37.0,0 -122.0,91.0,0 200.0,38.0,0 -121.0,38.0,0 -121.0,37.0,0 -122.0,37.0,0
          </coordinates>
        </LinearRing>
      </outerBoundaryIs>
    </Polygon>
  </Placemark>
</kml>)";
    const QString kmlFile = _writeKmlFile(tempDir.path(), "invalid_coords.kml", kmlContent);
    // Expect warnings for invalid coordinates
    expectLogMessage("Utilities.KMLHelper", QtWarningMsg, QRegularExpression("Latitude out of range.*91"));
    expectLogMessage("Utilities.KMLHelper", QtWarningMsg, QRegularExpression("Longitude out of range.*200"));
    QString errorString;
    QList<QGeoCoordinate> coords;
    // Should succeed but skip invalid coordinates
    QVERIFY(loadFirstPolygon(kmlFile, coords, errorString, 0));
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();
    QVERIFY(errorString.isEmpty());
    // Should have 3 valid coordinates (the two invalid ones skipped, plus duplicate closing removed)
    // Valid: -122,37 | -121,38 | -121,37
    // Invalid: -122,91 (lat>90) | 200,38 (lon>180)
    QCOMPARE(coords.count(), 3);
    // Verify the valid coordinates are present
    QCOMPARE(coords[0].latitude(), 37.0);
    QCOMPARE(coords[0].longitude(), -122.0);
    QCOMPARE(coords[1].latitude(), 38.0);
    QCOMPARE(coords[1].longitude(), -121.0);
    QCOMPARE(coords[2].latitude(), 37.0);
    QCOMPARE(coords[2].longitude(), -121.0);
}

void ShapeTest::_testKMLExportSchemaValidation()
{
    QTemporaryDir tempDir;
    // Test that KMLSchemaValidator is properly loaded and functional
    const auto* validator = KMLSchemaValidator::instance();
    // Verify schema was loaded and enum types were extracted
    const QStringList altitudeModes = validator->validEnumValues("altitudeModeEnumType");
    QVERIFY(!altitudeModes.isEmpty());
    QVERIFY(altitudeModes.contains("absolute"));
    QVERIFY(altitudeModes.contains("clampToGround"));
    QVERIFY(altitudeModes.contains("relativeToGround"));
    // Verify element validation works
    QVERIFY(validator->isValidElement("Polygon"));
    QVERIFY(validator->isValidElement("LineString"));
    QVERIFY(validator->isValidElement("Point"));
    QVERIFY(validator->isValidElement("coordinates"));
    QVERIFY(validator->isValidElement("altitudeMode"));
    // Create a KML document and validate it
    KMLDomDocument doc("Test Export");
    QDomElement placemark = doc.addPlacemark("Test Point", true);
    QDomElement point = doc.createElement("Point");
    doc.addTextElement(point, "altitudeMode", "absolute");
    doc.addTextElement(point, "coordinates", doc.kmlCoordString(QGeoCoordinate(37.0, -122.0, 100.0)));
    placemark.appendChild(point);
    // Validate the document
    const auto result = validator->validate(doc);
    if (!result.isValid) {
        qWarning() << "KML validation errors:" << result.errors;
    }
    QVERIFY(result.isValid);
    QVERIFY(result.errors.isEmpty());
    // Test validation catches invalid altitudeMode
    const QString invalidKml = R"(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Placemark>
      <Point>
        <altitudeMode>invalidMode</altitudeMode>
        <coordinates>-122.0,37.0,0</coordinates>
      </Point>
    </Placemark>
  </Document>
</kml>)";
    const QString invalidKmlPath = _writeKmlFile(tempDir.path(), "invalid.kml", invalidKml);
    const auto invalidResult = validator->validateFile(invalidKmlPath);
    QVERIFY(!invalidResult.isValid);
    QVERIFY(invalidResult.errors.first().contains("invalidMode"));
    // Test validation catches out-of-range coordinates
    const QString badCoordsKml = R"(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Placemark>
      <Point>
        <coordinates>200.0,95.0,0</coordinates>
      </Point>
    </Placemark>
  </Document>
</kml>)";
    const QString badCoordsPath = _writeKmlFile(tempDir.path(), "bad_coords.kml", badCoordsKml);
    const auto badCoordsResult = validator->validateFile(badCoordsPath);
    QVERIFY(!badCoordsResult.isValid);
    QVERIFY(badCoordsResult.errors.size() >= 2);  // lat and lon both out of range
}

void ShapeTest::_testStrictKML_data()
{
    QTest::addColumn<QString>("geometry");
    QTest::addColumn<bool>("success");
    QTest::addColumn<QString>("message");
    const QString good = QStringLiteral(
        "<Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 0.00001,0 1,0 1,1 "
        "0,0</coordinates></LinearRing></outerBoundaryIs></Polygon>");
    const auto polygon = [](const QString& coordinates) {
        return QStringLiteral(
                   "<Polygon><outerBoundaryIs><LinearRing><coordinates>%1</coordinates></LinearRing></"
                   "outerBoundaryIs></Polygon>")
            .arg(coordinates);
    };
    QTest::newRow("preserve") << good << true << QString();
    QTest::newRow("multi")
        << QStringLiteral("<MultiGeometry>%1%2</MultiGeometry>").arg(good, polygon(QStringLiteral("2,2 3,2 3,3 2,2")))
        << true << QString();
    QTest::newRow("prefixed polygon")
        << QStringLiteral(
               "<k:Polygon xmlns:k=\"urn:kml\"><k:outerBoundaryIs><k:LinearRing><k:coordinates>0,0 0.00001,0 1,0 1,1 "
               "0,0</k:coordinates></k:LinearRing></k:outerBoundaryIs></k:Polygon>")
        << true << QString();
    QTest::newRow("hole") << QString(good).replace(QStringLiteral("</Polygon>"),
                                                   QStringLiteral("<innerBoundaryIs/></Polygon>"))
                          << false << QStringLiteral("innerBoundaryIs");
    QTest::newRow("point") << (good + QStringLiteral("<Point/>")) << false
                           << QStringLiteral("unsupported geometry Point");
    QTest::newRow("line") << QStringLiteral("<LineString/>") << false
                          << QStringLiteral("unsupported geometry LineString");
    QTest::newRow("unclosed") << polygon(QStringLiteral("0,0 1,0 1,1")) << false
                              << QStringLiteral("ring is not closed");
    QTest::newRow("distinct") << polygon(QStringLiteral("0,0 1,0 1,0 0,0")) << false
                              << QStringLiteral("fewer than three distinct");
    QTest::newRow("tuple") << polygon(QStringLiteral("0,0 1,0 broken 0,0")) << false
                           << QStringLiteral("invalid coordinate tuple");
    QTest::newRow("longitude range") << polygon(QStringLiteral("0,0 1,0 181,1 0,0")) << false
                                     << QStringLiteral("invalid coordinate tuple");
    QTest::newRow("range") << polygon(QStringLiteral("0,0 1,0 1,91 0,0")) << false
                           << QStringLiteral("invalid coordinate tuple");
    QTest::newRow("altitude") << polygon(QStringLiteral("0,0 1,0 1,1,bad 0,0")) << false
                              << QStringLiteral("invalid coordinate tuple");
    QTest::newRow("atomic") << (good + polygon(QStringLiteral("0,0 1,0 1,1"))) << false
                            << QStringLiteral("ring is not closed");
    QTest::newRow("empty polygon") << QStringLiteral("<Polygon/>") << false << QStringLiteral("no coordinates");
    QTest::newRow("no geometry") << QStringLiteral("<name>label</name>") << false
                                 << QStringLiteral("No polygon geometry found");
}

void ShapeTest::_testStrictKML()
{
    QFETCH(QString, geometry);
    QFETCH(bool, success);
    QFETCH(QString, message);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString content = QStringLiteral(
                                "<kml><Document><Placemark><name>label</name></Placemark><Folder><Placemark>%1</"
                                "Placemark></Folder></Document></kml>")
                                .arg(geometry);
    const QString file = _writeKmlFile(dir.path(), QStringLiteral("strict.kml"), content);
    QList<QList<QGeoCoordinate>> polygons = {{QGeoCoordinate(2, 2)}};
    QString error;
    QCOMPARE(ShapeFileHelper::loadStrictPolygonsFromFile(file, polygons, error), success);
    if (!success) {
        QVERIFY(polygons.isEmpty());
        QVERIFY2(error.contains(message), qPrintable(error));
        QVERIFY(error.contains(QStringLiteral("strict.kml")));
        if (message == QLatin1String("invalid coordinate tuple")) {
            QVERIFY(error.contains(QStringLiteral("line")));
            QVERIFY(error.contains(QStringLiteral("vertex")));
        }
        return;
    }
    QVERIFY(error.isEmpty());
    const int expectedCount = geometry.startsWith(QStringLiteral("<MultiGeometry>")) ? 2 : 1;
    QCOMPARE(polygons.size(), expectedCount);
    const QList<QGeoCoordinate> expected = {QGeoCoordinate(0, 0), QGeoCoordinate(0, 0.00001), QGeoCoordinate(0, 1),
                                            QGeoCoordinate(1, 1)};
    QCOMPARE(polygons.first(), expected);
    if (expectedCount == 2) {
        const QList<QGeoCoordinate> second = {QGeoCoordinate(2, 2), QGeoCoordinate(2, 3), QGeoCoordinate(3, 3)};
        QCOMPARE(polygons[1], second);
    }
}

void ShapeTest::_testStrictSHP()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto create = [&](const QString& name, int type, const QList<QList<int>>& partsByEntity,
                            const QList<QList<QGeoCoordinate>>& entities) {
        const QString path = dir.filePath(name + QStringLiteral(".shp"));
        SHPHandle handle = SHPCreate(path.toUtf8().constData(), type);
        if (!handle) {
            return QString();
        }
        for (qsizetype entity = 0; entity < entities.size(); ++entity) {
            QList<double> x, y;
            for (const auto& coordinate : entities[entity]) {
                x.append(coordinate.longitude());
                y.append(coordinate.latitude());
            }
            const auto& parts = partsByEntity[entity];
            SHPObject* object =
                SHPCreateObject(type, -1, static_cast<int>(parts.size()), parts.constData(), nullptr,
                                static_cast<int>(x.size()), x.constData(), y.constData(), nullptr, nullptr);
            if (!object || SHPWriteObject(handle, -1, object) < 0) {
                if (object) {
                    SHPDestroyObject(object);
                }
                SHPClose(handle);
                return QString();
            }
            SHPDestroyObject(object);
        }
        SHPClose(handle);
        _writePrjFile(dir.filePath(name + QStringLiteral(".prj")), QStringLiteral("GEOGCS[\"GCS_WGS_1984\","));
        return path;
    };
    const QList<QGeoCoordinate> ring = {QGeoCoordinate(0, 0), QGeoCoordinate(0, 0.00001), QGeoCoordinate(0, 1),
                                        QGeoCoordinate(1, 1), QGeoCoordinate(0, 0)};
    const QList<QGeoCoordinate> second = {QGeoCoordinate(2, 2), QGeoCoordinate(2, 3), QGeoCoordinate(3, 3),
                                          QGeoCoordinate(2, 2)};
    const QList<QGeoCoordinate> hole = {QGeoCoordinate(0.1, 0.1), QGeoCoordinate(0.2, 0.2), QGeoCoordinate(0.1, 0.2),
                                        QGeoCoordinate(0.1, 0.1)};
    const QList<QGeoCoordinate> bad = {QGeoCoordinate(0, 0), QGeoCoordinate(0, 1), QGeoCoordinate(0, 1),
                                       QGeoCoordinate(0, 0)};
    QList<QList<QGeoCoordinate>> polygons;
    QString error;
    const QString valid = create(QStringLiteral("valid"), SHPT_POLYGON, {{0}}, {ring});
    QVERIFY(!valid.isEmpty());
    QVERIFY2(ShapeFileHelper::loadStrictPolygonsFromFile(valid, polygons, error), qPrintable(error));
    QCOMPARE(polygons.size(), 1);
    QCOMPARE(polygons.first(), ring.mid(0, ring.size() - 1));
    const auto reject = [&](const QString& file, const QString& expected) {
        polygons = {{QGeoCoordinate(5, 5)}};
        const bool loaded = ShapeFileHelper::loadStrictPolygonsFromFile(file, polygons, error);
        return !file.isEmpty() && !loaded && polygons.isEmpty() && error.contains(expected) &&
               error.contains(QFileInfo(file).fileName());
    };
    const QString multipart = create(QStringLiteral("multipart"), SHPT_POLYGON, {{0, 5}}, {ring + second});
    QVERIFY(reject(multipart, QStringLiteral("multipart polygon entity 1 needs conversion")));
    QVERIFY(reject(create(QStringLiteral("hole"), SHPT_POLYGON, {{0, 5}}, {ring + hole}),
                   QStringLiteral("multipart polygon entity 1")));
    QVERIFY(
        reject(create(QStringLiteral("point"), SHPT_POINT, {{}}, {{QGeoCoordinate(0, 0)}}), QStringLiteral("Point")));
    QVERIFY(reject(create(QStringLiteral("line"), SHPT_ARC, {{0}}, {ring}), QStringLiteral("Arc")));
    QVERIFY(reject(create(QStringLiteral("distinct"), SHPT_POLYGON, {{0}}, {bad}),
                   QStringLiteral("fewer than three distinct")));
    QVERIFY(reject(create(QStringLiteral("unclosed"), SHPT_POLYGON, {{0}}, {ring.mid(0, 4)}),
                   QStringLiteral("ring is not closed")));
    QVERIFY(
        reject(create(QStringLiteral("atomic"), SHPT_POLYGON, {{0}, {0}}, {bad, ring}), QStringLiteral("entity 1")));
    QVERIFY(
        reject(create(QStringLiteral("atomic2"), SHPT_POLYGON, {{0}, {0}}, {ring, bad}), QStringLiteral("entity 2")));
    QVERIFY(reject(create(QStringLiteral("empty"), SHPT_POLYGON, {}, {}), QStringLiteral("No polygon geometry found")));
    QVERIFY(ShapeFileHelper::loadPolygonsFromFile(multipart, polygons, error, 0));
    QCOMPARE(polygons.size(), 1);
    QCOMPARE(polygons.first().size(), ring.size());
}

void ShapeTest::_testStrictDispatcher()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QList<QList<QGeoCoordinate>> polygons = {{QGeoCoordinate(0, 0)}};
    QString error;
    QVERIFY(!ShapeFileHelper::loadStrictPolygonsFromFile(dir.filePath(QStringLiteral("unknown.txt")), polygons, error));
    QVERIFY(polygons.isEmpty());
    QVERIFY(error.contains(QStringLiteral("unknown.txt")));
    QVERIFY(error.contains(QStringLiteral("Unsupported file type")));
    QVERIFY(!ShapeFileHelper::loadStrictPolygonsFromFile(dir.filePath(QStringLiteral("missing.kml")), polygons, error));
    QVERIFY(error.contains(QStringLiteral("Unable to open")));
    const QString invalid = _writeKmlFile(dir.path(), QStringLiteral("invalid.kml"), QStringLiteral("<kml>"));
    QVERIFY(!ShapeFileHelper::loadStrictPolygonsFromFile(invalid, polygons, error));
    QVERIFY(error.contains(QStringLiteral("Unable to parse KML")));
    const QString empty = _writeKmlFile(dir.path(), QStringLiteral("empty.kml"), QString());
    QVERIFY(!ShapeFileHelper::loadStrictPolygonsFromFile(empty, polygons, error));
    QVERIFY(error.contains(QStringLiteral("No polygon geometry found")));
    const QString hole = _writeKmlFile(
        dir.path(), QStringLiteral("hole.kml"),
        QStringLiteral(
            "<kml><Placemark><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 1,0 1,1 0,0</coordinates>"
            "</LinearRing></outerBoundaryIs><innerBoundaryIs><LinearRing><coordinates>0.1,0.1 0.2,0.1 0.2,0.2 0.1,0.1"
            "</coordinates></LinearRing></innerBoundaryIs></Polygon></Placemark></kml>"));
    QVERIFY(ShapeFileHelper::loadPolygonsFromFile(hole, polygons, error, 0));
    QCOMPARE(polygons.size(), 1);
    QCOMPARE(polygons.first().size(), 3);
    QVERIFY(!ShapeFileHelper::loadStrictPolygonsFromFile(hole, polygons, error));
    QVERIFY(polygons.isEmpty());
    QVERIFY(error.contains(QStringLiteral("innerBoundaryIs")));
}

UT_REGISTER_TEST(ShapeTest, TestLabel::Unit, TestLabel::Utilities)
