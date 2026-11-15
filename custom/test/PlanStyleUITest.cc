#include "PlanStyleUITest.h"

#include <memory>
#include <vector>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaProperty>
#include <QtCore/QRectF>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QStyleHints>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "Fact.h"
#include "FirmwarePlugin.h"
#include "FixedWingLandingComplexItem.h"
#include "FlightPathSegment.h"
#include "GeoFenceController.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "MissionController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MissionSettingsItem.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PlanManager.h"
#include "PlanMasterController.h"
#include "QGCCorePlugin.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCMapCircle.h"
#include "QGCMapPolygon.h"
#include "QGCPlanStyle.h"
#include "QGroundControlQmlGlobal.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "TakeoffMissionItem.h"
#include "VTOLLandingComplexItem.h"
#include "Vehicle.h"
#include "VehicleRoleController.h"
#include "VisualMissionItem.h"

namespace {
void planStyleWarmUpScreenTools(QQmlApplicationEngine* engine)
{
    // Match main-window singleton initialization so the stock GCS image's height binding cannot re-enter itself.
    QQmlComponent component(engine);
    component.setData(R"(
        import QtQuick
        import QGroundControl.Controls
        Item { property real h: ScreenTools.defaultFontPixelHeight }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
}

QList<QQuickItem*> planStyleItems(QQuickItem* root, const QString& name)
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

QQuickItem* planStyleTab(QQuickItem* root, const QString& text)
{
    if (!root || !root->isVisible()) {
        return nullptr;
    }
    if (root->property("text").toString() == text && root->metaObject()->indexOfSignal("clicked()") >= 0) {
        return root;
    }
    for (auto* child : root->childItems()) {
        if (auto* result = planStyleTab(child, text)) {
            return result;
        }
    }
    return nullptr;
}

QColor planStyleColor(QObject* object, const QString& property)
{
    return QQmlProperty(object, property).read().value<QColor>();
}

QQuickItem* planStyleSource(QQuickItem* item)
{
    return item ? qvariant_cast<QQuickItem*>(item->property("sourceItem")) : nullptr;
}

bool planStyleRenderMap(QQuickItem* map)
{
    auto* window = map->window();
    return window && window->isExposed() && !window->grabWindow().isNull();
}

bool planStyleProjectedPosition(QQuickItem* map, const QGeoCoordinate& coordinate, const QPointF& scenePosition)
{
    QPointF projected;
    return coordinate.isValid() &&
           QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, projected),
                                     Q_ARG(QGeoCoordinate, coordinate), Q_ARG(bool, false)) &&
           (scenePosition - map->mapToScene(projected)).manhattanLength() < 1;
}

bool planStyleMapItemProjected(QQuickItem* map, QQuickItem* item)
{
    auto* source = planStyleSource(item);
    return source && source->window() == map->window() &&
           planStyleProjectedPosition(map, item->property("coordinate").value<QGeoCoordinate>(),
                                      source->mapToScene(item->property("anchorPoint").toPointF()));
}

void planStyleCheckBoundaries(QQuickItem* map, quint32 vehicleId, const QColor& color, int polygonCount,
                              int circleCount)
{
    const auto fills = planStyleItems(map, QStringLiteral("planFenceFill"));
    const auto keepIn = planStyleItems(map, QStringLiteral("planKeepInOutline"));
    const auto keepOut = planStyleItems(map, QStringLiteral("planKeepOutOutline"));
    QCOMPARE(fills.size(), polygonCount + circleCount + 1);
    QCOMPARE(keepIn.size() + keepOut.size(), fills.size());
    QSet<QQuickItem*> matched;
    int polygons = 0;
    int circles = 0;
    int parameterCircles = 0;
    for (auto* fill : fills) {
        QCOMPARE(fill->property("vehicleId").toUInt(), vehicleId);
        const auto kind = fill->property("boundaryKind").toString();
        const bool inclusion = fill->property("inclusion").toBool();
        const auto outlines = inclusion ? keepIn : keepOut;
        QQuickItem* outline = nullptr;
        for (auto* candidate : outlines) {
            if (matched.contains(candidate) || candidate->property("boundaryKind").toString() != kind) {
                continue;
            }
            if (kind == QStringLiteral("polygon")) {
                if (candidate->property("vertices").toList() != fill->property("path").toList()) {
                    continue;
                }
            } else {
                const auto vertices = candidate->property("vertices").toList();
                const auto center = fill->property("center").value<QGeoCoordinate>();
                const double radius = fill->property("radius").toDouble();
                if (center.isValid() && radius > 0 &&
                    (vertices.size() != 120 || vertices.first().value<QGeoCoordinate>().distanceTo(
                                                   center.atDistanceAndAzimuth(radius, 0)) > 0.01)) {
                    continue;
                }
            }
            outline = candidate;
            break;
        }
        QVERIFY(outline);
        matched.insert(outline);
        QTRY_VERIFY_WITH_TIMEOUT(!outline->property("_projectionPending").toBool(), TestTimeout::shortMs());
        QCOMPARE(outline->property("vehicleId").toUInt(), vehicleId);
        QCOMPARE(outline->property("lineColor").value<QColor>(), color);
        QCOMPARE(outline->property("renderedStrokeColor").value<QColor>(), color);
        QCOMPARE(outline->property("lineWidth").toReal(), 3.0);
        QCOMPARE(outline->property("renderedStrokeWidth").toReal(), 3.0);
        const QVariantList dashes = inclusion ? QVariantList{6.0, 3.0} : QVariantList{2.0, 2.0};
        QCOMPARE(outline->property("dashPattern").toList(), dashes);
        QCOMPARE(outline->property("renderedDashPattern").toList(), dashes);
        auto* shape = planStyleSource(outline);
        QVERIFY(shape);
        QObject* stroke = nullptr;
        for (auto* child : shape->findChildren<QObject*>()) {
            if (child->metaObject()->indexOfProperty("strokeStyle") >= 0) {
                stroke = child;
                break;
            }
        }
        QVERIFY(stroke);
        const auto styleEnum = stroke->metaObject()->enumerator(stroke->metaObject()->indexOfEnumerator("StrokeStyle"));
        QVERIFY(styleEnum.isValid());
        QCOMPARE(stroke->property("strokeStyle").toInt(), styleEnum.keyToValue("DashLine"));
        QCOMPARE(outline->property("renderedStrokeStyle").toInt(), stroke->property("strokeStyle").toInt());
        QCOMPARE(stroke->property("strokeColor").value<QColor>(), color);
        QCOMPARE(stroke->property("strokeWidth").toReal(), 3.0);
        QCOMPARE(stroke->property("dashPattern").toList(), dashes);
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), 0.0);
        const auto fillColor = fill->property("color").value<QColor>();
        if (inclusion) {
            QCOMPARE(fillColor.alpha(), 0);
        } else {
            QCOMPARE(fillColor.red(), 229);
            QCOMPARE(fillColor.green(), 57);
            QCOMPARE(fillColor.blue(), 53);
            QVERIFY(qAbs(fillColor.alphaF() - 0.35) < 0.005);
        }
        QCOMPARE(fill->opacity(), outline->opacity());
        QVERIFY(outline->z() > fill->z());
        QVERIFY(outline->z() < 47);
        QVERIFY(outline->z() < 51);
        for (auto* route : planStyleItems(map, QStringLiteral("planMissionRoute"))) {
            QVERIFY(outline->z() < route->z());
        }
        QCOMPARE(outline->acceptedMouseButtons(), Qt::NoButton);
        QCOMPARE(shape->acceptedMouseButtons(), Qt::NoButton);
        if (kind == QStringLiteral("polygon")) {
            ++polygons;
        } else if (kind == QStringLiteral("circle")) {
            ++circles;
        } else {
            QCOMPARE(kind, QStringLiteral("parameterCircle"));
            ++parameterCircles;
        }
        const auto vertices = outline->property("vertices").toList();
        if (vertices.isEmpty()) {
            QVERIFY(!outline->isVisible());
            continue;
        }
        const auto projected = [&] { return outline->property("_screenPoints").toList(); };
        // MapQuickItem placement is polished by the whole map's render pass, not by its source alone.
        QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map) && !outline->property("_projectionPending").toBool() &&
                                     planStyleMapItemProjected(map, outline),
                                 TestTimeout::shortMs());
        QTRY_COMPARE_WITH_TIMEOUT(projected().size(), vertices.size() + 1, TestTimeout::shortMs());
        QTRY_COMPARE_WITH_TIMEOUT(shape->property("points").toList(), projected(), TestTimeout::shortMs());
        const QPointF anchor = outline->property("anchorPoint").toPointF();
        QCOMPARE(outline->property("coordinate").value<QGeoCoordinate>(), vertices.first().value<QGeoCoordinate>());
        QCOMPARE(projected().first().toPointF(), anchor);
        QCOMPARE(projected().last().toPointF(), anchor);
        QPointF origin;
        QVERIFY(QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, origin),
                                          Q_ARG(QGeoCoordinate, vertices.first().value<QGeoCoordinate>()),
                                          Q_ARG(bool, false)));
        const auto expectedScene = map->mapToScene(origin);
        QTRY_VERIFY_WITH_TIMEOUT((shape->mapToScene(anchor) - expectedScene).manhattanLength() < 1,
                                 TestTimeout::shortMs());
        const QRectF bounds(0, 0, shape->width(), shape->height());
        // Edge vertices can round one ulp beyond the extent used to size the Shape.
        const auto tolerantBounds = bounds.adjusted(-0.5, -0.5, 0.5, 0.5);
        for (const auto& point : projected()) {
            QVERIFY(tolerantBounds.contains(point.toPointF()));
        }
        for (int i = 0; i < vertices.size(); ++i) {
            QPointF position;
            const auto vertex = vertices[i].value<QGeoCoordinate>();
            QVERIFY(QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, position),
                                              Q_ARG(QGeoCoordinate, vertex), Q_ARG(bool, false)));
            QTRY_VERIFY_WITH_TIMEOUT((projected()[i].toPointF() - anchor - position + origin).manhattanLength() < 0.05,
                                     TestTimeout::shortMs());
            if (kind != QStringLiteral("polygon")) {
                const auto center = fill->property("center").value<QGeoCoordinate>();
                const double radius = fill->property("radius").toDouble();
                QVERIFY(vertex.distanceTo(center.atDistanceAndAzimuth(radius, i * 3)) < 0.01);
            }
        }
    }
    QCOMPARE(polygons, polygonCount);
    QCOMPARE(circles, circleCount);
    QCOMPARE(parameterCircles, 1);
}

void planStyleCheckRally(QQuickItem* map, QQuickItem* marker, quint32 vehicleId, const QColor& color)
{
    QVERIFY(marker);
    // Render before reading source geometry; offscreen Fly markers need not be pointer targets.
    QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map), TestTimeout::shortMs());
    QCOMPARE(marker->property("vehicleId").toUInt(), vehicleId);
    QCOMPARE(marker->property("markerShape").toString(), QStringLiteral("diamond"));
    QCOMPARE(marker->property("markerColor").value<QColor>(), color);
    QCOMPARE(marker->property("markerBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
    QCOMPARE(marker->property("markerBorderWidth").toReal(), 2.0);
    QCOMPARE(marker->property("label").toString(), QStringLiteral("R"));
    auto* loader = planStyleSource(marker);
    QVERIFY(loader);
    auto* source = qvariant_cast<QQuickItem*>(loader->property("item"));
    QVERIFY(source);
    QCOMPARE(source->property("anchorPointX").toReal(), source->width() / 2);
    QCOMPARE(source->property("anchorPointY").toReal(), source->height() / 2);
    QCOMPARE(loader->property("anchorPointX").toReal(), source->property("anchorPointX").toReal());
    QCOMPARE(loader->property("anchorPointY").toReal(), source->property("anchorPointY").toReal());
    QQuickItem* diamond = nullptr;
    QQuickItem* text = nullptr;
    for (auto* child : source->childItems()) {
        if (child->rotation() == 45) {
            diamond = child;
        }
        if (child->property("text").toString() == QStringLiteral("R")) {
            text = child;
        }
    }
    QVERIFY(diamond && text);
    QCOMPARE(diamond->width(), source->width() * 0.72);
    QCOMPARE(diamond->height(), diamond->width());
    QCOMPARE(diamond->property("radius").toReal(), 0.0);
    QCOMPARE(diamond->property("color").value<QColor>(), color);
    QCOMPARE(planStyleColor(diamond, QStringLiteral("border.color")), QColor(QStringLiteral("white")));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.width")).read().toReal(), 2.0);
    QCOMPARE(text->rotation(), 0.0);
    QCOMPARE(source->rotation(), 0.0);
    QCOMPARE(loader->rotation(), 0.0);
    QCOMPARE(marker->rotation(), 0.0);
    QCOMPARE(text->property("color").value<QColor>(), QColor(QStringLiteral("black")));
    QVERIFY(QQmlProperty(text, QStringLiteral("font.bold")).read().toBool());
    const auto halos = planStyleItems(marker, QStringLiteral("planRallySelectionHalo"));
    QCOMPARE(halos.size(), 1);
    QCOMPARE(halos.first()->isVisible(), marker->property("checked").toBool());
    QVERIFY(halos.first()->width() > source->width());
    QVERIFY(QQmlProperty(halos.first(), QStringLiteral("border.width")).read().toReal() > 0);
    QVERIFY(planStyleColor(halos.first(), QStringLiteral("border.color")).alpha() > 0);
}

qreal planStyleOpacityBelowMap(QQuickItem* item, QQuickItem* map)
{
    qreal opacity = 1;
    while (item && item != map) {
        opacity *= item->opacity();
        item = item->parentItem();
    }
    return item == map ? opacity : qQNaN();
}

QQuickItem* planStyleMouseItemAt(QQuickItem* item, const QPointF& position)
{
    if (!item || !item->isVisible() || !item->isEnabled()) {
        return nullptr;
    }
    for (auto* child : item->childItems()) {
        if (auto* hit = planStyleMouseItemAt(child, position)) {
            return hit;
        }
    }
    return item->acceptedMouseButtons().testFlag(Qt::LeftButton) && item->contains(item->mapFromScene(position))
               ? item
               : nullptr;
}

bool planStyleEditorProjected(QQuickItem* map)
{
    if (!planStyleRenderMap(map)) {
        return false;
    }
    const auto projected = [map](QQuickItem* item) {
        if (!item->isVisible() || !item->property("coordinate").value<QGeoCoordinate>().isValid()) {
            return true;
        }
        if (item->metaObject()->indexOfProperty("_screenPoints") >= 0) {
            if (item->property("_projectionPending").toBool()) {
                return false;
            }
            // Tracing has no drawable outline until its second vertex.
            if (item->property("vertices").toList().size() < 2) {
                return true;
            }
        }
        return planStyleMapItemProjected(map, item);
    };
    for (auto* item : map->childItems()) {
        if (!item->isVisible()) {
            continue;
        }
        if (planStyleSource(item) && !projected(item)) {
            return false;
        }
        if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            auto* indicator = qvariant_cast<QQuickItem*>(item->property("itemIndicator"));
            if (!indicator || !indicator->isVisible()) {
                continue;
            }
            // Idle polygon draggers keep their creation coordinate while the indicator follows model edits.
            const auto coordinate = (item->property("_dragStartSignalled").toBool() ? item->property("itemCoordinate")
                                                                                    : indicator->property("coordinate"))
                                        .value<QGeoCoordinate>();
            if (!coordinate.isValid()) {
                continue;
            }
            const auto anchor = indicator->property("anchorPoint").toPointF() +
                                QPointF(item->property("_touchMarginHorizontal").toReal(),
                                        item->property("_touchMarginVertical").toReal());
            if (!planStyleProjectedPosition(map, coordinate, item->mapToScene(anchor))) {
                return false;
            }
        }
    }
    for (const auto& name :
         {QStringLiteral("planWaypoint"), QStringLiteral("planHomeMarker"), QStringLiteral("planRallyPoint"),
          QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline")}) {
        for (auto* item : planStyleItems(map, name)) {
            if (!projected(item)) {
                return false;
            }
        }
    }
    return true;
}
}  // namespace

