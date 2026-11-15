#include "PlanStyleComponentTest.h"

#include <memory>

#include <QtCore/QFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>

#include "AppSettings.h"
#include "Fact.h"
#include "FlightPathSegment.h"
#include "GeoFenceController.h"
#include "PlanMasterController.h"
#include "QGCCorePlugin.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCMAVLink.h"
#include "QGCMapCircle.h"
#include "QGCMapPolygon.h"
#include "QGCPlanStyle.h"
#include "QGroundControlQmlGlobal.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "SettingsManager.h"
#include "Vehicle.h"

namespace {
QList<QQuickItem*> componentPlanStyleItems(QQuickItem* root, const QString& name)
{
    QList<QQuickItem*> result;
    QSet<QObject*> visited;
    // Map items leave their Loader's visual tree; sourceItem may remain detached until window polish.
    const auto visit = [&](QObject* object, auto&& self) -> void {
        if (!object || visited.contains(object)) {
            return;
        }
        visited.insert(object);
        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            if (item->objectName() == name) {
                result.append(item);
            }
            for (auto* child : item->childItems()) {
                self(child, self);
            }
            self(qvariant_cast<QQuickItem*>(item->property("sourceItem")), self);
        }
        for (auto* child : object->children()) {
            self(child, self);
        }
    };
    visit(root, visit);
    return result;
}
}  // namespace

