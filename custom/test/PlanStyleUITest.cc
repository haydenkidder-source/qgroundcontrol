#include "PlanStyleUITest.h"

#include <memory>

#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "FirmwarePlugin.h"
#include "FixedWingLandingComplexItem.h"
#include "FlightPathSegment.h"
#include "LinkManager.h"
#include "MissionController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MissionSettingsItem.h"
#include "MultiVehicleManager.h"
#include "PlanMasterController.h"
#include "QGCCorePlugin.h"
#include "QGCPlanStyle.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "TakeoffMissionItem.h"
#include "VTOLLandingComplexItem.h"
#include "VehicleRoleController.h"

namespace {
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
    _links.clear();
    _configs.clear();
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    QmlUITestBase::cleanup();
    QVERIFY(removed);
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
        QCOMPARE(accents.first()->property("color").value<QColor>(), QColor(Qt::transparent));
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
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(collided(), QStringLiteral("line.color")), QColor(Qt::red),
                                  TestTimeout::shortMs());
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
        QCOMPARE(glideSlope->property("color").value<QColor>(), QColor(255, 165, 0));
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
            QCOMPARE(ring->property("color").value<QColor>(), QColor(Qt::transparent));
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
    QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(loiterCircle(), QStringLiteral("border.color")), QColor(Qt::red),
                              TestTimeout::shortMs());
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
    QTRY_COMPARE_WITH_TIMEOUT(glideSlope->property("color").value<QColor>(), QColor(Qt::red), TestTimeout::shortMs());
    QCOMPARE(landingArea->property("color").value<QColor>(), QColor(QStringLiteral("green")));
    approach->setCoord1AMSLAlt(2.0e6);
    approach->setCoord2AMSLAlt(2.0e6);
    QVERIFY(!approach->terrainCollision());
    QVERIFY(!fixedWing.terrainCollision());
    QTRY_COMPARE_WITH_TIMEOUT(glideSlope->property("color").value<QColor>(), QColor(255, 165, 0),
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

UT_REGISTER_TEST(PlanStyleUITest, TestLabel::Integration, TestLabel::Vehicle)