void PlanStyleUITest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    QDir directory(_directory.path());
    QVERIFY(directory.removeRecursively());
    QVERIFY(directory.mkpath(QStringLiteral(".")));
    QSettings().remove(QStringLiteral("COP"));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void PlanStyleUITest::cleanup()
{
    if (auto* controller = _controller()) {
        controller->cancelControl();
    }
    for (const auto& link : _links) {
        if (link) {
            link->disconnect();
        }
    }
    const bool removed = waitForCondition([] { return MultiVehicleManager::instance()->vehicles()->count() == 0; },
                                          TestTimeout::longMs(), QStringLiteral("plan style vehicles removed"));
    const bool inactive = waitForCondition([] { return MultiVehicleManager::instance()->activeVehicle() == nullptr; },
                                           TestTimeout::longMs(), QStringLiteral("plan style active vehicle cleared"));
    _links.clear();
    _configs.clear();
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    QmlUITestBase::cleanup();
    QVERIFY(removed);
    QVERIFY(inactive);
}

COPController* PlanStyleUITest::_controller() const
{
    return _engine ? _engine->singletonInstance<COPController*>("QGC", "COPController") : nullptr;
}

COPVehicle* PlanStyleUITest::_entryFor(quint32 sysid) const
{
    auto* controller = _controller();
    if (controller) {
        for (int i = 0; i < controller->vehicles()->count(); ++i) {
            auto* entry = controller->vehicles()->value<COPVehicle*>(i);
            if (entry && entry->sysid() == sysid) {
                return entry;
            }
        }
    }
    return nullptr;
}

MockLink* PlanStyleUITest::_start(MAV_TYPE type)
{
    if (_links.isEmpty()) {
        auto* manager = MultiVehicleManager::instance();
        connect(manager, &MultiVehicleManager::vehicleAdded, _window, [this, manager](Vehicle* vehicle) {
            if (vehicle && manager->vehicles()->count() > 1) {
                expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle %1").arg(vehicle->id())));
                QTimer::singleShot(0, _window, [this] {
                    QVERIFY(acceptDialog());
                    verifyExpectedLogMessage();
                });
            }
        });
    }
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("Plan style %1").arg(_configs.size()));
    config->setDynamic(true);
    config->setFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    config->setVehicleType(type);
    config->setIncrementVehicleId(true);
    SharedLinkConfigurationPtr shared = config;
    _configs.append(shared);
    if (!LinkManager::instance()->createConnectedLink(shared)) {
        return nullptr;
    }
    auto* link = qobject_cast<MockLink*>(config->link());
    _links.append(link);
    return link;
}

bool PlanStyleUITest::_selectTab(quint32 sysid)
{
    auto* entry = _entryFor(sysid);
    auto* tab = entry ? planStyleTab(_rootItem, entry->label()) : nullptr;
    return tab && _clickItemAt(tab, 0.5, 0.5, entry->label());
}

void PlanStyleUITest::_flyPlans_data()
{
    QTest::addColumn<bool>("grouped");
    QTest::newRow("separated-waypoints") << false;
    QTest::newRow("grouped-waypoints") << true;
}