void PlanStyleComponentTest::_sharedComponents()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtQuick.Shapes
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        Item {
            readonly property int dashedStrokeStyle: ShapePath.DashLine
            readonly property color neutralColor: QGroundControl.globalPalette.mapMissionTrajectory
            readonly property real markerSize: ScreenTools.defaultFontPixelHeight * 1.6
            MapDashedLine {
                objectName: "line"
                width: 100
                height: 50
                points: [Qt.point(0, 0), Qt.point(100, 50)]
                color: "blue"
                dashPattern: [6, 3]
            }
            MapBoundaryOutline {
                objectName: "outline"
                lineColor: "blue"
                dashPattern: [2, 2]
            }
            RallyPointMarker {
                objectName: "marker"
                color: "blue"
                borderColor: "white"
                borderWidth: 2
                textColor: "black"
            }
            PlanStyleContext { }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* line = root->findChild<QQuickItem*>(QStringLiteral("line"));
    auto* outline = root->findChild<QQuickItem*>(QStringLiteral("outline"));
    auto* marker = root->findChild<QQuickItem*>(QStringLiteral("marker"));
    auto* context = root->findChild<QObject*>(QStringLiteral("planStyleContext"));
    QVERIFY(line);
    QVERIFY(outline);
    QVERIFY(marker);
    QVERIFY(context);
    QCOMPARE(line->property("lineWidth").toReal(), 3);
    QCOMPARE(line->property("dashPattern").toList(), QVariantList({6.0, 3.0}));
    const auto strokePath = [](QObject* shape) -> QObject* {
        if (!shape) {
            return nullptr;
        }
        for (auto* child : shape->findChildren<QObject*>()) {
            if (child && child->metaObject()->indexOfProperty("strokeWidth") >= 0) {
                return child;
            }
        }
        return nullptr;
    };
    auto* path = strokePath(line);
    QVERIFY(path);
    QCOMPARE(path->property("strokeWidth").toReal(), 3);
    QCOMPARE(path->property("strokeStyle").toInt(), root->property("dashedStrokeStyle").toInt());
    QCOMPARE(path->property("fillColor").value<QColor>(), QColor(QStringLiteral("transparent")));
    QCOMPARE(path->property("strokeColor").value<QColor>(), QColor(QStringLiteral("blue")));
    QCOMPARE(path->property("dashPattern").toList(), QVariantList({6.0, 3.0}));
    QVERIFY(line->setProperty("lineWidth", 5));
    QCOMPARE(path->property("strokeWidth").toReal(), 5);
    auto* outlineLine = qvariant_cast<QQuickItem*>(outline->property("sourceItem"));
    QVERIFY(outlineLine);
    QCOMPARE(outline->property("lineWidth").toReal(), 3);
    QCOMPARE(outlineLine->property("lineWidth").toReal(), 3);
    QCOMPARE(outlineLine->property("color").value<QColor>(), QColor(QStringLiteral("blue")));
    QCOMPARE(outlineLine->property("dashPattern").toList(), QVariantList({2.0, 2.0}));
    QVERIFY(outline->setProperty("lineWidth", 7));
    QCOMPARE(outlineLine->property("lineWidth").toReal(), 7);
    QVERIFY(outline->property("_screenPoints").toList().isEmpty());

    QCOMPARE(marker->width(), root->property("markerSize").toReal());
    QCOMPARE(marker->height(), marker->width());
    QCOMPARE(marker->property("anchorPointX").toReal(), marker->width() / 2);
    QCOMPARE(marker->property("anchorPointY").toReal(), marker->height() / 2);
    QQuickItem* diamond = nullptr;
    QQuickItem* label = nullptr;
    for (auto* child : marker->childItems()) {
        if (!child) {
            continue;
        }
        if (child->rotation() == 45) {
            diamond = child;
        } else if (child->metaObject()->indexOfProperty("text") >= 0) {
            label = child;
        }
    }
    QVERIFY(diamond);
    QVERIFY(label);
    QCOMPARE(diamond->width(), marker->width() * 0.72);
    QCOMPARE(diamond->height(), diamond->width());
    QCOMPARE(diamond->property("color").value<QColor>(), QColor(QStringLiteral("blue")));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.color")).read().value<QColor>(),
             QColor(QStringLiteral("white")));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.width")).read().toReal(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(diamond->x() - (marker->width() - diamond->width()) / 2) < 1, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(diamond->y() - (marker->height() - diamond->height()) / 2) < 1,
                             TestTimeout::shortMs());
    QCOMPARE(label->rotation(), 0);
    QVERIFY(QQmlProperty(label, QStringLiteral("font.bold")).read().toBool());
    QCOMPARE(label->property("text").toString(), QStringLiteral("R"));
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("black")));
    QVERIFY(marker->setProperty("color", QColor(QStringLiteral("green"))));
    QCOMPARE(diamond->property("color").value<QColor>(), QColor(QStringLiteral("green")));
    QVERIFY(marker->setProperty("borderColor", QColor(QStringLiteral("red"))));
    QVERIFY(marker->setProperty("borderWidth", 4));
    QVERIFY(marker->setProperty("textColor", QColor(QStringLiteral("yellow"))));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.color")).read().value<QColor>(),
             QColor(QStringLiteral("red")));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.width")).read().toReal(), 4);
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("yellow")));

    auto* policy = plugin->planStyle();
    QCOMPARE(context->property("enabled").toBool(), policy != nullptr);
    QCOMPARE(context->property("vehicleId").toUInt(), 0U);
    QCOMPARE(context->property("lineColor").value<QColor>(), root->property("neutralColor").value<QColor>());
    QVERIFY(context->property("lineColor").value<QColor>().isValid());
    QCOMPARE(context->property("missionLineWidth").toReal(), policy ? policy->missionLineWidth() : 0);
    QCOMPARE(context->property("boundaryLineWidth").toReal(), policy ? policy->boundaryLineWidth() : 0);
    QCOMPARE(context->property("keepInDashPattern").toList(), policy ? policy->keepInDashPattern() : QVariantList());
    QCOMPARE(context->property("keepOutDashPattern").toList(), policy ? policy->keepOutDashPattern() : QVariantList());
    QCOMPARE(context->property("keepOutFillColor").value<QColor>(), policy ? policy->keepOutFillColor() : QColor());
    QCOMPARE(context->property("markerBorderColor").value<QColor>(), policy ? policy->markerBorderColor() : QColor());
    QCOMPARE(context->property("markerBorderWidth").toReal(), policy ? policy->markerBorderWidth() : 0);
    QCOMPARE(context->property("markerTextColor").value<QColor>(), policy ? policy->markerTextColor() : QColor());
    QCOMPARE(context->property("rallyShape").toString(), policy ? policy->rallyShape() : QString());
    QVERIFY(context->setProperty("supported", false));
    QVERIFY(!context->property("enabled").toBool());
}

void PlanStyleComponentTest::_stockMissionVisuals_data()
{
    QTest::addColumn<bool>("disabledContext");
    QTest::newRow("null") << false;
    QTest::newRow("disabled") << true;
}