void PlanStyleUITest::_flyPlans()
{
    QFETCH(bool, grouped);
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* controller = _controller();
    QVERIFY(controller);
    auto* policy = QGCCorePlugin::instance()->planStyle();
    QVERIFY(policy);
    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> first = _start(MAV_TYPE_GROUND_ROVER);
    QPointer<MockLink> second = _start(MAV_TYPE_QUADROTOR);
    QVERIFY(first && second);
    QVERIFY(first->vehicleId() != 0);
    QVERIFY(second->vehicleId() != 0);
    QVERIFY(first->vehicleId() != second->vehicleId());
    auto* manager = MultiVehicleManager::instance();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    QList<Vehicle*> vehicles;
    QList<QList<QGeoCoordinate>> uploaded;
    for (const auto& link : {first, second}) {
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(link->vehicleId()) && _entryFor(link->vehicleId())->vehicle(),
                                 TestTimeout::longMs());
        auto* vehicle = _entryFor(link->vehicleId())->vehicle();
        vehicles.append(vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->isInitialConnectComplete(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->missionManager()->inProgress(), TestTimeout::longMs());
        QList<MissionItem*> items;
        QList<QGeoCoordinate> coordinates;
        const int count = vehicle == vehicles.first() ? 4 : 6;
        for (int i = 0; i < count; ++i) {
            const int step = grouped && i == 2 ? 1 : i;
            const auto coordinate = vehicle->homePosition().atDistanceAndAzimuth(300 * step, count == 4 ? 90 : 180);
            coordinates.append(coordinate);
            items.append(new MissionItem(i, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, 0,
                                         coordinate.latitude(), coordinate.longitude(), 100, true, false, this));
        }
        uploaded.append(coordinates);
        QSignalSpy sent(vehicle->missionManager(), &PlanManager::sendComplete);
        QVERIFY(sent.isValid());
        vehicle->missionManager()->writeMissionItems(items);
        QTRY_VERIFY_WITH_TIMEOUT(sent.count() >= 1, TestTimeout::longMs());
        QVERIFY(!sent.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->missionManager()->inProgress(), TestTimeout::longMs());
        QmlObjectListModel polygons;
        QmlObjectListModel circles;
        for (bool inclusion : {true, false}) {
            auto* polygon = new QGCFencePolygon(inclusion);
            for (double azimuth : {0.0, 120.0, 240.0}) {
                polygon->appendVertex(vehicle->homePosition().atDistanceAndAzimuth(
                    inclusion ? 400 + count * 10 : 70 + count * 10, azimuth));
            }
            polygons.append(polygon);
            circles.append(
                new QGCFenceCircle(vehicle->homePosition().atDistanceAndAzimuth(count * 50, inclusion ? 90 : 180),
                                   inclusion ? 90 : 30, inclusion));
        }
        QSignalSpy fenceSent(vehicle->geoFenceManager(), &GeoFenceManager::sendComplete);
        QVERIFY(fenceSent.isValid());
        vehicle->geoFenceManager()->sendToVehicle(vehicle->homePosition(), polygons, circles);
        polygons.clearAndDeleteContents();
        circles.clearAndDeleteContents();
        QTRY_VERIFY_WITH_TIMEOUT(!fenceSent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!fenceSent.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->geoFenceManager()->inProgress(), TestTimeout::longMs());
        QList<RallyPointData> rallyPoints;
        for (int i = 0; i < count - 1; ++i) {
            auto coordinate = vehicle->homePosition().atDistanceAndAzimuth(100 + i * 60, count == 4 ? 45 : 225);
            coordinate.setAltitude(80 + i * 10);
            rallyPoints.append({coordinate, MAV_FRAME_GLOBAL_RELATIVE_ALT});
        }
        QSignalSpy rallySent(vehicle->rallyPointManager(), &RallyPointManager::sendComplete);
        QVERIFY(rallySent.isValid());
        vehicle->rallyPointManager()->sendToVehicle(rallyPoints);
        QTRY_VERIFY_WITH_TIMEOUT(!rallySent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!rallySent.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->rallyPointManager()->inProgress(), TestTimeout::longMs());
        auto* parameters = vehicle->parameterManager();
        QVERIFY(parameters);
        if (parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))) {
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS"))
                ->setRawValue(500 + count);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE"))
                ->setRawValue(2);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))
                ->setRawValue(1);
        }
    }
    QVERIFY(_entryFor(vehicles[0]->id())->color() != _entryFor(vehicles[1]->id())->color());
    const auto loaders = [this] { return planStyleItems(_rootItem, QStringLiteral("flyViewVehiclePlan")); };
    const auto loaderFor = [&](Vehicle* vehicle) -> QQuickItem* {
        for (auto* loader : loaders()) {
            if (qvariant_cast<QObject*>(loader->parentItem()->property("_vehicle")) == vehicle) {
                return loader;
            }
        }
        return nullptr;
    };
    const auto visualsFor = [&](Vehicle* vehicle) -> QObject* {
        auto* loader = loaderFor(vehicle);
        return loader ? qvariant_cast<QObject*>(loader->property("item")) : nullptr;
    };
    QTRY_COMPARE_WITH_TIMEOUT(loaders().size(), 2, TestTimeout::longMs());
    QList<QPointer<PlanMasterController>> controllers(2);
    QList<QPointer<QmlObjectListModel>> models(2);
    QList<QPointer<QmlObjectListModel>> segmentModels(2);
    QList<int> downloads(2);
    // Capture each retained controller only after its uploaded plan has reached the Fly model.
    for (int v = 0; v < 2; ++v) {
        auto* entry = _entryFor(vehicles[v]->id());
        QVERIFY(entry);
        QCOMPARE(entry->vehicle(), vehicles[v]);
        QVERIFY(entry->connected());
        controller->selectVehicle(vehicles[v]->id());
        QCOMPARE(controller->selected(), entry);
        controller->assumeControl();
        QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), vehicles[v], TestTimeout::longMs());
        QVERIFY(_selectTab(vehicles[v]->id()));
        QTRY_VERIFY_WITH_TIMEOUT(visualsFor(vehicles[v]), TestTimeout::longMs());
        auto* master = qvariant_cast<PlanMasterController*>(visualsFor(vehicles[v])->property("planMasterController"));
        QVERIFY(master);
        controllers[v] = master;
        QTRY_COMPARE_WITH_TIMEOUT(master->missionController()->visualItems()->count(), uploaded[v].size(),
                                  TestTimeout::longMs());
        models[v] = master->missionController()->visualItems();
        segmentModels[v] = master->missionController()->simpleFlightPathSegments();
        QTRY_VERIFY_WITH_TIMEOUT(segmentModels[v]->count() > 0, TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicles[v]->missionManager()->inProgress(), TestTimeout::longMs());
        downloads[v] = _links[v]->receivedMissionRequestListCount(MAV_MISSION_TYPE_MISSION);
    }
    QPointer<PlanMasterController> retainedFenceMaster;
    QPointer<QmlObjectListModel> retainedFencePolygons;
    QPointer<QmlObjectListModel> retainedFenceCircles;
    QPointer<QmlObjectListModel> retainedRallyPoints;
    const QList<int> fenceDownloads{first->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE),
                                    second->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE)};
    const QList<int> rallyDownloads{first->receivedMissionRequestListCount(MAV_MISSION_TYPE_RALLY),
                                    second->receivedMissionRequestListCount(MAV_MISSION_TYPE_RALLY)};
    for (int v : {0, 1, 0}) {
        auto* vehicle = vehicles[v];
        auto* entry = _entryFor(vehicle->id());
        QVERIFY(entry);
        QCOMPARE(entry->vehicle(), vehicle);
        QVERIFY(entry->connected());
        controller->selectVehicle(vehicle->id());
        QCOMPARE(controller->selected(), entry);
        controller->assumeControl();
        QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), vehicle, TestTimeout::longMs());
        QVERIFY(_selectTab(vehicle->id()));
        QTRY_VERIFY_WITH_TIMEOUT(visualsFor(vehicle), TestTimeout::longMs());
        QVERIFY(!loaderFor(vehicles[1 - v])->property("active").toBool());
        QVERIFY(!visualsFor(vehicles[1 - v]));
        QCOMPARE(qvariant_cast<PlanMasterController*>(visualsFor(vehicle)->property("planMasterController")),
                 controllers[v].data());
        for (int retained = 0; retained < 2; ++retained) {
            QVERIFY(controllers[retained]);
            QCOMPARE(controllers[retained]->missionController()->visualItems(), models[retained].data());
            QCOMPARE(controllers[retained]->missionController()->simpleFlightPathSegments(),
                     segmentModels[retained].data());
            QCOMPARE(_links[retained]->receivedMissionRequestListCount(MAV_MISSION_TYPE_MISSION), downloads[retained]);
        }
        auto* map = qvariant_cast<QQuickItem*>(visualsFor(vehicle)->property("map"));
        QVERIFY(map);
        QTRY_VERIFY_WITH_TIMEOUT(map->isVisible() && map->property("mapReady").toBool(), TestTimeout::longMs());
        const QColor expected = _entryFor(vehicle->id())->color();
        const auto routes = [&] { return planStyleItems(map, QStringLiteral("planMissionRoute")); };
        const auto markers = [&] { return planStyleItems(map, QStringLiteral("planWaypoint")); };
        const auto homes = [&] { return planStyleItems(map, QStringLiteral("planHomeMarker")); };
        QTRY_COMPARE_WITH_TIMEOUT(routes().size(), segmentModels[v]->count(), TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(markers().size(), uploaded[v].size() - 1, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(homes().size(), 1, TestTimeout::longMs());
        auto* fenceMaster = qvariant_cast<PlanMasterController*>(map->property("_planMasterController"));
        QVERIFY(fenceMaster);
        QCOMPARE(fenceMaster->managerVehicle(), vehicle);
        if (!retainedFenceMaster) {
            retainedFenceMaster = fenceMaster;
            retainedFencePolygons = fenceMaster->geoFenceController()->polygons();
            retainedFenceCircles = fenceMaster->geoFenceController()->circles();
            retainedRallyPoints = fenceMaster->rallyPointController()->points();
        }
        QCOMPARE(fenceMaster, retainedFenceMaster.data());
        QCOMPARE(fenceMaster->geoFenceController()->polygons(), retainedFencePolygons.data());
        QCOMPARE(fenceMaster->geoFenceController()->circles(), retainedFenceCircles.data());
        QCOMPARE(fenceMaster->rallyPointController()->points(), retainedRallyPoints.data());
        const auto fenceContexts = map->findChildren<QObject*>(QStringLiteral("flyFencePlanStyleContext"));
        QCOMPARE(fenceContexts.size(), 1);
        QCOMPARE(fenceContexts.first()->property("vehicleId").toUInt(), vehicle->id());
        for (int retained = 0; retained < 2; ++retained) {
            QCOMPARE(_links[retained]->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE),
                     fenceDownloads[retained]);
            QCOMPARE(_links[retained]->receivedMissionRequestListCount(MAV_MISSION_TYPE_RALLY),
                     rallyDownloads[retained]);
        }

        QTRY_COMPARE_WITH_TIMEOUT(fenceMaster->geoFenceController()->polygons()->count(), 2, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(fenceMaster->geoFenceController()->circles()->count(), 2, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(fenceMaster->rallyPointController()->points()->count(), uploaded[v].size() - 1,
                                  TestTimeout::longMs());
        QCOMPARE(vehicle->geoFenceManager()->polygons().size(), 2);
        QCOMPARE(vehicle->geoFenceManager()->circles().size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(
            fenceMaster->geoFenceController()->polygons()->count() == 2 &&
                fenceMaster->geoFenceController()->circles()->count() == 2 &&
                fenceMaster->geoFenceController()->polygons()->value<QGCFencePolygon*>(0)->path() ==
                    vehicle->geoFenceManager()->polygons()[0].path() &&
                fenceMaster->geoFenceController()->circles()->value<QGCFenceCircle*>(0)->center() ==
                    vehicle->geoFenceManager()->circles()[0].center(),
            TestTimeout::longMs());
        for (int i = 0; i < 2; ++i) {
            auto* polygon = fenceMaster->geoFenceController()->polygons()->value<QGCFencePolygon*>(i);
            auto* circle = fenceMaster->geoFenceController()->circles()->value<QGCFenceCircle*>(i);
            QVERIFY(polygon && circle);
            QCOMPARE(polygon->path(), vehicle->geoFenceManager()->polygons()[i].path());
            QCOMPARE(polygon->inclusion(), vehicle->geoFenceManager()->polygons()[i].inclusion());
            QCOMPARE(circle->center(), vehicle->geoFenceManager()->circles()[i].center());
            QGCFenceCircle uploadedCircle(vehicle->geoFenceManager()->circles()[i]);
            QCOMPARE(circle->radius()->rawValue(), uploadedCircle.radius()->rawValue());
        }
        const auto fenceFills = [&] { return planStyleItems(map, QStringLiteral("planFenceFill")); };
        QTRY_COMPARE_WITH_TIMEOUT(fenceFills().size(), 5, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleItems(map, QStringLiteral("planKeepInOutline")).size(), 3,
                                  TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleItems(map, QStringLiteral("planKeepOutOutline")).size(), 2,
                                  TestTimeout::longMs());
        planStyleCheckBoundaries(map, vehicle->id(), expected, 2, 2);
        QVERIFY(!QTest::currentTestFailed());
        for (auto* fill : fenceFills()) {
            QCOMPARE(fill->opacity(), 1.0);
            if (fill->property("boundaryKind").toString() == QStringLiteral("polygon")) {
                bool found = false;
                for (int i = 0; i < 2; ++i) {
                    const auto* polygon = fenceMaster->geoFenceController()->polygons()->value<QGCFencePolygon*>(i);
                    found |= fill->property("path").toList() == polygon->path();
                }
                QVERIFY(found);
            }
        }
        const auto rallies = [&] { return planStyleItems(map, QStringLiteral("planRallyPoint")); };
        QTRY_COMPARE_WITH_TIMEOUT(rallies().size(), uploaded[v].size() - 1, TestTimeout::longMs());
        auto* rallyController = fenceMaster->rallyPointController();
        const auto vehicleRallies = vehicle->rallyPointManager()->points();
        QCOMPARE(vehicleRallies.size(), rallyController->points()->count());
        for (int i = 0; i < vehicleRallies.size(); ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(rallyController->points()->value<RallyPoint*>(i)->coordinate(),
                                      vehicleRallies[i].coordinate, TestTimeout::longMs());
        }
        rallyController->setCurrentRallyPoint(rallyController->points()->get(1));
        int selectedRallies = 0;
        for (auto* marker : rallies()) {
            planStyleCheckRally(map, marker, vehicle->id(), expected);
            QVERIFY(!QTest::currentTestFailed());
            const auto coordinate = marker->property("coordinate").value<QGeoCoordinate>();
            bool found = false;
            for (int i = 0; i < rallyController->points()->count(); ++i) {
                found |= coordinate == rallyController->points()->value<RallyPoint*>(i)->coordinate();
            }
            QVERIFY(found);
            selectedRallies += marker->property("checked").toBool() ? 1 : 0;
        }
        QCOMPARE(selectedRallies, 1);
        const auto breach = planStyleItems(map, QStringLiteral("planBreachReturnPoint"));
        QCOMPARE(breach.size(), 1);
        QCOMPARE(breach.first()->property("vehicleId").toUInt(), vehicle->id());
        QCOMPARE(breach.first()->property("label").toString(), QStringLiteral("B"));
        QCOMPARE(breach.first()->property("markerColor").value<QColor>(), expected);
        QCOMPARE(breach.first()->property("markerBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
        QCOMPARE(breach.first()->property("markerBorderWidth").toReal(), 2.0);
        QCOMPARE(breach.first()->property("markerTextColor").value<QColor>(), QColor(QStringLiteral("black")));

        // Browsing a different tab does not change the owner of the active vehicle's retained plan.
        const auto firstRoute = QPointer<QQuickItem>(routes().first());
        auto* browsedEntry = _entryFor(vehicles[1 - v]->id());
        QVERIFY(browsedEntry);
        controller->selectVehicle(vehicles[1 - v]->id());
        QCOMPARE(controller->selected(), browsedEntry);
        QCOMPARE(manager->activeVehicle(), vehicle);
        const auto contexts = loaderFor(vehicle)->parentItem()->findChildren<QObject*>(
            QStringLiteral("flyPlanStyleContext"), Qt::FindDirectChildrenOnly);
        QCOMPARE(contexts.size(), 1);
        QCOMPARE(contexts.first()->property("vehicleId").toUInt(), vehicle->id());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(firstRoute, QStringLiteral("line.color")), expected,
                                  TestTimeout::shortMs());
        for (auto* outline : planStyleItems(map, QStringLiteral("planKeepInOutline")) +
                                 planStyleItems(map, QStringLiteral("planKeepOutOutline"))) {
            QCOMPARE(outline->property("vehicleId").toUInt(), vehicle->id());
            QCOMPARE(outline->property("renderedStrokeColor").value<QColor>(), expected);
        }
        for (auto* marker : rallies()) {
            QCOMPARE(marker->property("markerColor").value<QColor>(), expected);
            QCOMPARE(marker->property("vehicleId").toUInt(), vehicle->id());
        }
        QVERIFY(_entryFor(vehicle->id()) == entry);
        controller->selectVehicle(vehicle->id());
        QCOMPARE(controller->selected(), entry);
        QVERIFY(_selectTab(vehicle->id()));
        QVERIFY(firstRoute);
        QCOMPARE(routes().first(), firstRoute.data());
        QList<bool> matched(segmentModels[v]->count(), false);
        for (auto* route : routes()) {
            QCOMPARE(route->property("vehicleId").toUInt(), vehicle->id());
            QCOMPARE(route->property("layerKind").toString(), QStringLiteral("route"));
            QVERIFY(route->isVisible());
            QCOMPARE(planStyleColor(route, QStringLiteral("line.color")), expected);
            QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 4.0);
            QCOMPARE(route->property("renderedColor").value<QColor>(), expected);
            QCOMPARE(route->property("renderedWidth").toReal(), 4.0);
            // Native MapPolyline is solid; all route objects must be native polylines with exactly this path.
            const auto path = route->property("path").toList();
            QCOMPARE(path.size(), 2);
            bool found = false;
            for (int i = 0; i < segmentModels[v]->count(); ++i) {
                auto* segment = segmentModels[v]->value<FlightPathSegment*>(i);
                if (!matched[i] && path.first().value<QGeoCoordinate>() == segment->coordinate1() &&
                    path.last().value<QGeoCoordinate>() == segment->coordinate2()) {
                    matched[i] = true;
                    found = true;
                    break;
                }
            }
            QVERIFY(found);
        }
        QQuickItem* current = nullptr;
        QQuickItem* ordinary = nullptr;
        const int currentSeq = 1;
        const int firmwareSeq = currentSeq - (vehicle->firmwarePlugin()->sendHomePositionToVehicle() ? 0 : 1);
        emit vehicle->missionManager()->currentIndexChanged(firmwareSeq);
        QSet<int> sequences;
        for (auto* marker : markers()) {
            QCOMPARE(marker->property("vehicleId").toUInt(), vehicle->id());
            QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), expected);
            QCOMPARE(marker->property("renderedTextColor").value<QColor>(), policy->markerTextColor());
            QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), policy->markerBorderColor());
            QCOMPARE(marker->property("renderedBorderWidth").toReal(), policy->markerBorderWidth());
            const int sequence = marker->property("sequenceNumber").toInt();
            QVERIFY(sequence > 0 && sequence < uploaded[v].size());
            QVERIFY(!sequences.contains(sequence));
            sequences.insert(sequence);
            auto coordinate = marker->property("coordinate").value<QGeoCoordinate>();
            QVERIFY(coordinate.distanceTo(uploaded[v][sequence]) < 0.05);
            QCOMPARE(marker->property("index").toInt(), sequence);
            QCOMPARE(marker->property("renderedIndexText").toString(), QString::number(sequence));
            QCOMPARE(marker->property("checked").toBool(), sequence == currentSeq);
            QCOMPARE(marker->property("current").toBool(), sequence == currentSeq);
            if (sequence == currentSeq) {
                current = marker;
            } else if (marker->isVisible() && !marker->property("grouped").toBool()) {
                ordinary = marker;
            }
        }
        QVERIFY(current && ordinary);
        auto* currentLabel = planStyleSource(current);
        auto* ordinaryLabel = planStyleSource(ordinary);
        QVERIFY(currentLabel && ordinaryLabel);
        auto* currentBody = qvariant_cast<QQuickItem*>(currentLabel->property("renderedIndicator"));
        auto* ordinaryBody = qvariant_cast<QQuickItem*>(ordinaryLabel->property("renderedIndicator"));
        QVERIFY(currentBody && ordinaryBody);
        QVERIFY(currentBody->width() > ordinaryBody->width());
        const auto halos = planStyleItems(currentLabel, QStringLiteral("planWaypointSelectionHalo"));
        QCOMPARE(halos.size(), 1);
        QVERIFY(halos.first()->isVisible());
        QCOMPARE(halos.first()->width(), currentBody->width() * 2);
        QCOMPARE(planStyleColor(halos.first(), QStringLiteral("border.color")), QColor::fromRgbF(1, 1, 1, 0.5));
        auto* home = homes().first();
        QCOMPARE(home->property("vehicleId").toUInt(), vehicle->id());
        QVERIFY(home->isVisible());
        QCOMPARE(home->property("coordinate").value<QGeoCoordinate>(),
                 controllers[v]->missionController()->plannedHomePosition());
        auto* house = planStyleSource(home);
        QVERIFY(house);
        QVERIFY(house->inherits("QQuickImage"));
        QCOMPARE(house->property("source").toUrl(), QUrl(QStringLiteral("qrc:///qmlimages/MapHome.svg")));
        const auto accents = planStyleItems(home, QStringLiteral("planHomeAccent"));
        QCOMPARE(accents.size(), 1);
        QVERIFY(accents.first()->isVisible());
        QCOMPARE(planStyleColor(accents.first(), QStringLiteral("border.color")), expected);
        QCOMPARE(accents.first()->property("color").value<QColor>(), QColor(QStringLiteral("transparent")));
        QCOMPARE(home->property("renderedBorderColor").value<QColor>(), expected);
        auto* tracker = qvariant_cast<QObject*>(map->property("positionTracker"));
        QVERIFY(tracker);
        QVERIFY(tracker->setProperty("userInteracting", true));
        // Pausing follow does not stop a recenter animation that is already running.
        QTRY_VERIFY_WITH_TIMEOUT(tracker->property("firstVehiclePositionReceived").toBool(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(!tracker->property("animating").toBool(), TestTimeout::mediumMs());
        QVERIFY(map->setProperty("zoomLevel", 17));
        QVERIFY(map->setProperty("center", current->property("coordinate")));
        const auto clickTargetReady = [&] {
            map->ensurePolished();
            current->ensurePolished();
            currentLabel->ensurePolished();
            currentBody->ensurePolished();
            const QPointF sceneCenter =
                currentBody->mapToScene(QPointF(currentBody->width() / 2, currentBody->height() / 2));
            const QPointF mapCenter = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
            return currentBody->window() == _window && current->isVisible() && currentBody->isVisible() &&
                   currentBody->width() > 0 && currentBody->height() > 0 && sceneCenter.x() >= 0 &&
                   sceneCenter.x() < _window->width() && sceneCenter.y() >= 0 && sceneCenter.y() < _window->height() &&
                   (sceneCenter - mapCenter).manhattanLength() < 2;
        };
        QTRY_VERIFY_WITH_TIMEOUT(clickTargetReady(), TestTimeout::mediumMs());
        if (grouped) {
            QTRY_VERIFY_WITH_TIMEOUT(current->property("grouped").toBool(), TestTimeout::shortMs());
            QCOMPARE(currentLabel->property("indicatorSubText").toString(), QStringLiteral("…"));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!current->property("grouped").toBool(), TestTimeout::shortMs());
        }
        QVERIFY(_clickItemAt(currentBody, 0.5, 0.5, QStringLiteral("current waypoint")));
        if (grouped) {
            QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("missionItemSelection_2"), 0),
                                     TestTimeout::shortMs());
            auto* group = qvariant_cast<QObject*>(current->property("indicatorGroup"));
            QVERIFY(group);
            QSignalSpy selected(group, SIGNAL(itemSelected(int)));
            QVERIFY(selected.isValid());
            QVERIFY(clickItemFraction(QStringLiteral("missionItemSelection_2"), 0.5, 0.5));
            QTRY_COMPARE_WITH_TIMEOUT(selected.count(), 1, TestTimeout::shortMs());
            QCOMPARE(selected.first().first().toInt(), 2);
        }
        QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), 0),
                                 TestTimeout::shortMs());
        auto* confirmation = findItem(_rootItem, QStringLiteral("guidedActionConfirmButton"));
        while (confirmation && !confirmation->property("actionData").isValid()) {
            confirmation = confirmation->parentItem();
        }
        QVERIFY(confirmation);
        QVERIFY(confirmation->property("pending").toBool());
        auto* guidedController = qvariant_cast<QObject*>(confirmation->property("guidedController"));
        QVERIFY(guidedController);
        QCOMPARE(confirmation->property("action").toInt(), guidedController->property("actionSetWaypoint").toInt());
        QCOMPARE(confirmation->property("actionData").toInt(), grouped ? 2 : currentSeq);
        QCOMPARE(qvariant_cast<QObject*>(confirmation->property("vehicle")), static_cast<QObject*>(vehicle));
        QVERIFY(QMetaObject::invokeMethod(confirmation, "confirmCancelled", Q_ARG(QVariant, QVariant())));
        QVERIFY(!confirmation->property("pending").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), 0),
                                 TestTimeout::shortMs());
        QVERIFY(tracker->setProperty("userInteracting", false));
        // A real path segment computes collision from a deterministic terrain profile and altitude changes.
        auto* segment = segmentModels[v]->value<FlightPathSegment*>(0);
        QVERIFY(segment);
        QVERIFY(segment->totalDistance() > 0.0);
        QVERIFY(segment->setProperty("distanceBetween", segment->totalDistance()));
        QVERIFY(segment->setProperty("finalDistanceBetween", segment->totalDistance()));
        QVERIFY(segment->setProperty("amslTerrainHeights", QVariantList{1.0e6, 1.0e6}));
        segment->setCoord1AMSLAlt(-100);
        segment->setCoord2AMSLAlt(-100);
        QVERIFY(segment->terrainCollision());
        const auto collided = [&]() -> QQuickItem* {
            for (auto* route : routes()) {
                const auto path = route->property("path").toList();
                if (!path.isEmpty() && path.first().value<QGeoCoordinate>() == segment->coordinate1() &&
                    path.last().value<QGeoCoordinate>() == segment->coordinate2()) {
                    return route;
                }
            }
            return nullptr;
        };
        QVERIFY(collided());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(collided(), QStringLiteral("line.color")),
                                  QColor(QStringLiteral("red")), TestTimeout::shortMs());
        QVERIFY(collided()->property("terrainCollision").toBool());
        segment->setCoord1AMSLAlt(2.0e6);
        segment->setCoord2AMSLAlt(2.0e6);
        QVERIFY(!segment->terrainCollision());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(collided(), QStringLiteral("line.color")), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(_links[v]->receivedMissionRequestListCount(MAV_MISSION_TYPE_MISSION), downloads[v]);
    }
}

void PlanStyleUITest::_liveRegistryAndDynamicVisuals()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto* policy = plugin->planStyle();
    QVERIFY(policy);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    auto* registry = engine->singletonInstance<COPController*>("QGC", "COPController");
    auto* roles = engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(registry && roles);
    registry->initialize(roles);
    for (quint32 id : {0U, 7501U, 7502U, 7599U}) {
        QVERIFY(!registry->colorForSysid(id).isValid());
        QVERIFY(roles->roleForSysid(id).isEmpty());
    }
    const QStringList availableRoles = roles->availableRoles();
    QVERIFY(!availableRoles.isEmpty());
    const QString role = availableRoles.first();
    const int initialEntries = registry->vehicles()->count();
    const int initialRoleEntries = roles->roleEntries()->count();
    PlanMasterController master(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_FIXED_WING);
    master.setFlyView(true);
    master.start();
    auto* home = master.missionController()->visualItems()->value<MissionSettingsItem*>(0);
    QVERIFY(home);
    const QGeoCoordinate base(47, 8, 100);
    home->setCoordinate(base);
    SimpleMissionItem waypoint(&master, true, false);
    waypoint.setCommand(MAV_CMD_NAV_LOITER_UNLIM);
    waypoint.missionItem().setParam3(80);
    waypoint.setCoordinate(base.atDistanceAndAzimuth(300, 90));
    waypoint.setSequenceNumber(1);
    TakeoffMissionItem takeoff(MAV_CMD_NAV_TAKEOFF, &master, true, home, false);
    takeoff.setCoordinate(base.atDistanceAndAzimuth(100, 90));
    takeoff.setSequenceNumber(2);
    FixedWingLandingComplexItem fixedWing(&master, true);
    fixedWing.setSequenceNumber(3);
    fixedWing.setLandingCoordinate(base.atDistanceAndAzimuth(600, 90));
    fixedWing.setIsCurrentItem(true);
    VTOLLandingComplexItem vtol(&master, true);
    vtol.setSequenceNumber(20);
    vtol.setLandingCoordinate(base.atDistanceAndAzimuth(900, 90));
    vtol.setIsCurrentItem(true);
    QmlObjectListModel visualItems;
    visualItems.append(home);
    visualItems.append(&waypoint);
    visualItems.append(&takeoff);
    visualItems.append(&fixedWing);
    visualItems.append(&vtol);
    QmlObjectListModel segments;
    FlightPathSegment segment(FlightPathSegment::SegmentTypeGeneric, base, 100, base.atDistanceAndAzimuth(300, 90), 100,
                              false, nullptr);
    segments.append(&segment);
    engine->rootContext()->setContextProperty(QStringLiteral("testVisualItems"), &visualItems);
    engine->rootContext()->setContextProperty(QStringLiteral("testSegments"), &segments);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        Map {
            id: testMap
            width: 640
            height: 480
            plugin: Plugin { name: "QGroundControl" }
            property bool planView: false
            property bool isSatelliteMap: false
            property real styleId: 7501
            property bool useSecondContext: false
            property var style: useSecondContext ? secondStyle : firstStyle
            readonly property color neutral: QGroundControl.globalPalette.mapMissionTrajectory
            PlanStyleContext { id: firstStyle; vehicleId: testMap.styleId }
            PlanStyleContext { id: secondStyle; vehicleId: 7502 }
            MissionLineView { model: testSegments; planStyle: testMap.style }
            Repeater {
                model: testVisualItems
                MissionItemMapVisual { map: testMap; vehicle: null; planStyle: testMap.style }
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    auto* firstContext = qvariant_cast<QObject*>(root->property("style"));
    QVERIFY(firstContext);
    QCOMPARE(firstContext->property("vehicleId").toUInt(), 7501U);
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planMissionRoute")).size(), 1, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planHomeAccent")).size(), 1, TestTimeout::mediumMs());
    // Simple, takeoff, launch, two FW endpoints and two VTOL endpoints all use the real async dispatch.
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planWaypoint")).size(), 7, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPatternLine")).size(), 2, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planLoiterRing")).size(), 2, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planSimpleLoiterVisual")).size(), 1, TestTimeout::mediumMs());
    auto* simpleLoiter = named(QStringLiteral("planSimpleLoiterVisual")).first();
    const auto loiterCircle = [simpleLoiter] {
        return qvariant_cast<QQuickItem*>(simpleLoiter->property("_circleComponent"));
    };
    QTRY_VERIFY_WITH_TIMEOUT(loiterCircle(), TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planLandingArea")).size(), 1, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planGlideSlope")).size(), 1, TestTimeout::mediumMs());
    auto* landingArea = named(QStringLiteral("planLandingArea")).first();
    auto* glideSlope = named(QStringLiteral("planGlideSlope")).first();
    const auto markers = named(QStringLiteral("planWaypoint"));
    const auto route = QPointer<QQuickItem>(named(QStringLiteral("planMissionRoute")).first());
    const auto accent = QPointer<QQuickItem>(named(QStringLiteral("planHomeAccent")).first());
    QList<QPointer<QQuickItem>> retainedMarkers;
    for (auto* marker : markers) {
        retainedMarkers.append(marker);
    }
    const auto verifyColour = [&](const QColor& expected, quint32 id) {
        QVERIFY(route && accent);
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(route, QStringLiteral("line.color")), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 4.0);
        QCOMPARE(route->property("vehicleId").toUInt(), id);
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(accent, QStringLiteral("border.color")), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(accent->property("vehicleId").toUInt(), id);
        for (const auto& marker : retainedMarkers) {
            QVERIFY(marker);
            auto* label = planStyleSource(marker);
            QVERIFY(label);
            auto* body = qvariant_cast<QQuickItem*>(label->property("renderedIndicator"));
            QVERIFY(body);
            QTRY_COMPARE_WITH_TIMEOUT(marker->property("vehicleId").toUInt(), id, TestTimeout::shortMs());
            QTRY_COMPARE_WITH_TIMEOUT(marker->property("renderedBodyColor").value<QColor>(), expected,
                                      TestTimeout::shortMs());
            QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), policy->markerBorderColor());
            QCOMPARE(marker->property("renderedBorderWidth").toReal(), policy->markerBorderWidth());
            QCOMPARE(marker->property("renderedTextColor").value<QColor>(), policy->markerTextColor());
            if (marker->property("current").toBool()) {
                const auto halos = planStyleItems(label, QStringLiteral("planWaypointSelectionHalo"));
                QCOMPARE(halos.size(), 1);
                QVERIFY(halos.first()->isVisible());
                QTRY_COMPARE_WITH_TIMEOUT(halos.first()->width(), body->width() * 2, TestTimeout::shortMs());
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(loiterCircle(), TestTimeout::shortMs());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(loiterCircle(), QStringLiteral("border.color")), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(QQmlProperty(loiterCircle(), QStringLiteral("border.width")).read().toReal(), 3.0);
        QCOMPARE(landingArea->property("color").value<QColor>(), QColor(QStringLiteral("green")));
        QCOMPARE(landingArea->opacity(), 0.5);
        QCOMPARE(glideSlope->property("color").value<QColor>(), QColor(QStringLiteral("orange")));
        QCOMPARE(glideSlope->opacity(), 0.5);
        QCOMPARE(named(QStringLiteral("planPatternLine")).size(), 2);
        for (auto* path : named(QStringLiteral("planPatternLine"))) {
            QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(path, QStringLiteral("line.color")), expected,
                                      TestTimeout::shortMs());
            QCOMPARE(path->property("vehicleId").toUInt(), id);
            QCOMPARE(QQmlProperty(path, QStringLiteral("line.width")).read().toReal(), 2.0);
        }
        QCOMPARE(named(QStringLiteral("planLoiterRing")).size(), 2);
        for (auto* ring : named(QStringLiteral("planLoiterRing"))) {
            QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(ring, QStringLiteral("border.color")), expected,
                                      TestTimeout::shortMs());
            QCOMPARE(ring->property("vehicleId").toUInt(), id);
            QCOMPARE(QQmlProperty(ring, QStringLiteral("border.width")).read().toReal(), 2.0);
            QCOMPARE(ring->property("color").value<QColor>(), QColor(QStringLiteral("transparent")));
        }
    };
    const QColor neutral = root->property("neutral").value<QColor>();
    QVERIFY(neutral.isValid());
    verifyColour(neutral, 7501);
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(registry->vehicles()->count(), initialEntries);
    QVERIFY(!roles->isNicknameTaken(QStringLiteral("Other vehicle"), 7502));
    roles->addEntry(7502, role, QStringLiteral("Other vehicle"), 0);
    QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 1);
    auto* otherRole = roles->roleEntries()->value<VehicleRoleEntry*>(initialRoleEntries);
    QVERIFY(otherRole);
    QCOMPARE(otherRole->sysid(), 7502U);
    QCOMPARE(otherRole->role(), role);
    QCOMPARE(otherRole->name(), QStringLiteral("Other vehicle"));
    QCOMPARE(otherRole->port(), 0);
    QCOMPARE(registry->vehicles()->count(), initialEntries + 1);
    // _remember gives 7502 colour index initialEntries and 7501 the next index, including palette wrap.
    auto* other = registry->vehicles()->value<COPVehicle*>(initialEntries);
    QVERIFY(other);
    QCOMPARE(other->sysid(), 7502U);
    QVERIFY(other->color().isValid());
    registry->selectVehicle(7502);
    QCOMPARE(registry->selected(), other);
    QSignalSpy revisions(policy, &QGCPlanStyle::revisionChanged);
    QVERIFY(revisions.isValid());
    const int revisionBeforeEntry = policy->revision();
    QVERIFY(!registry->colorForSysid(7501).isValid());
    QVERIFY(!roles->isNicknameTaken(QStringLiteral("Late vehicle"), 7501));
    roles->addEntry(7501, role, QStringLiteral("Late vehicle"), 0);
    QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 2);
    auto* lateRole = roles->roleEntries()->value<VehicleRoleEntry*>(initialRoleEntries + 1);
    QVERIFY(lateRole);
    QCOMPARE(lateRole->sysid(), 7501U);
    QCOMPARE(lateRole->role(), role);
    QCOMPARE(lateRole->name(), QStringLiteral("Late vehicle"));
    QCOMPARE(lateRole->port(), 0);
    QCOMPARE(registry->vehicles()->count(), initialEntries + 2);
    auto* lateEntry = registry->vehicles()->value<COPVehicle*>(initialEntries + 1);
    QVERIFY(lateEntry);
    QCOMPARE(lateEntry->sysid(), 7501U);
    QTRY_VERIFY_WITH_TIMEOUT(revisions.count() > 0, TestTimeout::shortMs());
    QVERIFY(policy->revision() != revisionBeforeEntry);
    QTRY_VERIFY_WITH_TIMEOUT(registry->colorForSysid(7501).isValid(), TestTimeout::shortMs());
    const QColor late = registry->colorForSysid(7501);
    QCOMPARE(late, lateEntry->color());
    QCOMPARE(registry->selected(), other);
    QVERIFY(late != other->color());
    QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(route, QStringLiteral("line.color")), late, TestTimeout::shortMs());
    verifyColour(late, 7501);
    QVERIFY(!QTest::currentTestFailed());
    // Change the context object itself after every async visual has already been created.
    QVERIFY(root->setProperty("useSecondContext", true));
    auto* secondContext = qvariant_cast<QObject*>(root->property("style"));
    QVERIFY(secondContext);
    QVERIFY(secondContext != firstContext);
    QCOMPARE(secondContext->property("vehicleId").toUInt(), 7502U);
    QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(route, QStringLiteral("line.color")), other->color(),
                              TestTimeout::shortMs());
    verifyColour(other->color(), 7502);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(root->setProperty("useSecondContext", false));
    QCOMPARE(qvariant_cast<QObject*>(root->property("style")), firstContext);
    verifyColour(late, 7501);
    QVERIFY(!QTest::currentTestFailed());
    const int revisionBeforeLookups = policy->revision();
    const auto orderBeforeLookups = QSettings().value(QStringLiteral("COP/VehicleOrder")).toList();
    for (quint32 id : {0U, 7599U}) {
        QVERIFY(root->setProperty("styleId", id));
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(route, QStringLiteral("line.color")), neutral, TestTimeout::shortMs());
        verifyColour(neutral, id);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(!registry->colorForSysid(id).isValid());
        QVERIFY(!policy->vehicleColor(id).isValid());
        for (int i = 0; i < registry->vehicles()->count(); ++i) {
            auto* entry = registry->vehicles()->value<COPVehicle*>(i);
            QVERIFY(entry);
            QVERIFY(entry->sysid() != id);
        }
        QVERIFY(roles->roleForSysid(id).isEmpty());
        QCOMPARE(registry->vehicles()->count(), initialEntries + 2);
        QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 2);
        QCOMPARE(policy->revision(), revisionBeforeLookups);
        QCOMPARE(QSettings().value(QStringLiteral("COP/VehicleOrder")).toList(), orderBeforeLookups);
    }
    // The simple loiter renderer retains its error colour and recovers its neutral fallback.
    waypoint.setTerrainCollision(true);
    QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(loiterCircle(), QStringLiteral("border.color")),
                              QColor(QStringLiteral("red")), TestTimeout::shortMs());
    waypoint.setTerrainCollision(false);
    QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(loiterCircle(), QStringLiteral("border.color")), neutral,
                              TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(fixedWing.flightPathSegments()->count() > 0, TestTimeout::mediumMs());
    auto* approach = fixedWing.flightPathSegments()->value<FlightPathSegment*>(0);
    QVERIFY(approach);
    QVERIFY(approach->totalDistance() > 0.0);
    QVERIFY(approach->setProperty("distanceBetween", approach->totalDistance()));
    QVERIFY(approach->setProperty("finalDistanceBetween", approach->totalDistance()));
    QVERIFY(approach->setProperty("amslTerrainHeights", QVariantList{1.0e6, 1.0e6}));
    approach->setCoord1AMSLAlt(-100);
    approach->setCoord2AMSLAlt(-100);
    QVERIFY(approach->terrainCollision());
    QVERIFY(fixedWing.terrainCollision());
    QTRY_COMPARE_WITH_TIMEOUT(glideSlope->property("color").value<QColor>(), QColor(QStringLiteral("red")),
                              TestTimeout::shortMs());
    QCOMPARE(landingArea->property("color").value<QColor>(), QColor(QStringLiteral("green")));
    approach->setCoord1AMSLAlt(2.0e6);
    approach->setCoord2AMSLAlt(2.0e6);
    QVERIFY(!approach->terrainCollision());
    QVERIFY(!fixedWing.terrainCollision());
    QTRY_COMPARE_WITH_TIMEOUT(glideSlope->property("color").value<QColor>(), QColor(QStringLiteral("orange")),
                              TestTimeout::shortMs());
    QCOMPARE(registry->vehicles()->count(), initialEntries + 2);
    QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 2);
    QCOMPARE(named(QStringLiteral("planMissionRoute")).size(), 1);
    QCOMPARE(named(QStringLiteral("planMissionRoute")).first(), route.data());
    QCOMPARE(named(QStringLiteral("planHomeAccent")).size(), 1);
    QCOMPARE(named(QStringLiteral("planHomeAccent")).first(), accent.data());
    QCOMPARE(named(QStringLiteral("planWaypoint")).size(), retainedMarkers.size());
    for (const auto& marker : retainedMarkers) {
        QVERIFY(marker);
        QVERIFY(named(QStringLiteral("planWaypoint")).contains(marker.data()));
    }
}