void PlanStyleComponentTest::_stockMissionVisuals()
{
    QFETCH(bool, disabledContext);
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    QmlObjectListModel segments;
    FlightPathSegment segment(FlightPathSegment::SegmentTypeGeneric, QGeoCoordinate(47, 8), 100,
                              QGeoCoordinate(47.001, 8.001), 100, false, nullptr);
    segments.append(&segment);
    engine->rootContext()->setContextProperty(QStringLiteral("testSegments"), &segments);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        Map {
            id: testMap
            width: 640
            height: 480
            plugin: Plugin { name: "QGroundControl" }
            property bool planView: false
            property bool isSatelliteMap: false
            property bool disabledContext: false
            property var style: disabledContext ? disabledStyle : null
            readonly property color trajectory: QGroundControl.globalPalette.mapMissionTrajectory
            readonly property color ordinary: palette.mapIndicator
            readonly property color childColor: palette.mapIndicatorChild
            QGCPalette { id: palette }
            QtObject {
                id: disabledStyle
                property bool enabled: false
                property real vehicleId: 77
                property color lineColor: "magenta"
                property real missionLineWidth: 9
                property color markerBorderColor: "cyan"
                property real markerBorderWidth: 7
                property color markerTextColor: "yellow"
            }
            property QtObject object: QtObject {
                property var coordinate: QtPositioning.coordinate(47, 8)
                property bool specifiesCoordinate: true
                property bool isCurrentItem: false
                property bool hasCurrentChildItem: false
                property string abbreviation: ""
                property int sequenceNumber: 5
                property real missionGimbalYaw: NaN
                property real missionVehicleYaw: NaN
            }
            MissionLineView { model: testSegments; planStyle: testMap.style }
            MissionItemIndicator {
                missionItem: testMap.object
                sequenceNumber: testMap.object.sequenceNumber
                coordinate: testMap.object.coordinate
                planStyle: testMap.style
            }
            MissionItemIndexLabel {
                objectName: "childLabel"
                child: true
                planStyle: testMap.style
            }
            HomePositionMapVisual { map: testMap; planStyle: testMap.style }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(root->setProperty("disabledContext", disabledContext));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    // Map items leave their Loader's visual tree; sourceItem may remain detached until window polish.
    const auto find = [](QObject* searchRoot, const QString& name) {
        QSet<QObject*> visited;
        const auto visit = [&](QObject* object, auto&& self) -> QQuickItem* {
            if (!object || visited.contains(object)) {
                return nullptr;
            }
            visited.insert(object);
            if (auto* item = qobject_cast<QQuickItem*>(object)) {
                if (item->objectName() == name) {
                    return item;
                }
                for (auto* child : item->childItems()) {
                    if (auto* match = self(child, self)) {
                        return match;
                    }
                }
                if (auto* match = self(qvariant_cast<QQuickItem*>(item->property("sourceItem")), self)) {
                    return match;
                }
            }
            for (auto* child : object->children()) {
                if (auto* match = self(child, self)) {
                    return match;
                }
            }
            return nullptr;
        };
        return visit(searchRoot, visit);
    };
    QTRY_VERIFY_WITH_TIMEOUT(find(map, QStringLiteral("planMissionRoute")), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(find(map, QStringLiteral("planWaypoint")), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(find(map, QStringLiteral("planHomeMarker")), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(find(map, QStringLiteral("planHomeAccent")), TestTimeout::mediumMs());
    auto* route = find(map, QStringLiteral("planMissionRoute"));
    auto* marker = find(map, QStringLiteral("planWaypoint"));
    auto* home = find(map, QStringLiteral("planHomeMarker"));
    auto* accent = find(map, QStringLiteral("planHomeAccent"));
    QVERIFY(accent);
    QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 3.0);
    QCOMPARE(QQmlProperty(route, QStringLiteral("line.color")).read().value<QColor>(),
             root->property("trajectory").value<QColor>());
    QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), root->property("ordinary").value<QColor>());
    QCOMPARE(marker->property("renderedBorderWidth").toReal(), 0.0);
    QCOMPARE(marker->property("renderedTextColor").value<QColor>(), QColor(QStringLiteral("white")));
    QCOMPARE(marker->property("index").toInt(), 5);
    QCOMPARE(marker->property("renderedIndexText").toString(), QStringLiteral("5"));
    auto* childLabel = find(map, QStringLiteral("childLabel"));
    QVERIFY(childLabel);
    QCOMPARE(childLabel->property("renderedBodyColor").value<QColor>(), root->property("childColor").value<QColor>());
    auto* label = qvariant_cast<QQuickItem*>(marker->property("sourceItem"));
    QVERIFY(label);
    auto* body = qvariant_cast<QQuickItem*>(label->property("renderedIndicator"));
    QVERIFY(body);
    QTRY_VERIFY_WITH_TIMEOUT(body->width() > 0, TestTimeout::shortMs());
    const qreal ordinarySize = body->width();
    auto* missionItem = qvariant_cast<QObject*>(root->property("object"));
    QVERIFY(missionItem);
    QVERIFY(missionItem->setProperty("isCurrentItem", true));
    QTRY_COMPARE_WITH_TIMEOUT(marker->property("renderedBodyColor").value<QColor>(), QColor(QStringLiteral("green")),
                              TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(body->width() > ordinarySize, TestTimeout::shortMs());
    auto* halo = find(label, QStringLiteral("planWaypointSelectionHalo"));
    QVERIFY(halo);
    QVERIFY(halo->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(halo->width(), body->width() * 2, TestTimeout::shortMs());
    QCOMPARE(QQmlProperty(halo, QStringLiteral("border.color")).read().value<QColor>(), QColor::fromRgbF(1, 1, 1, 0.5));
    QVERIFY(!accent->isVisible());
    QCOMPARE(QQmlProperty(accent, QStringLiteral("border.width")).read().toReal(), 0.0);
    auto* house = qvariant_cast<QQuickItem*>(home->property("sourceItem"));
    QVERIFY(house);
    QVERIFY(house->inherits("QQuickImage"));
    QCOMPARE(house->property("source").toUrl(), QUrl(QStringLiteral("qrc:///qmlimages/MapHome.svg")));
    QCOMPARE(find(house, QStringLiteral("planHomeAccent")), accent);
    // Exercise the production collision computation rather than overriding the rendered colour or flag.
    QVERIFY(segment.totalDistance() > 0.0);
    QVERIFY(segment.setProperty("distanceBetween", segment.totalDistance()));
    QVERIFY(segment.setProperty("finalDistanceBetween", segment.totalDistance()));
    QVERIFY(segment.setProperty("amslTerrainHeights", QVariantList{150.0, 150.0}));
    segment.setCoord1AMSLAlt(101);
    QVERIFY(segment.terrainCollision());
    QTRY_COMPARE_WITH_TIMEOUT(QQmlProperty(route, QStringLiteral("line.color")).read().value<QColor>(),
                              QColor(QStringLiteral("red")), TestTimeout::shortMs());
    segment.setCoord1AMSLAlt(200);
    segment.setCoord2AMSLAlt(200);
    QVERIFY(!segment.terrainCollision());
    QTRY_COMPARE_WITH_TIMEOUT(QQmlProperty(route, QStringLiteral("line.color")).read().value<QColor>(),
                              root->property("trajectory").value<QColor>(), TestTimeout::shortMs());
}

void PlanStyleComponentTest::_optionalVisualContract()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fileName = directory.filePath(QStringLiteral("ExternalVisual.qml"));
    QFile file(fileName);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray external = R"(
        import QtQuick
        Item {
            objectName: "externalVisual"
            property var map
            property var vehicle
            property bool interactive: true
        }
    )";
    QCOMPARE(file.write(external), external.size());
    file.close();
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    engine->rootContext()->setContextProperty(QStringLiteral("externalVisualUrl"), QUrl::fromLocalFile(fileName));
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QGroundControl.PlanView
        Item {
            id: testMap
            property QtObject object: QtObject {
                property bool isSimpleItem: false
                property url mapVisualQML: externalVisualUrl
            }
            QtObject { id: style; property bool enabled: true }
            MissionItemMapVisual { map: testMap; planStyle: style }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    const auto externalItem = [map]() -> QQuickItem* {
        for (auto* child : map->childItems()) {
            if (child->objectName() == QStringLiteral("externalVisual")) {
                return child;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(externalItem(), TestTimeout::mediumMs());
    QCOMPARE(externalItem()->metaObject()->indexOfProperty("planStyle"), -1);
    QCOMPARE(qvariant_cast<QObject*>(externalItem()->property("map")), root.get());
}

void PlanStyleComponentTest::_stockBoundaryVisuals_data()
{
    QTest::addColumn<bool>("disabledContext");
    QTest::addColumn<bool>("fence");
    QTest::addColumn<bool>("inclusion");
    QTest::newRow("null-inclusion") << false << true << true;
    QTest::newRow("null-exclusion") << false << true << false;
    QTest::newRow("disabled-inclusion") << true << true << true;
    QTest::newRow("disabled-exclusion") << true << true << false;
    QTest::newRow("generic") << false << false << true;
    QTest::newRow("generic-disabled") << true << false << true;
}

void PlanStyleComponentTest::_stockBoundaryVisuals()
{
    QFETCH(bool, disabledContext);
    QFETCH(bool, fence);
    QFETCH(bool, inclusion);
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    QGCMapPolygon polygon;
    polygon.setInteractive(false);
    for (double azimuth : {0.0, 120.0, 240.0}) {
        polygon.appendVertex(QGeoCoordinate(47, 8).atDistanceAndAzimuth(100, azimuth));
    }
    QGCMapCircle circle(QGeoCoordinate(47, 8), 100, false, true);
    circle.setInteractive(false);
    engine->rootContext()->setContextProperty(QStringLiteral("testPolygon"), &polygon);
    engine->rootContext()->setContextProperty(QStringLiteral("testCircle"), &circle);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        Map {
            id: testMap
            width: 640
            height: 480
            plugin: Plugin { name: "QGroundControl" }
            property bool planView: true
            property bool isSatelliteMap: false
            property bool disabledContext: false
            property bool fence: true
            property bool inclusion: true
            property var style: disabledContext ? disabledStyle : null
            QtObject {
                id: disabledStyle
                property bool enabled: false
                property real vehicleId: 77
                property color lineColor: "magenta"
                property real boundaryLineWidth: 9
                property var keepInDashPattern: [1, 9]
                property var keepOutDashPattern: [7, 1]
                property color keepOutFillColor: "cyan"
            }
            function fill(polygon) {
                for (const item of mapItems) {
                    if (polygon ? "path" in item : "radius" in item) {
                        return item
                    }
                }
                return null
            }
            QGCMapPolygonVisuals {
                mapControl: testMap
                mapPolygon: testPolygon
                planStyle: testMap.style
                boundaryKind: testMap.fence ? "polygon" : ""
                inclusion: testMap.inclusion
                borderWidth: testMap.fence ? (inclusion ? 2 : 0) : 5
                borderColor: testMap.fence ? "orange" : "blue"
                interiorColor: testMap.fence ? (inclusion ? "transparent" : "orange") : "green"
                altColor: "purple"
                interiorOpacity: testMap.fence ? (inclusion ? 1 : 0.2) : 0.6
            }
            QGCMapCircleVisuals {
                mapControl: testMap
                mapCircle: testCircle
                planStyle: testMap.style
                boundaryKind: testMap.fence ? "circle" : ""
                inclusion: testMap.inclusion
                borderWidth: testMap.fence ? (inclusion ? 2 : 0) : 5
                borderColor: testMap.fence ? "orange" : "blue"
                interiorColor: testMap.fence ? (inclusion ? "transparent" : "orange") : "green"
                interiorOpacity: testMap.fence ? (inclusion ? 1 : 0.2) : 0.6
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const QVariantMap properties{{QStringLiteral("disabledContext"), disabledContext},
                                 {QStringLiteral("fence"), fence},
                                 {QStringLiteral("inclusion"), inclusion}};
    std::unique_ptr<QObject> root(component.createWithInitialProperties(properties));
    QVERIFY2(root, qPrintable(component.errorString()));
    const auto fill = [&](bool isPolygon) -> QQuickItem* {
        QVariant result;
        if (!QMetaObject::invokeMethod(root.get(), "fill", Q_RETURN_ARG(QVariant, result),
                                       Q_ARG(QVariant, isPolygon))) {
            return nullptr;
        }
        return qvariant_cast<QQuickItem*>(result);
    };
    QTRY_VERIFY_WITH_TIMEOUT(fill(true) && fill(false), TestTimeout::shortMs());
    const QColor border = fence ? QColor(QStringLiteral("orange")) : QColor(QStringLiteral("blue"));
    const QColor interior = fence
                                ? (inclusion ? QColor(QStringLiteral("transparent")) : QColor(QStringLiteral("orange")))
                                : QColor(QStringLiteral("green"));
    for (auto* item : {fill(true), fill(false)}) {
        QCOMPARE(QQmlProperty(item, QStringLiteral("border.width")).read().toReal(),
                 fence ? (inclusion ? 2.0 : 0.0) : 5.0);
        QCOMPARE(QQmlProperty(item, QStringLiteral("border.color")).read().value<QColor>(), border);
        QCOMPARE(item->property("color").value<QColor>(), interior);
        QCOMPARE(item->opacity(), fence ? (inclusion ? 1.0 : 0.2) : 0.6);
    }
    const auto outlineCount = [&] {
        auto* map = qobject_cast<QQuickItem*>(root.get());
        return componentPlanStyleItems(map, QStringLiteral("planKeepInOutline")).size() +
               componentPlanStyleItems(map, QStringLiteral("planKeepOutOutline")).size();
    };
    QCOMPARE(outlineCount(), 0);
    polygon.setShowAltColor(true);
    QCOMPARE(fill(true)->property("color").value<QColor>(), QColor(QStringLiteral("purple")));
    polygon.setVertexDrag(true);
    QCOMPARE(QQmlProperty(fill(true), QStringLiteral("border.width")).read().toReal(), 3.0);
    QCOMPARE(QQmlProperty(fill(true), QStringLiteral("border.color")).read().value<QColor>(),
             QColor(QStringLiteral("orange")));
    const auto moved = QGeoCoordinate(47.002, 8.003);
    polygon.adjustVertex(0, moved);
    QTRY_COMPARE_WITH_TIMEOUT(fill(true)->property("path").toList().first().value<QGeoCoordinate>(), moved,
                              TestTimeout::shortMs());
    QCOMPARE(outlineCount(), 0);
    polygon.setVertexDrag(false);
    QCOMPARE(QQmlProperty(fill(true), QStringLiteral("border.width")).read().toReal(),
             fence ? (inclusion ? 2.0 : 0.0) : 5.0);
}

void PlanStyleComponentTest::_stockEditorVisuals_data()
{
    QTest::addColumn<bool>("disabledContext");
    QTest::addColumn<bool>("px4");
    QTest::addColumn<bool>("absolute");
    QTest::newRow("null-ardupilot") << false << false << true;
    QTest::newRow("disabled-ardupilot") << true << false << true;
    QTest::newRow("null-px4") << false << true << true;
    QTest::newRow("disabled-px4") << true << true << true;
    QTest::newRow("null-ardupilot-relative") << false << false << false;
    QTest::newRow("disabled-ardupilot-relative") << true << false << false;
    QTest::newRow("null-px4-relative") << false << true << false;
    QTest::newRow("disabled-px4-relative") << true << true << false;
}

void PlanStyleComponentTest::_stockEditorVisuals()
{
    QFETCH(bool, disabledContext);
    QFETCH(bool, px4);
    QFETCH(bool, absolute);
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    auto* settings = SettingsManager::instance()->appSettings();
    auto* firmware = settings->offlineEditingFirmwareClass();
    auto* vehicleClass = settings->offlineEditingVehicleClass();
    const auto savedFirmware = firmware->rawValue();
    const auto savedVehicleClass = vehicleClass->rawValue();
    const auto restoreSettings = qScopeGuard([=] {
        firmware->setRawValue(savedFirmware);
        vehicleClass->setRawValue(savedVehicleClass);
    });
    const auto autopilot = px4 ? MAV_AUTOPILOT_PX4 : MAV_AUTOPILOT_ARDUPILOTMEGA;
    // Empty-mission initialization makes even the explicit test vehicle track these settings again.
    firmware->setRawValue(QGCMAVLink::firmwareClass(autopilot));
    vehicleClass->setRawValue(QGCMAVLink::vehicleClass(MAV_TYPE_QUADROTOR));
    PlanMasterController master(autopilot, MAV_TYPE_QUADROTOR);
    QVERIFY(master.controllerVehicle());
    master.controllerVehicle()->setParent(&master);
    master.start();
    QVERIFY(master.controllerVehicle());
    QVERIFY2(master.offline() && master.controllerVehicle()->firmwareType() == autopilot,
             qPrintable(QStringLiteral("Expected offline %1 editor, got firmware %2")
                            .arg(px4 ? QStringLiteral("PX4") : QStringLiteral("ArduPilot"))
                            .arg(master.controllerVehicle()->firmwareType())));
    QCOMPARE(master.controllerVehicle()->px4Firmware(), px4);
    const QGeoCoordinate home(47.397742, 8.545594, 120);
    auto* fence = master.geoFenceController();
    const auto topLeft = home.atDistanceAndAzimuth(300, 315);
    const auto bottomRight = home.atDistanceAndAzimuth(300, 135);
    for (bool inclusion : {true, false}) {
        fence->addInclusionPolygon(topLeft, bottomRight);
        auto* polygon = fence->polygons()->value<QGCFencePolygon*>(fence->polygons()->count() - 1);
        polygon->setInclusion(inclusion);
        fence->addInclusionCircle(topLeft, bottomRight);
        fence->circles()->value<QGCFenceCircle*>(fence->circles()->count() - 1)->setInclusion(inclusion);
    }
    fence->clearAllInteractive();
    auto* rally = master.rallyPointController();
    rally->addPoint(home.atDistanceAndAzimuth(100, 90));
    rally->addPoint(home.atDistanceAndAzimuth(200, 90));
    auto* point = rally->points()->value<RallyPoint*>(0);
    QVERIFY(point);
    for (int i = 0; i < rally->points()->count(); ++i) {
        rally->points()->value<RallyPoint*>(i)->changeAltitudeFrame(QGroundControlQmlGlobal::AltitudeFrameRelative);
    }
    const auto frame =
        absolute ? QGroundControlQmlGlobal::AltitudeFrameAbsolute : QGroundControlQmlGlobal::AltitudeFrameRelative;
    // Changing to the current frame cancels terrain following but does not apply the seed altitude.
    point->altitude()->setRawValue(137);
    point->setAltitudeFrame(frame);
    QCOMPARE(point->coordinate().altitude(), 137.0);
    QCOMPARE(point->altitudeFrame(), frame);
    QVERIFY(!point->terrainQueryPending());
    rally->setCurrentRallyPoint(point);
    engine->rootContext()->setContextProperty(QStringLiteral("testFence"), fence);
    engine->rootContext()->setContextProperty(QStringLiteral("testRally"), rally);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        Map {
            id: testMap
            width: 640
            height: 480
            center: QtPositioning.coordinate(47.397742, 8.545594)
            zoomLevel: 16
            plugin: Plugin { name: "QGroundControl" }
            property bool planView: true
            property bool isSatelliteMap: false
            property rect centerViewport: Qt.rect(0, 0, width, height)
            property int _editingLayer: 3
            property int _layerRally: 3
            property bool disabledContext: false
            property var style: disabledContext ? disabledStyle : null
            readonly property color ordinary: palette.mapIndicator
            QGCPalette { id: palette }
            QtObject { id: mainWindow; function allowViewSwitch() { return true } }
            QtObject {
                id: disabledStyle
                property bool enabled: false
                property real vehicleId: 77
                property color lineColor: "magenta"
                property real boundaryLineWidth: 9
                property var keepInDashPattern: [1, 9]
                property var keepOutDashPattern: [7, 1]
                property color keepOutFillColor: "cyan"
                property color markerBorderColor: "cyan"
                property real markerBorderWidth: 7
                property color markerTextColor: "yellow"
            }
            GeoFenceMapVisuals {
                map: testMap
                myGeoFenceController: testFence
                homePosition: QtPositioning.coordinate(47.397742, 8.545594)
                planView: true
                planStyle: testMap.style
                opacity: 0.5
            }
            RallyPointMapVisuals {
                map: testMap
                myRallyPointController: testRally
                planView: true
                interactive: true
                planStyle: testMap.style
                opacity: 0.5
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(
        component.createWithInitialProperties({{QStringLiteral("disabledContext"), disabledContext}}));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    const auto named = [map](const QString& name) { return componentPlanStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planFenceFill")).size(), 5, TestTimeout::shortMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planRallyPoint")).size(), 2, TestTimeout::shortMs());
    QVERIFY(named(QStringLiteral("planKeepInOutline")).isEmpty());
    QVERIFY(named(QStringLiteral("planKeepOutOutline")).isEmpty());
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        const bool inclusion = fill->property("inclusion").toBool();
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), inclusion ? 2.0 : 0.0);
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.color")).read().value<QColor>(),
                 QColor(QStringLiteral("orange")));
        QCOMPARE(fill->property("color").value<QColor>(),
                 QColor(inclusion ? QStringLiteral("transparent") : QStringLiteral("orange")));
        QCOMPARE(fill->opacity(), inclusion ? 0.5 : 0.1);
    }
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        const bool selected = qvariant_cast<QObject*>(marker->property("rallyPointObject")) == point;
        QCOMPARE(marker->property("markerShape").toString(), QStringLiteral("circle"));
        QCOMPARE(marker->property("label").toString(), QStringLiteral("R"));
        QCOMPARE(marker->property("markerBorderWidth").toReal(), 0.0);
        QCOMPARE(marker->property("markerColor").value<QColor>(),
                 selected ? QColor(QStringLiteral("green")) : map->property("ordinary").value<QColor>());
        QCOMPARE(marker->property("checked").toBool(), selected);
        QCOMPARE(marker->opacity(), 0.5);
        auto* loader = qvariant_cast<QQuickItem*>(marker->property("sourceItem"));
        QVERIFY(loader);
        auto* source = qvariant_cast<QQuickItem*>(loader->property("item"));
        QVERIFY(source);
        QCOMPARE(source->property("renderedTextColor").value<QColor>(), QColor(QStringLiteral("white")));
        const auto halos = componentPlanStyleItems(source, QStringLiteral("planWaypointSelectionHalo"));
        QCOMPARE(halos.size(), 1);
        QCOMPARE(halos.first()->isVisible(), selected);
    }
    QQuickItem* drag = nullptr;
    for (auto* item : map->childItems()) {
        if (qvariant_cast<QObject*>(item->property("rallyPointObject")) == point &&
            item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            drag = item;
        }
    }
    QVERIFY(drag && drag->isVisible());
    auto moved = point->coordinate().atDistanceAndAzimuth(30, 45);
    moved.setAltitude(-500);
    QVERIFY(drag->setProperty("itemCoordinate", QVariant::fromValue(moved)));
    QCOMPARE(point->coordinate().altitude(), 137.0);
    QVERIFY(point->coordinate().distanceTo(moved) < 0.01);
    QVERIFY(!point->terrainQueryPending());
    QVERIFY(QMetaObject::invokeMethod(drag, "dragStop"));
    QCOMPARE(point->terrainQueryPending(), px4 && absolute);
    if (px4 && absolute) {
        QTRY_VERIFY_WITH_TIMEOUT(!point->terrainQueryPending(), TestTimeout::mediumMs());
        QVERIFY(!point->terrainQueryFailed());
        // The unit-test tile provider returns zero terrain height outside its Point Nemo regions.
        QCOMPARE(point->coordinate().altitude(), 0.0);
    } else {
        QCOMPARE(point->coordinate().altitude(), 137.0);
    }
    QCOMPARE(point->altitudeFrame(), frame);
    QVERIFY(point->coordinate().distanceTo(moved) < 0.01);
    point->changeAltitudeFrame(QGroundControlQmlGlobal::AltitudeFrameRelative, 137);
    QVERIFY(!point->terrainQueryPending());
    QVERIFY(map->setProperty("_editingLayer", 1));
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        QVERIFY(!marker->property("checked").toBool());
    }
}

UT_REGISTER_TEST(PlanStyleComponentTest, TestLabel::Unit)