void PlanStyleUITest::_liveBoundaryVisuals()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    planStyleWarmUpScreenTools(engine.get());
    if (QTest::currentTestFailed()) {
        return;
    }
    auto* registry = engine->singletonInstance<COPController*>("QGC", "COPController");
    auto* roles = engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(registry && roles);
    registry->initialize(roles);
    QVERIFY(!registry->colorForSysid(7601).isValid());
    QVERIFY(!registry->colorForSysid(7602).isValid());
    QVERIFY(!roles->availableRoles().isEmpty());
    const auto role = roles->availableRoles().first();
    const QGeoCoordinate base(47, 8, 120);
    QmlObjectListModel polygons;
    QmlObjectListModel circles;
    for (bool inclusion : {true, false}) {
        auto* polygon = new QGCFencePolygon(inclusion, &polygons);
        polygon->setInteractive(false);
        for (double azimuth : {0.0, 120.0, 240.0}) {
            polygon->appendVertex(base.atDistanceAndAzimuth(inclusion ? 250 : 80, azimuth));
        }
        polygons.append(polygon);
        auto* circle =
            new QGCFenceCircle(base.atDistanceAndAzimuth(150, inclusion ? 90 : 180), 45, inclusion, &circles);
        circle->setInteractive(false);
        circles.append(circle);
    }
    PlanMasterController master(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR);
    master.start();
    auto* rallyController = master.rallyPointController();
    QVERIFY(rallyController);
    rallyController->addPoint(base.atDistanceAndAzimuth(50, 90));
    rallyController->addPoint(base.atDistanceAndAzimuth(100, 90));
    QCOMPARE(rallyController->points()->count(), 2);
    auto* rally = rallyController->points()->value<RallyPoint*>(0);
    QVERIFY(rally);
    rally->setCoordinate(QGeoCoordinate(47, 8.001, 137));
    rallyController->setCurrentRallyPoint(rally);
    engine->rootContext()->setContextProperty(QStringLiteral("testPolygons"), &polygons);
    engine->rootContext()->setContextProperty(QStringLiteral("testCircles"), &circles);
    engine->rootContext()->setContextProperty(QStringLiteral("testRallyController"), rallyController);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        FlightMap {
            id: testMap
            width: 640
            height: 480
            center: QtPositioning.coordinate(47, 8)
            zoomLevel: 16
            planView: true
            isSatelliteMap: false
            allowGCSLocationCenter: false
            allowVehicleLocationCenter: false
            property int _editingLayer: 2
            property int _layerRally: 2
            property real layerOpacity: 1
            property bool visualsEnabled: true
            property bool styleSupported: true
            property real styleId: 7601
            readonly property color neutral: QGroundControl.globalPalette.mapMissionTrajectory
            readonly property var style: fenceStyle
            QtObject { id: mainWindow; function allowViewSwitch() { return true } }
            PlanStyleContext { id: fenceStyle; vehicleId: testMap.styleId; supported: testMap.styleSupported }
            property QtObject fence: QtObject {
                property var polygons: testPolygons
                property var circles: testCircles
                property real paramCircularFence: 350
                property var breachReturnPoint: QtPositioning.coordinate(47, 8, 120)
            }
            Loader {
                active: testMap.visualsEnabled
                sourceComponent: GeoFenceMapVisuals {
                    map: testMap
                    myGeoFenceController: testMap.fence
                    homePosition: QtPositioning.coordinate(47, 8)
                    planStyle: fenceStyle
                    opacity: testMap.layerOpacity
                }
            }
            Loader {
                active: testMap.visualsEnabled
                sourceComponent: RallyPointMapVisuals {
                    objectName: "rallyVisuals"
                    map: testMap
                    myRallyPointController: testRallyController
                    planStyle: fenceStyle
                    interactive: true
                    planView: true
                    opacity: testMap.layerOpacity
                }
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QQuickWindow boundaryWindow;
    boundaryWindow.resize(800, 600);
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    map->setParentItem(boundaryWindow.contentItem());
    boundaryWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(boundaryWindow.isExposed(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(map->property("mapReady").toBool(), TestTimeout::longMs());
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planFenceFill")).size(), 5, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepInOutline")).size(), 3, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepOutOutline")).size(), 2, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planRallyPoint")).size(), 2, TestTimeout::mediumMs());
    const auto rallyPositionsReady = [&] {
        if (!planStyleRenderMap(map)) {
            return false;
        }
        const auto markers = named(QStringLiteral("planRallyPoint"));
        if (markers.size() != rallyController->points()->count()) {
            return false;
        }
        for (auto* marker : markers) {
            if (!planStyleMapItemProjected(map, marker)) {
                return false;
            }
        }
        for (auto* item : map->childItems()) {
            if (!item->isVisible() || item->metaObject()->indexOfProperty("itemCoordinate") < 0) {
                continue;
            }
            auto* indicator = qvariant_cast<QQuickItem*>(item->property("itemIndicator"));
            if (!indicator) {
                return false;
            }
            const auto anchor = indicator->property("anchorPoint").toPointF() +
                                QPointF(item->property("_touchMarginHorizontal").toReal(),
                                        item->property("_touchMarginVertical").toReal());
            if (!planStyleProjectedPosition(map, item->property("itemCoordinate").value<QGeoCoordinate>(),
                                            item->mapToScene(anchor))) {
                return false;
            }
        }
        return true;
    };
    const auto check = [&](const QColor& color) {
        QTRY_VERIFY_WITH_TIMEOUT(rallyPositionsReady(), TestTimeout::mediumMs());
        planStyleCheckBoundaries(map, 7601, color, 2, 2);
        QVERIFY(!QTest::currentTestFailed());
        int selected = 0;
        for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
            planStyleCheckRally(map, marker, 7601, color);
            QVERIFY(!QTest::currentTestFailed());
            selected += marker->property("checked").toBool() ? 1 : 0;
            QCOMPARE(marker->opacity(), root->property("layerOpacity").toReal());
        }
        QCOMPARE(selected, 1);
        const auto breach = named(QStringLiteral("planBreachReturnPoint"));
        QCOMPARE(breach.size(), 1);
        QCOMPARE(breach.first()->property("markerColor").value<QColor>(), color);
        QCOMPARE(breach.first()->property("label").toString(), QStringLiteral("B"));
        QCOMPARE(breach.first()->property("vehicleId").toUInt(), 7601U);
        QCOMPARE(breach.first()->property("coordinate").value<QGeoCoordinate>(), base);
        QCOMPARE(breach.first()->property("markerBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
        QCOMPARE(breach.first()->property("markerBorderWidth").toReal(), 2.0);
        QCOMPARE(breach.first()->property("markerTextColor").value<QColor>(), QColor(QStringLiteral("black")));
    };
    const int registryCount = registry->vehicles()->count();
    check(root->property("neutral").value<QColor>());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(registry->vehicles()->count(), registryCount);
    QList<QPointer<QQuickItem>> retained;
    for (const auto& name : {QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"),
                             QStringLiteral("planKeepOutOutline"), QStringLiteral("planRallyPoint")}) {
        for (auto* item : named(name)) {
            retained.append(item);
        }
    }
    roles->addEntry(7602, role, QStringLiteral("Browsed boundary test vehicle"), 0);
    QTRY_VERIFY_WITH_TIMEOUT(registry->colorForSysid(7602).isValid(), TestTimeout::shortMs());
    registry->selectVehicle(7602);
    QVERIFY(registry->selected());
    QCOMPARE(registry->selected()->sysid(), 7602U);
    QVERIFY(registry->selected()->sysid() != 0);
    roles->addEntry(7601, role, QStringLiteral("Late boundary test vehicle"), 0);
    QTRY_VERIFY_WITH_TIMEOUT(registry->colorForSysid(7601).isValid(), TestTimeout::shortMs());
    COPVehicle* entry = nullptr;
    for (int i = 0; i < registry->vehicles()->count(); ++i) {
        auto* candidate = registry->vehicles()->value<COPVehicle*>(i);
        QVERIFY(candidate);
        if (candidate->sysid() == 7601) {
            entry = candidate;
        }
    }
    QVERIFY(entry);
    QVERIFY(entry->sysid() != 0);
    QVERIFY(entry->color() != registry->selected()->color());
    QTRY_COMPARE_WITH_TIMEOUT(
        named(QStringLiteral("planKeepInOutline")).first()->property("renderedStrokeColor").value<QColor>(),
        entry->color(), TestTimeout::shortMs());
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    for (const auto& item : retained) {
        QVERIFY(item);
    }
    QVERIFY(root->setProperty("layerOpacity", 0.5));
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        QCOMPARE(fill->opacity(), 0.5);
    }
    auto* polygon = polygons.value<QGCFencePolygon*>(0);
    QVERIFY(polygon);
    QQuickItem* polygonOutline = nullptr;
    for (auto* item : named(QStringLiteral("planKeepInOutline"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon")) {
            polygonOutline = item;
        }
    }
    QVERIFY(polygonOutline);
    const auto beforeDrag = polygonOutline->property("vertices").toList();
    polygon->setVertexDrag(true);
    QTRY_VERIFY_WITH_TIMEOUT(!polygonOutline->property("_projectionPending").toBool(), TestTimeout::shortMs());
    QSignalSpy projections(polygonOutline, SIGNAL(_screenPointsChanged()));
    QVERIFY(projections.isValid());
    const auto dragged = base.atDistanceAndAzimuth(600, 45);
    polygon->adjustVertex(0, base.atDistanceAndAzimuth(400, 45));
    polygon->adjustVertex(0, base.atDistanceAndAzimuth(500, 45));
    polygon->adjustVertex(0, dragged);
    QVERIFY(polygon->vertexDrag());
    QTRY_COMPARE_WITH_TIMEOUT(polygonOutline->property("vertices").toList().first().value<QGeoCoordinate>(), dragged,
                              TestTimeout::shortMs());
    QVERIFY(polygonOutline->property("vertices").toList() != beforeDrag);
    polygon->setShowAltColor(true);
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(projections.count(), 1);
    polygon->setVertexDrag(false);
    auto* circle = circles.value<QGCFenceCircle*>(0);
    QVERIFY(circle);
    circle->setCenter(base.atDistanceAndAzimuth(400, 225));
    circle->radius()->setRawValue(123);
    auto* fence = qvariant_cast<QObject*>(root->property("fence"));
    QVERIFY(fence);
    QVERIFY(fence->setProperty("paramCircularFence", 480));
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        if (fill->property("boundaryKind").toString() == QStringLiteral("circle") &&
            fill->property("inclusion").toBool()) {
            QCOMPARE(fill->property("center").value<QGeoCoordinate>(), circle->center());
            QCOMPARE(fill->property("radius").toDouble(), 123.0);
        } else if (fill->property("boundaryKind").toString() == QStringLiteral("parameterCircle")) {
            QCOMPARE(fill->property("radius").toDouble(), 480.0);
        }
    }
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    {
        const auto boundaryOutlines =
            named(QStringLiteral("planKeepInOutline")) + named(QStringLiteral("planKeepOutOutline"));
        QCOMPARE(boundaryOutlines.size(), 5);
        const auto projectionsSettled = [&] {
            for (auto* outline : boundaryOutlines) {
                if (outline->property("_projectionPending").toBool()) {
                    return false;
                }
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(projectionsSettled(), TestTimeout::shortMs());
        std::vector<std::unique_ptr<QSignalSpy>> projectionSpies;
        for (auto* outline : boundaryOutlines) {
            projectionSpies.push_back(std::make_unique<QSignalSpy>(outline, SIGNAL(_screenPointsChanged())));
            QVERIFY(projectionSpies.back()->isValid());
        }
        QSignalSpy zoomChanges(map, QQmlProperty(map, QStringLiteral("zoomLevel")).property().notifySignal());
        QSignalSpy widthChanges(map, &QQuickItem::widthChanged);
        QSignalSpy heightChanges(map, &QQuickItem::heightChanged);
        QVERIFY(zoomChanges.isValid());
        QVERIFY(widthChanges.isValid());
        QVERIFY(heightChanges.isValid());
        QCOMPARE(map->property("zoomLevel").toReal(), 16.0);
        QCOMPARE(map->width(), 640.0);
        QCOMPARE(map->height(), 480.0);
        // Unlike vertex drags, these independent inputs are not coalesced by the boundary models.
        QVERIFY(root->setProperty("zoomLevel", 17));
        QVERIFY(root->setProperty("width", 800));
        QVERIFY(root->setProperty("height", 600));
        QCOMPARE(map->property("zoomLevel").toReal(), 17.0);
        QCOMPARE(map->width(), 800.0);
        QCOMPARE(map->height(), 600.0);
        QCOMPARE(zoomChanges.count(), 1);
        QCOMPARE(widthChanges.count(), 1);
        QCOMPARE(heightChanges.count(), 1);
        check(entry->color());
        QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY_WITH_TIMEOUT(projectionsSettled(), TestTimeout::shortMs());
        for (const auto& spy : projectionSpies) {
            QCOMPARE(spy->count(), 1);
        }
    }
    QVERIFY(root->setProperty("center", QVariant::fromValue(base.atDistanceAndAzimuth(100, 90))));
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    // Exercise the existing selected-point drag object and its altitude-preserving assignment.
    const auto rallyVisualItems = named(QStringLiteral("rallyVisuals"));
    QCOMPARE(rallyVisualItems.size(), 1);
    auto* rallyVisuals = rallyVisualItems.first();
    auto* repeater = qvariant_cast<QObject*>(rallyVisuals->property("_rallyPointsComponent"));
    QVERIFY(repeater);
    QObject* drag = nullptr;
    for (auto* item : map->childItems()) {
        if (qvariant_cast<QObject*>(item->property("rallyPointObject")) == rally &&
            item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            drag = item;
        }
    }
    QVERIFY(drag);
    QVERIFY(drag->property("visible").toBool());
    const double altitude = rally->coordinate().altitude();
    QVERIFY(drag->setProperty("itemCoordinate", QVariant::fromValue(QGeoCoordinate(47.001, 8.002, -500))));
    QCOMPARE(rally->coordinate(), QGeoCoordinate(47.001, 8.002, altitude));
    rallyController->setCurrentRallyPoint(rallyController->points()->get(1));
    QVERIFY(!drag->property("visible").toBool());
    QQuickItem* clickMarker = nullptr;
    for (auto* item : named(QStringLiteral("planRallyPoint"))) {
        if (item->property("coordinate").value<QGeoCoordinate>() == rally->coordinate()) {
            clickMarker = item;
        }
    }
    QVERIFY(clickMarker);
    QVERIFY(!clickMarker->property("checked").toBool());
    auto* clickLoader = planStyleSource(clickMarker);
    QVERIFY(clickLoader);
    QPointer<QQuickItem> clickSource = qvariant_cast<QQuickItem*>(clickLoader->property("item"));
    QVERIFY(clickSource);
    QVERIFY(root->setProperty("center", QVariant::fromValue(rally->coordinate())));
    const auto pointerReady = [&] {
        // A correct target position alone can hide another marker's stale drag overlay.
        if (!rallyPositionsReady() || !clickSource) {
            return false;
        }
        const auto position = clickSource->mapToScene(QPointF(clickSource->width() / 2, clickSource->height() / 2));
        const auto mapCenter = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
        return clickSource->window() == &boundaryWindow && clickSource->isVisible() && clickSource->width() > 0 &&
               position.x() >= 0 && position.x() < boundaryWindow.width() && position.y() >= 0 &&
               position.y() < boundaryWindow.height() && (position - mapCenter).manhattanLength() < 2;
    };
    QTRY_VERIFY_WITH_TIMEOUT(pointerReady(), TestTimeout::mediumMs());
    const auto clickPosition =
        clickSource->mapToScene(QPointF(clickSource->width() / 2, clickSource->height() / 2)).toPoint();
    const auto mouseItemAtClick = [&](QQuickItem* item, auto&& self) -> QQuickItem* {
        if (!item || !item->isVisible() || !item->isEnabled()) {
            return nullptr;
        }
        for (auto* child : item->childItems()) {
            if (auto* hit = self(child, self)) {
                return hit;
            }
        }
        return item->acceptedMouseButtons().testFlag(Qt::LeftButton) &&
                       item->contains(item->mapFromScene(clickPosition))
                   ? item
                   : nullptr;
    };
    QVERIFY(mouseItemAtClick(clickSource.data(), mouseItemAtClick));
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        if (marker != clickMarker) {
            QVERIFY2(!mouseItemAtClick(planStyleSource(marker), mouseItemAtClick),
                     "Another rally marker accepts the target click");
        }
    }
    for (auto* item : map->childItems()) {
        if (item->isVisible() && item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            QVERIFY2(!mouseItemAtClick(item, mouseItemAtClick), "A visible drag item accepts the target click");
        }
    }
    QTest::mouseClick(&boundaryWindow, Qt::LeftButton, Qt::NoModifier, clickPosition);
    QTRY_COMPARE_WITH_TIMEOUT(rallyController->currentRallyPoint(), static_cast<QObject*>(rally),
                              TestTimeout::shortMs());
    QVERIFY(clickMarker->property("checked").toBool());

    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    // Disabling the context tears down outlines and returns every actual fill to its stock branch.
    QList<QPointer<QQuickItem>> outlines;
    QList<QPointer<QObject>> outlineLoaders;
    for (auto* item : named(QStringLiteral("planKeepInOutline")) + named(QStringLiteral("planKeepOutOutline"))) {
        outlines.append(item);
        auto* loader = item->parent();
        QVERIFY(loader && qvariant_cast<QQuickItem*>(loader->property("item")) == item);
        outlineLoaders.append(loader);
    }
    QList<QPointer<QQuickItem>> styledMarkers;
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        auto* loader = planStyleSource(marker);
        QVERIFY(loader);
        styledMarkers.append(qvariant_cast<QQuickItem*>(loader->property("item")));
        QVERIFY(styledMarkers.last());
    }
    polygon->setShowAltColor(false);
    for (const auto& outline : outlines) {
        QVERIFY(outline && outline->property("coalesceProjection").toBool());
        QVERIFY(!outline->property("_projectionPending").toBool());
    }
    // Clear the real Loaders with projections queued, before returning to the event loop.
    QVERIFY(root->setProperty("width", 780));
    QVERIFY(root->setProperty("height", 580));
    for (const auto& outline : outlines) {
        QVERIFY(outline->property("_projectionPending").toBool());
    }
    QVERIFY(root->setProperty("styleSupported", false));
    for (const auto& loader : outlineLoaders) {
        QVERIFY(loader && !loader->property("active").toBool());
        QVERIFY(!qvariant_cast<QQuickItem*>(loader->property("item")));
    }
    QObject projectionBarrier;
    bool projectionsDrained = false;
    QTimer::singleShot(0, &projectionBarrier, [&] { projectionsDrained = true; });
    QTRY_VERIFY_WITH_TIMEOUT(projectionsDrained, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(
        named(QStringLiteral("planKeepInOutline")).isEmpty() && named(QStringLiteral("planKeepOutOutline")).isEmpty(),
        TestTimeout::shortMs());
    for (const auto& outline : outlines) {
        QTRY_VERIFY_WITH_TIMEOUT(outline.isNull(), TestTimeout::shortMs());
    }
    for (const auto& marker : styledMarkers) {
        QTRY_VERIFY_WITH_TIMEOUT(marker.isNull(), TestTimeout::shortMs());
    }
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        const bool inclusion = fill->property("inclusion").toBool();
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), inclusion ? 2.0 : 0.0);
        QCOMPARE(fill->property("color").value<QColor>(),
                 inclusion ? QColor(QStringLiteral("transparent")) : QColor(QStringLiteral("orange")));
        QCOMPARE(fill->opacity(), inclusion ? 0.5 : 0.1);
    }
    QList<QPointer<QQuickItem>> stockMarkers;
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        QTRY_COMPARE_WITH_TIMEOUT(marker->property("markerShape").toString(), QStringLiteral("circle"),
                                  TestTimeout::shortMs());
        QCOMPARE(marker->property("markerShape").toString(), QStringLiteral("circle"));
        QCOMPARE(marker->property("label").toString(), QStringLiteral("R"));
        QCOMPARE(marker->property("markerBorderWidth").toReal(), 0.0);
        auto* loader = planStyleSource(marker);
        QVERIFY(loader);
        auto* stock = qvariant_cast<QQuickItem*>(loader->property("item"));
        QVERIFY(stock);
        stockMarkers.append(stock);
        QCOMPARE(marker->property("anchorPoint").toPointF(),
                 QPointF(stock->property("anchorPointX").toReal(), stock->property("anchorPointY").toReal()));
        QCOMPARE(marker->property("markerColor").value<QColor>(), stock->property("renderedBodyColor").value<QColor>());
        QCOMPARE(marker->property("markerBorderColor").value<QColor>(),
                 stock->property("renderedBorderColor").value<QColor>());
        QCOMPARE(marker->property("checked").toBool(), stock->property("checked").toBool());
    }
    QVERIFY(root->setProperty("styleSupported", true));
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepInOutline")).size(), 3, TestTimeout::shortMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepOutOutline")).size(), 2, TestTimeout::shortMs());
    for (const auto& marker : stockMarkers) {
        QTRY_VERIFY_WITH_TIMEOUT(marker.isNull(), TestTimeout::shortMs());
    }
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    // Removing the authoritative boundary must delete both its fill and its Loader-owned outline.
    QPointer<QQuickItem> removedOutline;
    // Locate the replacement created when the style was enabled again.
    for (auto* item : named(QStringLiteral("planKeepInOutline"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon")) {
            removedOutline = item;
        }
    }
    QVERIFY(removedOutline);
    QPointer<QQuickItem> removedFill;
    for (auto* item : named(QStringLiteral("planFenceFill"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon") &&
            item->property("inclusion").toBool()) {
            removedFill = item;
        }
    }
    QVERIFY(removedFill);
    polygons.removeAt(0);
    polygon->deleteLater();
    QTRY_VERIFY_WITH_TIMEOUT(removedFill.isNull() && removedOutline.isNull(), TestTimeout::shortMs());
    planStyleCheckBoundaries(map, 7601, entry->color(), 1, 2);
    QVERIFY(!QTest::currentTestFailed());
    QList<QPointer<QQuickItem>> remaining;
    for (const auto& name :
         {QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline"),
          QStringLiteral("planRallyPoint"), QStringLiteral("planBreachReturnPoint")}) {
        for (auto* item : named(name)) {
            remaining.append(item);
        }
    }
    for (auto* item : map->childItems()) {
        if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            remaining.append(item);
        }
    }
    QVERIFY(root->setProperty("visualsEnabled", false));
    for (const auto& item : remaining) {
        QTRY_VERIFY_WITH_TIMEOUT(item.isNull(), TestTimeout::shortMs());
    }
    root.reset();
    polygons.clearAndDeleteContents();
    circles.clearAndDeleteContents();
}

void PlanStyleUITest::_uploadPlan(Vehicle* vehicle, int count)
{
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->isInitialConnectComplete() && vehicle->initialPlanRequestComplete(),
                             TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid(), TestTimeout::longMs());
    const auto home = vehicle->homePosition();
    QList<MissionItem*> items;
    for (int i = 0; i < count; ++i) {
        const auto coordinate = home.atDistanceAndAzimuth(i * 180, count == 4 ? 90 : 180);
        items.append(new MissionItem(i, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, 0,
                                     coordinate.latitude(), coordinate.longitude(), 100 + i * 10, true, false, this));
    }
    QSignalSpy missionSent(vehicle->missionManager(), &PlanManager::sendComplete);
    QVERIFY(missionSent.isValid());
    vehicle->missionManager()->writeMissionItems(items);
    QTRY_COMPARE_WITH_TIMEOUT(missionSent.count(), 1, TestTimeout::longMs());
    QVERIFY(!missionSent.first().first().toBool());
    QmlObjectListModel polygons;
    QmlObjectListModel circles;
    for (bool inclusion : {true, false}) {
        auto* polygon = new QGCFencePolygon(inclusion);
        for (double azimuth : {0.0, 120.0, 240.0}) {
            polygon->appendVertex(home.atDistanceAndAzimuth(inclusion ? 400 + count * 10 : 80 + count * 10, azimuth));
        }
        polygons.append(polygon);
        circles.append(new QGCFenceCircle(home.atDistanceAndAzimuth(count * 50, inclusion ? 90 : 180),
                                          inclusion ? 90 : 30, inclusion));
    }
    QSignalSpy fenceSent(vehicle->geoFenceManager(), &GeoFenceManager::sendComplete);
    QVERIFY(fenceSent.isValid());
    vehicle->geoFenceManager()->sendToVehicle(home, polygons, circles);
    polygons.clearAndDeleteContents();
    circles.clearAndDeleteContents();
    QTRY_COMPARE_WITH_TIMEOUT(fenceSent.count(), 1, TestTimeout::longMs());
    QVERIFY(!fenceSent.first().first().toBool());
    QList<RallyPointData> points;
    for (int i = 0; i < count - 1; ++i) {
        auto coordinate = home.atDistanceAndAzimuth(150 + i * 100, 315);
        coordinate.setAltitude(80 + i * 10);
        points.append({coordinate, MAV_FRAME_GLOBAL_RELATIVE_ALT});
    }
    QSignalSpy rallySent(vehicle->rallyPointManager(), &RallyPointManager::sendComplete);
    QVERIFY(rallySent.isValid());
    vehicle->rallyPointManager()->sendToVehicle(points);
    QTRY_COMPARE_WITH_TIMEOUT(rallySent.count(), 1, TestTimeout::longMs());
    QVERIFY(!rallySent.first().first().toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!vehicle->missionManager()->inProgress() && !vehicle->geoFenceManager()->inProgress() &&
                                 !vehicle->rallyPointManager()->inProgress(),
                             TestTimeout::longMs());
}

void PlanStyleUITest::_setPlanLayer(QQuickItem* view, int layer)
{
    if (view->property("_useNewPlanPanel").toBool()) {
        const QStringList tabs{QStringLiteral("plan"), QStringLiteral("mission"), QStringLiteral("fence"),
                               QStringLiteral("rally")};
        QVERIFY(layer >= 0 && layer < tabs.size());
        QVERIFY(clickButton(QStringLiteral("planPanel_tab_") + tabs[layer]));
    } else if (view->property("_editingLayer").toInt() != layer) {
        const QStringList groups{QString(), QStringLiteral("missionGroup"), QStringLiteral("fenceGroup"),
                                 QStringLiteral("rallyGroup")};
        QVERIFY(layer > 0 && layer < groups.size());
        QVERIFY(clickButton(QStringLiteral("layerSwitcher_activeButton")));
        QVERIFY(clickButton(QStringLiteral("layerSwitcher_choice_") + groups[layer]));
    }
    QTRY_COMPARE_WITH_TIMEOUT(view->property("_editingLayer").toInt(), layer, TestTimeout::shortMs());
}

void PlanStyleUITest::_checkPlan(QQuickItem* view, quint32 id, const QColor& color, int itemCount, int rallyCount)
{
    auto* map = qvariant_cast<QQuickItem*>(view->property("editorMap"));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY(map && master);
    QVERIFY(!map->property("camera").isValid());
    QCOMPARE(master->planVehicleId(), id);
    const auto contexts = map->findChildren<QObject*>(QStringLiteral("planViewPlanStyleContext"));
    QCOMPARE(contexts.size(), 1);
    auto* context = contexts.first();
    QCOMPARE(context->property("vehicleId").toUInt(), id);
    QVERIFY(context->property("enabled").toBool());
    QCOMPARE(context->property("lineColor").value<QColor>(), color);
    const int layer = view->property("_editingLayer").toInt();
    const qreal missionOpacity = layer == 1 ? 1 : 0.5;
    const qreal fenceOpacity = layer == 2 ? 1 : 0.5;
    const qreal rallyOpacity = layer == 3 ? 1 : 0.5;
    auto* mission = master->missionController();
    QCOMPARE(mission->visualItems()->count(), itemCount);
    auto* segments = mission->simpleFlightPathSegments();
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planMissionRoute")).size(), segments->count(),
                              TestTimeout::mediumMs());
    QVERIFY(segments->count() > 0);
    QSet<int> matchedSegments;
    for (auto* route : named(QStringLiteral("planMissionRoute"))) {
        QCOMPARE(route->property("vehicleId").toUInt(), id);
        QCOMPARE(planStyleColor(route, QStringLiteral("line.color")), color);
        QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 4.0);
        QCOMPARE(planStyleOpacityBelowMap(route, map), missionOpacity);
        const auto path = route->property("path").toList();
        QCOMPARE(path.size(), 2);
        int matched = -1;
        for (int i = 0; i < segments->count(); ++i) {
            auto* segment = segments->value<FlightPathSegment*>(i);
            if (!matchedSegments.contains(i) && path[0].value<QGeoCoordinate>() == segment->coordinate1() &&
                path[1].value<QGeoCoordinate>() == segment->coordinate2()) {
                matched = i;
                break;
            }
        }
        QVERIFY(matched >= 0);
        matchedSegments.insert(matched);
    }
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planWaypoint")).size(), itemCount - 1, TestTimeout::mediumMs());
    QSet<int> sequences;
    for (auto* marker : named(QStringLiteral("planWaypoint"))) {
        QCOMPARE(qvariant_cast<QObject*>(marker->property("planStyle")), context);
        QCOMPARE(marker->property("vehicleId").toUInt(), id);
        QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), color);
        QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
        QCOMPARE(marker->property("renderedBorderWidth").toReal(), 2.0);
        QCOMPARE(marker->property("renderedTextColor").value<QColor>(), QColor(QStringLiteral("black")));
        QCOMPARE(marker->property("interactive").toBool(), layer == 1);
        const int sequence = marker->property("sequenceNumber").toInt();
        QVERIFY(sequence > 0 && sequence < itemCount && !sequences.contains(sequence));
        sequences.insert(sequence);
        auto* item = mission->visualItems()->value<VisualMissionItem*>(sequence);
        QCOMPARE(marker->property("coordinate").value<QGeoCoordinate>(), item->coordinate());
        QCOMPARE(marker->property("checked").toBool(), item->isCurrentItem());
        QCOMPARE(marker->property("index").toInt(), sequence);
        QCOMPARE(marker->property("renderedIndexText").toString(), QString::number(sequence));
        auto* source = planStyleSource(marker);
        QVERIFY(source);
        auto* body = qvariant_cast<QQuickItem*>(source->property("renderedIndicator"));
        QVERIFY(body);
        QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map) && body->width() > 0, TestTimeout::shortMs());
        QCOMPARE(body->width(), source->property("_indicatorRadius").toReal() * 2);
        QCOMPARE(source->property("small").toBool(), !item->isCurrentItem());
        QCOMPARE(source->property("highlightSelected").toBool(), true);
        QCOMPARE(planStyleOpacityBelowMap(body, map), missionOpacity);
        const auto halos = named(QStringLiteral("planWaypointSelectionHalo"));
        const auto ownHalos = planStyleItems(marker, QStringLiteral("planWaypointSelectionHalo"));
        QCOMPARE(ownHalos.size(), 1);
        QVERIFY(halos.contains(ownHalos.first()));
        QCOMPARE(ownHalos.first()->isVisible(), item->isCurrentItem() && marker->isVisible());
        QCOMPARE(ownHalos.first()->width(), body->width() * 2);
    }
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planHomeMarker")).size(), 1, TestTimeout::mediumMs());
    auto* home = named(QStringLiteral("planHomeMarker")).first();
    QCOMPARE(home->property("vehicleId").toUInt(), id);
    QCOMPARE(home->property("coordinate").value<QGeoCoordinate>(), mission->plannedHomePosition());
    auto* house = planStyleSource(home);
    QVERIFY(house && house->inherits("QQuickImage"));
    QCOMPARE(house->property("source").toUrl(), QUrl(QStringLiteral("qrc:///qmlimages/MapHome.svg")));
    const auto accents = planStyleItems(home, QStringLiteral("planHomeAccent"));
    QCOMPARE(accents.size(), 1);
    QVERIFY(accents.first()->isVisible());
    QCOMPARE(planStyleColor(accents.first(), QStringLiteral("border.color")), color);
    QCOMPARE(planStyleOpacityBelowMap(accents.first(), map), missionOpacity);

    auto* fence = master->geoFenceController();
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planFenceFill")).size(),
                              fence->polygons()->count() + fence->circles()->count() + 1, TestTimeout::mediumMs());
    auto* fenceVisuals = findItem(map, QStringLiteral("planView_geoFenceMapVisuals"));
    auto* rallyVisuals = findItem(map, QStringLiteral("planView_rallyPointMapVisuals"));
    QVERIFY(fenceVisuals && rallyVisuals);
    QCOMPARE(qvariant_cast<QObject*>(fenceVisuals->property("planStyle")), context);
    QCOMPARE(qvariant_cast<QObject*>(rallyVisuals->property("planStyle")), context);
    QCOMPARE(fenceVisuals->property("interactive").toBool(), layer == 2);
    QCOMPARE(rallyVisuals->property("interactive").toBool(), layer == 3);
    planStyleCheckBoundaries(map, id, color, fence->polygons()->count(), fence->circles()->count());
    QVERIFY(!QTest::currentTestFailed());
    QSet<QObject*> matchedFences;
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        QCOMPARE(fill->opacity(), fenceOpacity);
        QCOMPARE(planStyleOpacityBelowMap(fill, map), fenceOpacity);
        if (!fill->property("inclusion").toBool()) {
            QVERIFY(qAbs(fill->property("color").value<QColor>().alphaF() * planStyleOpacityBelowMap(fill, map) -
                         0.35 * fenceOpacity) < 0.005);
        }
        const auto kind = fill->property("boundaryKind").toString();
        if (kind == QStringLiteral("parameterCircle")) {
            QCOMPARE(fill->property("center").value<QGeoCoordinate>(), mission->plannedHomePosition());
            QCOMPARE(fill->property("radius").toReal(), fence->paramCircularFence());
            continue;
        }
        QObject* matched = nullptr;
        auto* model = kind == QStringLiteral("polygon") ? fence->polygons() : fence->circles();
        for (int i = 0; i < model->count(); ++i) {
            auto* candidate = model->get(i);
            const bool geometryMatches =
                kind == QStringLiteral("polygon")
                    ? fill->property("path").toList() == candidate->property("dragPath").toList()
                    : fill->property("center") == candidate->property("center");
            if (!matchedFences.contains(candidate) && geometryMatches &&
                fill->property("inclusion") == candidate->property("inclusion")) {
                matched = candidate;
                break;
            }
        }
        QVERIFY(matched);
        matchedFences.insert(matched);
    }
    for (auto* outline : named(QStringLiteral("planKeepInOutline")) + named(QStringLiteral("planKeepOutOutline"))) {
        QCOMPARE(planStyleOpacityBelowMap(planStyleSource(outline), map), fenceOpacity);
    }
    auto* rally = master->rallyPointController();
    QCOMPARE(rally->points()->count(), rallyCount);
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planRallyPoint")).size(), rallyCount, TestTimeout::mediumMs());
    QSet<QObject*> matchedPoints;
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        planStyleCheckRally(map, marker, id, color);
        QVERIFY(!QTest::currentTestFailed());
        auto* point = qvariant_cast<QObject*>(marker->property("rallyPointObject"));
        QVERIFY(point && rally->points()->contains(point) && !matchedPoints.contains(point));
        matchedPoints.insert(point);
        QCOMPARE(marker->property("coordinate"), point->property("coordinate"));
        QCOMPARE(marker->property("checked").toBool(), layer == 3 && rally->currentRallyPoint() == point);
        auto* loader = planStyleSource(marker);
        auto* source = qvariant_cast<QQuickItem*>(loader->property("item"));
        QVERIFY(source);
        QCOMPARE(source->property("interactive").toBool(), layer == 3);
        QCOMPARE(planStyleOpacityBelowMap(source, map), rallyOpacity);
    }
}

void PlanStyleUITest::_editPlan(QQuickItem* view)
{
    auto* map = qvariant_cast<QQuickItem*>(view->property("editorMap"));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY(map && master);
    auto* mission = master->missionController();
    auto* fence = master->geoFenceController();
    auto* rally = master->rallyPointController();
    const QColor color = QGCCorePlugin::instance()->planStyle()->vehicleColor(master->planVehicleId());
    const auto markerFor = [map](const QString& name, const QGeoCoordinate& coordinate) -> QQuickItem* {
        for (auto* marker : planStyleItems(map, name)) {
            if (marker->property("coordinate").value<QGeoCoordinate>() == coordinate) {
                return marker;
            }
        }
        return nullptr;
    };
    const auto dragFor = [map](QQuickItem* marker) -> QQuickItem* {
        for (auto* item : map->childItems()) {
            if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0 &&
                qvariant_cast<QQuickItem*>(item->property("itemIndicator")) == marker) {
                return item;
            }
        }
        return nullptr;
    };
    const auto preparePointer = [&](QQuickItem* marker) {
        QVERIFY(marker && marker->isVisible());
        QVERIFY(map->setProperty("zoomLevel", 17));
        QVERIFY(map->setProperty("center", marker->property("coordinate")));
        QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
        auto* source = planStyleSource(marker);
        QVERIFY(source);
        const auto position = source->mapToScene(marker->property("anchorPoint").toPointF());
        QVERIFY(map->property("centerViewport").toRectF().contains(map->mapFromScene(position)));
        QVERIFY(planStyleMouseItemAt(source, position) || planStyleMouseItemAt(dragFor(marker), position));
        for (auto* item : map->childItems()) {
            if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0 && item != dragFor(marker)) {
                QVERIFY2(!planStyleMouseItemAt(item, position), "A competing drag overlay accepts the target click");
            }
        }
    };
    const auto dragPoint = [&](QQuickItem* marker, QObject* point, Fact* altitude) {
        preparePointer(marker);
        QVERIFY(!QTest::currentTestFailed());
        auto* drag = dragFor(marker);
        QVERIFY(drag && drag->isVisible());
        auto* source = planStyleSource(marker);
        QVERIFY(source);
        QVERIFY(drag->width() > 0 && drag->height() > 0);
        QCOMPARE(drag->property("_itemIndicatorWidth").toReal(), marker->width());
        QCOMPARE(drag->property("_itemIndicatorHeight").toReal(), marker->height());
        const auto before = point->property("coordinate").value<QGeoCoordinate>();
        QVERIFY2(before.isValid() && altitude && qIsFinite(altitude->rawValue().toDouble()),
                 "Plan point drag requires a valid coordinate and a finite altitude Fact");
        const auto beforeAltitude = altitude->rawValue().toDouble();
        const auto sameCoordinateAltitude = [&](const QGeoCoordinate& coordinate) {
            return qIsNaN(before.altitude()) ? qIsNaN(coordinate.altitude())
                                             : coordinate.altitude() == before.altitude();
        };
        QVERIFY2(sameCoordinateAltitude(drag->property("itemCoordinate").value<QGeoCoordinate>()),
                 "The selected drag item must start with its model's coordinate altitude");
        const auto start = source->mapToScene(marker->property("anchorPoint").toPointF()).toPoint();
        const QPoint delta(-70, 50);
        const int activationDistance = QGuiApplication::styleHints()->startDragDistance() + 4;
        const QPoint activePointer = start + QPoint(-activationDistance - 4, activationDistance + 4);
        QTest::mousePress(_window, Qt::LeftButton, Qt::NoModifier, start);
        // The threshold-crossing event rebases Qt's drag; the next move activates it.
        QTest::mouseMove(_window, start + QPoint(-activationDistance, activationDistance));
        QTest::mouseMove(_window, activePointer);
        const bool active = waitForCondition(
            [&] {
                return planStyleRenderMap(map) && drag->property("_dragStartSignalled").toBool() &&
                       planStyleMapItemProjected(map, marker);
            },
            TestTimeout::shortMs(), QStringLiteral("Plan point drag active and projected"));
        if (!active) {
            QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activePointer);
            QVERIFY(active);
        }
        const auto activeAnchor = source->mapToScene(marker->property("anchorPoint").toPointF());
        for (int i = 1; i <= 10; ++i) {
            QTest::mouseMove(_window, activePointer + delta * i / 10);
        }
        const bool moved = waitForCondition(
            [&] {
                return planStyleRenderMap(map) &&
                       point->property("coordinate").value<QGeoCoordinate>().distanceTo(before) > 1 &&
                       planStyleMapItemProjected(map, marker);
            },
            TestTimeout::shortMs(), QStringLiteral("Plan point moved while held"));
        const auto held = point->property("coordinate").value<QGeoCoordinate>();
        const auto heldDragCoordinate = drag->property("itemCoordinate").value<QGeoCoordinate>();
        const auto heldAltitude = altitude->rawValue().toDouble();
        const auto heldAnchor = source->mapToScene(marker->property("anchorPoint").toPointF());
        QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activePointer + delta);
        QVERIFY(moved);
        QVERIFY((heldAnchor - activeAnchor - delta).manhattanLength() < 5);
        // SimpleMissionItem coordinates are 2D; altitude lives in a separate Fact. Rally coordinates are 3D.
        QVERIFY(sameCoordinateAltitude(held));
        QVERIFY(sameCoordinateAltitude(heldDragCoordinate));
        QVERIFY(sameCoordinateAltitude(point->property("coordinate").value<QGeoCoordinate>()));
        QCOMPARE(heldAltitude, beforeAltitude);
        QCOMPARE(altitude->rawValue().toDouble(), beforeAltitude);
        QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
        const auto end = planStyleSource(marker)->mapToScene(marker->property("anchorPoint").toPointF());
        QVERIFY((end - activeAnchor - delta).manhattanLength() < 5);
        QVERIFY((end - heldAnchor).manhattanLength() < 5);
    };

    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    mission->setCurrentPlanViewSeqNum(1, true);
    auto* waypoint = mission->visualItems()->value<SimpleMissionItem*>(2);
    QVERIFY2(waypoint && waypoint->command() == MAV_CMD_NAV_WAYPOINT && waypoint->specifiesAltitude() &&
                 qIsFinite(waypoint->altitude()->rawValue().toDouble()),
             "Mission drag fixture must select a NAV_WAYPOINT with a finite altitude Fact");
    QVERIFY(qIsNaN(waypoint->coordinate().altitude()));
    auto* waypointMarker = markerFor(QStringLiteral("planWaypoint"), waypoint->coordinate());
    QVERIFY(waypointMarker);
    preparePointer(waypointMarker);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_clickItemAt(planStyleSource(waypointMarker), 0.5, 0.5, QStringLiteral("Plan waypoint")));
    QTRY_COMPARE_WITH_TIMEOUT(mission->currentPlanViewItem(), waypoint, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(dragFor(waypointMarker), TestTimeout::shortMs());
    const auto waypointHalos = planStyleItems(waypointMarker, QStringLiteral("planWaypointSelectionHalo"));
    QCOMPARE(waypointHalos.size(), 1);
    QVERIFY(waypointHalos.first()->isVisible());
    QCOMPARE(waypointMarker->property("renderedBodyColor").value<QColor>(), color);
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("planPanel_inspectorLoader")));
    dragPoint(waypointMarker, waypoint, waypoint->altitude());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(mission->visualItems()->count(), 4);

    _setPlanLayer(view, 3);
    QVERIFY(!QTest::currentTestFailed());
    rally->setCurrentRallyPoint(rally->points()->get(0));
    auto* point = rally->points()->value<RallyPoint*>(1);
    QVERIFY(point);
    auto* rallyMarker = markerFor(QStringLiteral("planRallyPoint"), point->coordinate());
    QVERIFY(rallyMarker);
    preparePointer(rallyMarker);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_clickItemAt(planStyleSource(rallyMarker), 0.5, 0.5, QStringLiteral("Plan rally point")));
    QTRY_COMPARE_WITH_TIMEOUT(rally->currentRallyPoint(), static_cast<QObject*>(point), TestTimeout::shortMs());
    QVERIFY(rallyMarker->property("checked").toBool());
    const auto rallyHalos = planStyleItems(rallyMarker, QStringLiteral("planRallySelectionHalo"));
    QCOMPARE(rallyHalos.size(), 1);
    QVERIFY(rallyHalos.first()->isVisible());
    QCOMPARE(point->altitudeFrame(), QGroundControlQmlGlobal::AltitudeFrameRelative);
    dragPoint(rallyMarker, point, point->altitude());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(rally->points()->count(), 3);
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(!dragFor(rallyMarker)->isVisible());
    auto* rallySource = qvariant_cast<QQuickItem*>(planStyleSource(rallyMarker)->property("item"));
    QVERIFY(rallySource && !rallySource->property("interactive").toBool());
    QVERIFY(!planStyleMouseItemAt(rallySource, rallySource->mapToScene(QPointF(0, 0))));

    _setPlanLayer(view, 2);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(clickButton(QStringLiteral("planPanel_fenceRow_polygon_0")));
    auto* polygon = fence->polygons()->value<QGCFencePolygon*>(0);
    QVERIFY(polygon && polygon->interactive());
    QQuickItem* polygonFill = nullptr;
    QQuickItem* polygonOutline = nullptr;
    for (auto* item : planStyleItems(map, QStringLiteral("planFenceFill"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon") &&
            item->property("path").toList() == polygon->path()) {
            polygonFill = item;
        }
    }
    for (auto* item : planStyleItems(map, QStringLiteral("planKeepInOutline"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon")) {
            polygonOutline = item;
        }
    }
    QVERIFY(polygonFill && polygonOutline);
    QQuickItem* vertexDrag = nullptr;
    const auto findVertexDrag = [&] {
        for (auto* item : map->childItems()) {
            if (item->property("polygonVertex").isValid() && item->property("polygonVertex").toInt() == 0 &&
                item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
                return item;
            }
        }
        return static_cast<QQuickItem*>(nullptr);
    };
    QTRY_VERIFY_WITH_TIMEOUT((vertexDrag = findVertexDrag()), TestTimeout::shortMs());
    auto* vertexHandle = qvariant_cast<QQuickItem*>(vertexDrag->property("itemIndicator"));
    QVERIFY(vertexHandle);
    QVERIFY(vertexDrag->z() >= 51 && vertexDrag->z() <= 53);
    QVERIFY(vertexHandle->z() >= 51 && vertexHandle->z() <= 53);
    QVERIFY(polygonOutline->z() < vertexDrag->z());
    preparePointer(vertexHandle);
    QVERIFY(!QTest::currentTestFailed());
    const auto committed = polygonFill->property("path").toList();
    QSignalSpy committedChanges(polygon, &QGCMapPolygon::pathChanged);
    QVERIFY(committedChanges.isValid());
    const auto start =
        planStyleSource(vertexHandle)->mapToScene(vertexHandle->property("anchorPoint").toPointF()).toPoint();
    const QPoint delta(-60, 45);
    QTest::mousePress(_window, Qt::LeftButton, Qt::NoModifier, start);
    for (int i = 1; i <= 10; ++i) {
        QTest::mouseMove(_window, start + delta * i / 10);
    }
    const bool live = waitForCondition(
        [&] {
            return planStyleRenderMap(map) && polygon->vertexDrag() &&
                   polygonOutline->property("vertices").toList() != committed &&
                   polygonOutline->property("vertices").toList() == polygon->path();
        },
        TestTimeout::shortMs(), QStringLiteral("Plan vertex preview follows held pointer"));
    const auto heldPath = polygonOutline->property("vertices").toList();
    const qreal heldBorder = QQmlProperty(polygonFill, QStringLiteral("border.width")).read().toReal();
    const int commitsWhileHeld = committedChanges.count();
    QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, start + delta);
    QVERIFY(live);
    QVERIFY(heldPath != committed);
    QCOMPARE(heldBorder, 0.0);
    QCOMPARE(commitsWhileHeld, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!polygon->vertexDrag(), TestTimeout::shortMs());
    planStyleCheckBoundaries(map, master->planVehicleId(), color, 2, 2);
    QVERIFY(!QTest::currentTestFailed());

    // The same native editor also previews a whole-polygon centre drag before its committed-path notification.
    const auto beforeCenter = polygonOutline->property("vertices").toList();
    polygon->setCenterDrag(true);
    polygon->setVertexDrag(true);
    polygon->setCenter(polygon->center().atDistanceAndAzimuth(40, 180));
    QTRY_VERIFY_WITH_TIMEOUT(polygonOutline->property("vertices").toList() != beforeCenter, TestTimeout::shortMs());
    QCOMPARE(polygonOutline->property("vertices").toList(), polygon->path());
    QCOMPARE(QQmlProperty(polygonFill, QStringLiteral("border.width")).read().toReal(), 0.0);
    polygon->setCenterDrag(false);
    polygon->setVertexDrag(false);

    QQuickItem* splitHandle = nullptr;
    for (auto* item : map->childItems()) {
        if (item->property("vertexIndex").isValid() && !item->property("distance").isValid() && planStyleSource(item)) {
            splitHandle = item;
            break;
        }
    }
    QVERIFY(splitHandle);
    preparePointer(splitHandle);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_clickItemAt(planStyleSource(splitHandle), 0.5, 0.5, QStringLiteral("fence split handle")));
    QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), 4, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT((vertexDrag = findVertexDrag()), TestTimeout::shortMs());
    QVERIFY(vertexDrag->isVisible());
    QVERIFY(QMetaObject::invokeMethod(vertexDrag, "clicked"));
    QQuickItem* removeVertex = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((removeVertex = planStyleTab(_rootItem, QStringLiteral("Remove vertex"))),
                             TestTimeout::shortMs());
    QVERIFY(_clickItemAt(removeVertex, 0.5, 0.5, QStringLiteral("remove fence vertex")));
    QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), 3, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());

    auto* trace = planStyleTab(map, QStringLiteral("Trace"));
    QVERIFY(trace);
    QVERIFY(_clickItemAt(trace, 0.5, 0.5, QStringLiteral("trace fence")));
    QTRY_VERIFY_WITH_TIMEOUT(polygon->traceMode(), TestTimeout::shortMs());
    QCOMPARE(polygon->count(), 0);
    const auto viewport = map->property("centerViewport").toRectF();
    int tracedVertices = 0;
    for (const QPointF& fraction : {QPointF(0.3, 0.4), QPointF(0.7, 0.45), QPointF(0.5, 0.7)}) {
        QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
        const auto position =
            viewport.topLeft() + QPointF(viewport.width() * fraction.x(), viewport.height() * fraction.y());
        QTest::mouseClick(_window, Qt::LeftButton, Qt::NoModifier, map->mapToScene(position).toPoint());
        ++tracedVertices;
        QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), tracedVertices, TestTimeout::shortMs());
    }
    QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), 3, TestTimeout::shortMs());
    auto* done = planStyleTab(map, QStringLiteral("Done Tracing"));
    QVERIFY(done);
    QVERIFY(_clickItemAt(done, 0.5, 0.5, QStringLiteral("finish tracing")));
    QVERIFY(!polygon->traceMode());
    QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());

    QVERIFY(clickButton(QStringLiteral("planPanel_fenceRow_circle_0")));
    auto* circle = fence->circles()->value<QGCFenceCircle*>(0);
    QVERIFY(circle && circle->interactive());
    QQuickItem* circleVisual = nullptr;
    for (auto* item : planStyleItems(map, QString())) {
        if (qvariant_cast<QObject*>(item->property("mapCircle")) == circle &&
            item->property("_dragHandlesComponent").isValid()) {
            circleVisual = item;
            break;
        }
    }
    QVERIFY(circleVisual);
    auto* handles = qvariant_cast<QObject*>(circleVisual->property("_dragHandlesComponent"));
    QVERIFY(handles);
    auto* centerDrag = qvariant_cast<QQuickItem*>(handles->property("centerDragArea"));
    auto* radiusDrag = qvariant_cast<QQuickItem*>(handles->property("radiusDragArea"));
    QVERIFY(centerDrag && radiusDrag && centerDrag->isVisible() && radiusDrag->isVisible());
    const auto center = circle->center().atDistanceAndAzimuth(30, 45);
    QVERIFY(centerDrag->setProperty("itemCoordinate", QVariant::fromValue(center)));
    QCOMPARE(circle->center(), center);
    QVERIFY(radiusDrag->setProperty("itemCoordinate", QVariant::fromValue(center.atDistanceAndAzimuth(175, 90))));
    QVERIFY(qAbs(circle->radius()->rawValue().toDouble() - 175) < 0.01);
    for (const auto& property : {"centerDragHandle", "radiusDragHandle"}) {
        auto* handle = qvariant_cast<QQuickItem*>(handles->property(property));
        QVERIFY(handle && handle->z() == 52);
    }
    planStyleCheckBoundaries(map, master->planVehicleId(), color, 2, 2);
    QVERIFY(!QTest::currentTestFailed());
    QPointer<QObject> oldHandles = handles;
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_VERIFY_WITH_TIMEOUT(oldHandles.isNull(), TestTimeout::shortMs());
}

void PlanStyleUITest::_planEditor_data()
{
    QTest::addColumn<bool>("tabbed");
    QTest::addColumn<bool>("lateRegistry");
    QTest::newRow("tabbed") << true << false;
    QTest::newRow("tree") << false << false;
    QTest::newRow("late-registry") << true << true;
}

void PlanStyleUITest::_planEditor()
{
    QFETCH(bool, tabbed);
    QFETCH(bool, lateRegistry);
    setUseTabbedPlanPanel(tabbed);
    setUseGeoMapPlanEngine(false);
    if (lateRegistry) {
        // Delay registry initialization, while keeping the real MainWindow, Plan view and vehicle downloads.
        const auto replacement = _directory.filePath(QStringLiteral("DelayedNavigation.qml"));
        QFile file(replacement);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray qml = "import QtQuick\nItem { property var hostWindow }\n";
        QCOMPARE(file.write(qml), qml.size());
        file.close();
        overrideQmlFile(QUrl(QStringLiteral("qrc:/qml/Custom/COP/COPNavigation.qml")),
                        QUrl::fromLocalFile(replacement));
    }
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* registry = _controller();
    QVERIFY(registry);
    auto* policy = QGCCorePlugin::instance()->planStyle();
    QVERIFY(policy);
    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> first = _start(MAV_TYPE_GROUND_ROVER);
    QPointer<MockLink> second = _start(MAV_TYPE_QUADROTOR);
    QVERIFY(first && second);
    auto* manager = MultiVehicleManager::instance();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    const auto vehicleFor = [manager](quint32 id) -> Vehicle* {
        for (int i = 0; i < manager->vehicles()->count(); ++i) {
            auto* vehicle = manager->vehicles()->value<Vehicle*>(i);
            if (vehicle && vehicle->id() == id) {
                return vehicle;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(vehicleFor(first->vehicleId()) && vehicleFor(second->vehicleId()), TestTimeout::longMs());
    auto* a = vehicleFor(first->vehicleId());
    auto* b = vehicleFor(second->vehicleId());
    QVERIFY(a && b && a != b);
    const quint32 aId = a->id();
    const quint32 bId = b->id();
    _uploadPlan(a, 4);
    QVERIFY(!QTest::currentTestFailed());
    _uploadPlan(b, 6);
    QVERIFY(!QTest::currentTestFailed());
    for (auto* vehicle : {a, b}) {
        auto* parameters = vehicle->parameterManager();
        QVERIFY(parameters);
        if (parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))) {
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS"))
                ->setRawValue(vehicle == a ? 650 : 750);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE"))
                ->setRawValue(2);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))
                ->setRawValue(1);
        }
    }
    manager->setActiveVehicle(a);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewPlan")));
    auto* view = findVisibleItem(_rootItem, QStringLiteral("mainView_plan"));
    auto* map = findVisibleItem(_rootItem, QStringLiteral("planView_map"));
    QVERIFY(view && map);
    QVERIFY(!map->property("camera").isValid());
    QCOMPARE(view->property("_useNewPlanPanel").toBool(), tabbed);
    QVERIFY(findVisibleItem(_rootItem, tabbed ? QStringLiteral("planPanel") : QStringLiteral("planView_planTree")));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY(master);
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), a, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(!master->syncInProgress(), TestTimeout::longMs());
    QSignalSpy loaded(master->rallyPointController(), &RallyPointController::loadComplete);
    QVERIFY(loaded.isValid());
    master->loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty() && !master->syncInProgress(), TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->planVehicleId(), aId, TestTimeout::longMs());
    auto* mission = master->missionController();
    auto* fence = master->geoFenceController();
    auto* rally = master->rallyPointController();
    QCOMPARE(mission->visualItems()->count(), 4);
    QCOMPARE(fence->polygons()->count(), 2);
    QCOMPARE(fence->circles()->count(), 2);
    QCOMPARE(rally->points()->count(), 3);
    auto* tracker = qvariant_cast<QObject*>(map->property("positionTracker"));
    QVERIFY(tracker);
    QVERIFY(tracker->setProperty("userInteracting", true));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker->property("animating").toBool(), TestTimeout::mediumMs());
    QVERIFY(map->setProperty("zoomLevel", 16));
    QVERIFY(map->setProperty("center", QVariant::fromValue(mission->plannedHomePosition())));
    const auto contexts = map->findChildren<QObject*>(QStringLiteral("planViewPlanStyleContext"));
    QCOMPARE(contexts.size(), 1);
    QPointer<QObject> context = contexts.first();
    QColor aColor = policy->vehicleColor(aId);
    if (lateRegistry) {
        QVERIFY(!registry->colorForSysid(aId).isValid());
        QVERIFY(!registry->colorForSysid(bId).isValid());
        aColor = context->property("lineColor").value<QColor>();
        QCOMPARE(aColor, QColor(QStringLiteral("#ffa000")));
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
        QList<QPointer<QQuickItem>> retained;
        for (const auto& name :
             {QStringLiteral("planMissionRoute"), QStringLiteral("planWaypoint"), QStringLiteral("planHomeAccent"),
              QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"),
              QStringLiteral("planKeepOutOutline"), QStringLiteral("planRallyPoint")}) {
            for (auto* item : planStyleItems(map, name)) {
                retained.append(item);
            }
        }
        auto* roles = _engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
        QVERIFY(roles && !roles->availableRoles().isEmpty());
        roles->addEntry(bId, roles->availableRoles().first(), QStringLiteral("Late Plan target"), 0);
        QCOMPARE(roles->roleForSysid(bId), roles->availableRoles().first());
        registry->initialize(roles);
        QTRY_VERIFY_WITH_TIMEOUT(registry->colorForSysid(aId).isValid() && registry->colorForSysid(bId).isValid(),
                                 TestTimeout::shortMs());
        aColor = registry->colorForSysid(aId);
        QVERIFY(aColor != QColor(QStringLiteral("#ffa000")));
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
        for (const auto& item : retained) {
            QVERIFY(item);
            QVERIFY(planStyleItems(map, item->objectName()).contains(item.data()));
        }
    }
    QVERIFY(aColor.isValid());
    const QColor bColor = registry->colorForSysid(bId);
    QVERIFY(bColor.isValid() && aColor != bColor);
    mission->setCurrentPlanViewSeqNum(1, true);
    rally->setCurrentRallyPoint(rally->points()->get(0));
    for (int layer : {1, 2, 3, 1, 2, 3, 1}) {
        _setPlanLayer(view, layer);
        QVERIFY(!QTest::currentTestFailed());
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
    }
    if (tabbed) {
        _setPlanLayer(view, 0);
        QVERIFY(!QTest::currentTestFailed());
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
    }
    if (tabbed && !lateRegistry) {
        _editPlan(view);
        QVERIFY(!QTest::currentTestFailed());
    }

    auto* waypoint = mission->visualItems()->value<VisualMissionItem*>(1);
    QVERIFY(waypoint);
    waypoint->setCoordinate(waypoint->coordinate().atDistanceAndAzimuth(30, 45));
    QVERIFY(master->dirtyForSave());
    QList<QGeoCoordinate> keptCoordinates;
    for (int i = 0; i < mission->visualItems()->count(); ++i) {
        keptCoordinates.append(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate());
    }
    QList<QPointer<QQuickItem>> keptObjects;
    for (const auto& name : {QStringLiteral("planMissionRoute"), QStringLiteral("planWaypoint"),
                             QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"),
                             QStringLiteral("planKeepOutOutline"), QStringLiteral("planRallyPoint")}) {
        for (auto* item : planStyleItems(map, name)) {
            keptObjects.append(item);
        }
    }
    QSignalSpy prompted(master, &PlanMasterController::promptForPlanUsageOnVehicleChange);
    QVERIFY(prompted.isValid());
    const auto choosePrompt = [&](bool keep) {
        QTRY_VERIFY_WITH_TIMEOUT(view->property("_promptForPlanUsageShowing").toBool(), TestTimeout::mediumMs());
        const QString text = keep ? (master->offline() ? QStringLiteral("Keep Current Plan")
                                                       : QStringLiteral("Keep Current Plan, Don't Update From Vehicle"))
                                  : QStringLiteral("Discard Unsaved Changes, Load New Plan From Vehicle");
        QQuickItem* button = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((button = planStyleTab(_rootItem, text)), TestTimeout::mediumMs());
        QVERIFY(_clickItemAt(button, 0.5, 0.5, text));
        QTRY_VERIFY_WITH_TIMEOUT(!view->property("_promptForPlanUsageShowing").toBool(), TestTimeout::shortMs());
    };
    manager->setActiveVehicle(b);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), b, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), b, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(prompted.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(master->planVehicleId(), aId);
    choosePrompt(true);
    QVERIFY(!QTest::currentTestFailed());
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    _checkPlan(view, aId, aColor, 4, 3);
    QVERIFY(!QTest::currentTestFailed());
    for (int i = 0; i < keptCoordinates.size(); ++i) {
        QCOMPARE(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate(), keptCoordinates[i]);
    }
    for (const auto& item : keptObjects) {
        QVERIFY(item);
        QVERIFY(planStyleItems(map, item->objectName()).contains(item.data()));
    }

    // Reopen the real decision by switching back and then to B; Keep itself deliberately performs no load.
    manager->setActiveVehicle(a);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), a, TestTimeout::longMs());
    choosePrompt(true);
    QVERIFY(!QTest::currentTestFailed());
    manager->setActiveVehicle(b);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), b, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), b, TestTimeout::longMs());
    choosePrompt(false);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_COMPARE_WITH_TIMEOUT(master->planVehicleId(), bId, TestTimeout::longMs());
    _checkPlan(view, bId, bColor, 6, 5);
    QVERIFY(!QTest::currentTestFailed());
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(fence->polygons()->value<QGCFencePolygon*>(i)->path(), b->geoFenceManager()->polygons()[i].path());
        QCOMPARE(fence->circles()->value<QGCFenceCircle*>(i)->center(), b->geoFenceManager()->circles()[i].center());
    }
    QCOMPARE(rally->points()->value<RallyPoint*>(0)->coordinate(), b->rallyPointManager()->points()[0].coordinate);
    QVERIFY(mission->visualItems()->value<VisualMissionItem*>(1)->coordinate().distanceTo(keptCoordinates[1]) > 10);

    manager->setActiveVehicle(a);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->planVehicleId(), aId, TestTimeout::longMs());
    _checkPlan(view, aId, aColor, 4, 3);
    QVERIFY(!QTest::currentTestFailed());
    waypoint = mission->visualItems()->value<VisualMissionItem*>(1);
    waypoint->setCoordinate(waypoint->coordinate().atDistanceAndAzimuth(40, 45));
    const auto dirtyCoordinate = waypoint->coordinate();
    const auto offlineHome = mission->plannedHomePosition();
    QVERIFY(master->dirtyForSave());
    second->disconnect();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 1, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    first->disconnect();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 0, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(manager->activeVehicle() == nullptr && master->offline(), TestTimeout::longMs());
    choosePrompt(true);
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(master->planVehicleId(), aId);
    QCOMPARE(waypoint->coordinate(), dirtyCoordinate);
    _checkPlan(view, aId, aColor, 4, 3);
    QVERIFY(!QTest::currentTestFailed());

    QList<QPointer<QQuickItem>> removed;
    for (const auto& name : {QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline"),
                             QStringLiteral("planRallyPoint")}) {
        for (auto* item : planStyleItems(map, name)) {
            if (item->property("boundaryKind").toString() != QStringLiteral("parameterCircle")) {
                removed.append(item);
            }
        }
    }
    master->removeAll();
    QCOMPARE(master->planVehicleId(), 0U);
    for (const auto& item : removed) {
        QTRY_VERIFY_WITH_TIMEOUT(item.isNull(), TestTimeout::shortMs());
    }
    mission->setHomePosition(offlineHome);
    for (int i = 1; i <= 2; ++i) {
        QVERIFY(mission->insertSimpleMissionItem(offlineHome.atDistanceAndAzimuth(200 * i, 90), i, true));
    }
    const auto topLeft = offlineHome.atDistanceAndAzimuth(300, 315);
    const auto bottomRight = offlineHome.atDistanceAndAzimuth(300, 135);
    for (bool inclusion : {true, false}) {
        fence->addInclusionPolygon(topLeft, bottomRight);
        fence->polygons()->value<QGCFencePolygon*>(fence->polygons()->count() - 1)->setInclusion(inclusion);
        fence->addInclusionCircle(topLeft, bottomRight);
        auto* circle = fence->circles()->value<QGCFenceCircle*>(fence->circles()->count() - 1);
        circle->setInclusion(inclusion);
        circle->setCenter(offlineHome.atDistanceAndAzimuth(150, inclusion ? 180 : 270));
    }
    rally->addPoint(offlineHome.atDistanceAndAzimuth(200, 315));
    rally->addPoint(offlineHome.atDistanceAndAzimuth(300, 315));
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    const QColor neutral(QStringLiteral("#ffa000"));
    _checkPlan(view, 0, neutral, 3, 2);
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(context.data(), map->findChild<QObject*>(QStringLiteral("planViewPlanStyleContext")));
    QVERIFY(context->setProperty("supported", false));
    QTRY_VERIFY_WITH_TIMEOUT(planStyleItems(map, QStringLiteral("planKeepInOutline")).isEmpty() &&
                                 planStyleItems(map, QStringLiteral("planKeepOutOutline")).isEmpty(),
                             TestTimeout::shortMs());
    for (auto* route : planStyleItems(map, QStringLiteral("planMissionRoute"))) {
        QCOMPARE(planStyleColor(route, QStringLiteral("line.color")), neutral);
        QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 3.0);
    }
    for (auto* fill : planStyleItems(map, QStringLiteral("planFenceFill"))) {
        const bool inclusion = fill->property("inclusion").toBool();
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), inclusion ? 2.0 : 0.0);
        QCOMPARE(fill->opacity(), inclusion ? 0.5 : 0.1);
    }
    for (auto* marker : planStyleItems(map, QStringLiteral("planWaypoint"))) {
        QCOMPARE(marker->property("renderedBorderWidth").toReal(), 0.0);
        QCOMPARE(marker->property("renderedTextColor").value<QColor>(), QColor(QStringLiteral("white")));
        if (marker->property("checked").toBool()) {
            QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), QColor(QStringLiteral("green")));
        }
    }
    for (auto* marker : planStyleItems(map, QStringLiteral("planRallyPoint"))) {
        QTRY_COMPARE_WITH_TIMEOUT(marker->property("markerShape").toString(), QStringLiteral("circle"),
                                  TestTimeout::shortMs());
        QCOMPARE(marker->property("markerBorderWidth").toReal(), 0.0);
    }
    for (auto* accent : planStyleItems(map, QStringLiteral("planHomeAccent"))) {
        QVERIFY(!accent->isVisible());
    }
    QVERIFY(context->setProperty("supported", true));
    _checkPlan(view, 0, neutral, 3, 2);
    QVERIFY(!QTest::currentTestFailed());
}

UT_REGISTER_TEST(PlanStyleUITest, TestLabel::Integration, TestLabel::Vehicle)
