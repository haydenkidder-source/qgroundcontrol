#include "ExclusionZoneControllerTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "ExclusionZoneController.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MockConfiguration.h"
#include "MockLink.h"
#include "PlanMasterController.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "StagedExclusionZone.h"
#include "Vehicle.h"
#include "shapefil.h"

namespace {

QString writeExclusionKml(const QString& path, bool hole)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    const QByteArray first =
        "<Polygon><outerBoundaryIs><LinearRing><coordinates>"
        "0,0 0.00001,0 1,0 1,1 0,0"
        "</coordinates></LinearRing></outerBoundaryIs></Polygon>";
    const QByteArray second =
        "<Polygon><outerBoundaryIs><LinearRing><coordinates>"
        "2,2 3,2 3,3 2,2"
        "</coordinates></LinearRing></outerBoundaryIs>" +
        (hole ? QByteArray("<innerBoundaryIs/>") : QByteArray()) + "</Polygon>";
    const QByteArray content =
        "<kml><Document><Placemark><MultiGeometry>" + first + second + "</MultiGeometry></Placemark></Document></kml>";
    return file.write(content) == content.size() ? path : QString();
}

QString writeExclusionShp(const QString& path, bool multipart)
{
    SHPHandle handle = SHPCreate(path.toUtf8().constData(), SHPT_POLYGON);
    if (!handle) {
        return {};
    }
    const double x[] = {0, 1, 1, 0, 2, 3, 3, 2};
    const double y[] = {0, 0, 1, 0, 2, 2, 3, 2};
    const int parts[] = {0, 4};
    SHPObject* object =
        SHPCreateObject(SHPT_POLYGON, -1, multipart ? 2 : 1, parts, nullptr, multipart ? 8 : 4, x, y, nullptr, nullptr);
    const bool written = object && SHPWriteObject(handle, -1, object) >= 0;
    if (object) {
        SHPDestroyObject(object);
    }
    SHPClose(handle);
    QFile prj(path.left(path.size() - 4) + QStringLiteral(".prj"));
    const QByteArray projection("GEOGCS[\"GCS_WGS_1984\",");
    return written && prj.open(QIODevice::WriteOnly) && prj.write(projection) == projection.size() ? path : QString();
}

}  // namespace

void ExclusionZoneControllerTest::_importKml()
{
    ExclusionZoneController controller;
    const QString good = writeExclusionKml(_directory.filePath(QStringLiteral("good.kml")), false);
    const QString hole = writeExclusionKml(_directory.filePath(QStringLiteral("hole.kml")), true);
    QVERIFY(!good.isEmpty() && !hole.isEmpty());
    QSignalSpy errors(&controller, &ExclusionZoneController::importErrorChanged);
    QSignalSpy summaries(&controller, &ExclusionZoneController::importSummaryChanged);
    QVERIFY(!controller.importFromFile(hole));
    QCOMPARE(controller.stagedZones()->count(), 0);
    QVERIFY(controller.importError().contains(QStringLiteral("hole.kml")));
    QVERIFY(controller.importError().contains(QStringLiteral("innerBoundaryIs")));
    QVERIFY(controller.importSummary().isEmpty());
    QVERIFY(controller.importFromFile(good));
    QVERIFY(controller.importError().isEmpty());
    QCOMPARE(controller.importSummary(), QStringLiteral("Imported 2 zone(s) from good.kml"));
    QCOMPARE(controller.stagedZones()->count(), 2);
    const QList<QGeoCoordinate> first = {QGeoCoordinate(0, 0), QGeoCoordinate(0, 0.00001), QGeoCoordinate(0, 1),
                                         QGeoCoordinate(1, 1)};
    const QList<QGeoCoordinate> second = {QGeoCoordinate(2, 2), QGeoCoordinate(2, 3), QGeoCoordinate(3, 3)};
    auto* zone0 = controller.stagedZones()->value<StagedExclusionZone*>(0);
    auto* zone1 = controller.stagedZones()->value<StagedExclusionZone*>(1);
    QVERIFY(zone0 && zone1);
    QCOMPARE(zone0->polygon()->coordinateList(), first);
    QCOMPARE(zone1->polygon()->coordinateList(), second);
    QVERIFY(!zone0->approved() && !zone1->approved());
    QCOMPARE(controller.approvedCount(), 0);
    QVERIFY(!controller.importFromFile(hole));
    QVERIFY(controller.importSummary().isEmpty());
    QVERIFY(controller.importError().contains(QStringLiteral("innerBoundaryIs")));
    QCOMPARE(controller.stagedZones()->count(), 2);
    QCOMPARE(errors.count(), 3);
    QCOMPARE(summaries.count(), 2);
}

void ExclusionZoneControllerTest::_importFailurePreservesZones()
{
    ExclusionZoneController controller;
    const QString good = writeExclusionKml(_directory.filePath(QStringLiteral("preserve.kml")), false);
    const QString hole = writeExclusionKml(_directory.filePath(QStringLiteral("preserve-hole.kml")), true);
    QVERIFY(!good.isEmpty() && !hole.isEmpty());
    QVERIFY(controller.importFromFile(good));
    auto* first = controller.stagedZones()->value<StagedExclusionZone*>(0);
    auto* second = controller.stagedZones()->value<StagedExclusionZone*>(1);
    QVERIFY(first && second);
    controller.setApproved(0, true);
    QCOMPARE(controller.approvedCount(), 1);
    QVERIFY(!controller.importFromFile(hole));
    QCOMPARE(controller.stagedZones()->count(), 2);
    QCOMPARE(controller.stagedZones()->get(0), first);
    QCOMPARE(controller.stagedZones()->get(1), second);
    QVERIFY(first->approved());
    QVERIFY(!second->approved());
    QCOMPARE(controller.approvedCount(), 1);
}

void ExclusionZoneControllerTest::_importShp()
{
    ExclusionZoneController controller;
    const QString multipart = writeExclusionShp(_directory.filePath(QStringLiteral("multipart.shp")), true);
    const QString single = writeExclusionShp(_directory.filePath(QStringLiteral("single.shp")), false);
    QVERIFY(!multipart.isEmpty() && !single.isEmpty());
    QVERIFY(!controller.importFromFile(multipart));
    QVERIFY(controller.importError().contains(QStringLiteral("multipart.shp")));
    QVERIFY(controller.importError().contains(QStringLiteral("multipart polygon entity 1 needs conversion")));
    QCOMPARE(controller.stagedZones()->count(), 0);
    QVERIFY(controller.importFromFile(single));
    QCOMPARE(controller.stagedZones()->count(), 1);
    const QList<QGeoCoordinate> expected = {QGeoCoordinate(0, 0), QGeoCoordinate(0, 1), QGeoCoordinate(1, 1)};
    auto* zone = controller.stagedZones()->value<StagedExclusionZone*>(0);
    QVERIFY(zone);
    QCOMPARE(zone->polygon()->coordinateList(), expected);
}

void ExclusionZoneControllerTest::_importMissingAndUnsupported()
{
    ExclusionZoneController controller;
    for (const QString& name : {QStringLiteral("missing.kml"), QStringLiteral("unsupported.txt")}) {
        QVERIFY(!controller.importFromFile(_directory.filePath(name)));
        QVERIFY(controller.importError().contains(name));
        QVERIFY(controller.importError().size() > name.size());
        QVERIFY(controller.importSummary().isEmpty());
        QCOMPARE(controller.stagedZones()->count(), 0);
    }
}

void ExclusionZoneControllerTest::init()
{
    VehicleTestManualConnect::init();
    QVERIFY(_directory.isValid());
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void ExclusionZoneControllerTest::cleanup()
{
    if (_otherLink) {
        _otherLink->disconnect();
        QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->vehicles()->count(), 1, TestTimeout::longMs());
        _otherLink.clear();
        QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), vehicle(), TestTimeout::mediumMs());
    }
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    VehicleTestManualConnect::cleanup();
}

void ExclusionZoneControllerTest::_seedFence(bool unspecifiedAltitude)
{
    Vehicle* target = vehicle();
    QVERIFY(target);
    GeoFenceManager* fence = target->geoFenceManager();
    QVERIFY(fence);
    QVERIFY(fence->supported());
    QVERIFY(!fence->inProgress());
    QmlObjectListModel polygons;
    QmlObjectListModel circles;
    for (bool inclusion : {true, false}) {
        auto* polygon = new QGCFencePolygon(inclusion, &polygons);
        polygon->appendVertices(QList<QGeoCoordinate>{QGeoCoordinate(47.001, 8.001), QGeoCoordinate(47.002, 8.001),
                                                      QGeoCoordinate(47.002, 8.002), QGeoCoordinate(47.001, 8.002)});
        polygons.append(polygon);
        circles.append(new QGCFenceCircle(QGeoCoordinate(47.004, 8.004), 25.123456789, inclusion, &circles));
    }
    QSignalSpy sent(fence, &GeoFenceManager::sendComplete);
    fence->sendToVehicle(QGeoCoordinate(47.001, 8.001, unspecifiedAltitude ? qQNaN() : 50.123456789), polygons,
                         circles);
    QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, TestTimeout::mediumMs());
    QVERIFY(!sent.first().first().toBool());
    mockLink()->clearReceivedMavlinkMessageCounts();
}

void ExclusionZoneControllerTest::_stageZone(ExclusionZoneController& controller)
{
    const int approvedBefore = controller.approvedCount();
    auto* polygon = new QGCFencePolygon(false);
    // Fractional int32 steps exercise truncation in MISSION_ITEM_INT on both firmwares.
    polygon->appendVertices(QList<QGeoCoordinate>{QGeoCoordinate(47.005000089, 8.005000089),
                                                  QGeoCoordinate(47.006000089, 8.005000089),
                                                  QGeoCoordinate(47.006000089, 8.006000089)});
    auto* zone = new StagedExclusionZone(polygon, &controller);
    controller.stagedZones()->append(zone);
    controller.setApproved(controller.stagedZones()->count() - 1, true);
    QCOMPARE(controller.approvedCount(), approvedBefore + 1);
}

void ExclusionZoneControllerTest::_connectOtherVehicle()
{
    auto* manager = MultiVehicleManager::instance();
    const auto connection = connect(manager, &MultiVehicleManager::vehicleAdded, this, [this](Vehicle* added) {
        if (added) {
            expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle %1").arg(added->id())));
        }
    });
    auto* config = new MockConfiguration(QStringLiteral("Exclusion verification other vehicle"));
    config->setFirmwareType(MAV_AUTOPILOT_PX4);
    config->setVehicleType(MAV_TYPE_QUADROTOR);
    config->setIncrementVehicleId(true);
    config->setDynamic(true);
    SharedLinkConfigurationPtr sharedConfig = LinkManager::instance()->addConfiguration(config);
    QVERIFY(LinkManager::instance()->createConnectedLink(sharedConfig));
    _otherLink = qobject_cast<MockLink*>(sharedConfig->link());
    QVERIFY(_otherLink);
    QTRY_VERIFY_WITH_TIMEOUT(manager->getVehicleById(_otherLink->vehicleId()), TestTimeout::longMs());
    Vehicle* other = manager->getVehicleById(_otherLink->vehicleId());
    QVERIFY(other);
    QTRY_VERIFY_WITH_TIMEOUT(other->isInitialConnectComplete(), TestTimeout::longMs());
    verifyExpectedLogMessage();
    disconnect(connection);
    _otherLink->clearReceivedMavlinkMessageCounts();
    manager->setActiveVehicle(vehicle());
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), vehicle(), TestTimeout::mediumMs());
}

void ExclusionZoneControllerTest::_expectFenceError(int errorCode, const QString& message)
{
    expectLogMessage("Vehicle.Vehicle", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^GeoFence transfer failed - errorCode: %1 errorMsg: \"%2\"$")
                                            .arg(errorCode)
                                            .arg(QRegularExpression::escape(message))));
    expectAppMessage(QRegularExpression(
        QStringLiteral("GeoFence transfer failed\\. Error: %1").arg(QRegularExpression::escape(message))));
}

void ExclusionZoneControllerTest::_exclusiveToken()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    QVERIFY(fence);
    QObject other;
    auto* owner = new QObject;
    QVERIFY(fence->tryAcquireExclusive(owner));
    QVERIFY(fence->exclusiveBusy());
    QVERIFY(fence->tryAcquireExclusive(owner));
    QVERIFY(!fence->tryAcquireExclusive(&other));
    fence->releaseExclusive(&other);
    QVERIFY(fence->exclusiveBusy());
    delete owner;
    QVERIFY(!fence->exclusiveBusy());
    QVERIFY(fence->tryAcquireExclusive(&other));
    fence->releaseExclusive(&other);
    QVERIFY(!fence->exclusiveBusy());
}

void ExclusionZoneControllerTest::_verifiedPushUpdatesPlan_data()
{
    QTest::addColumn<bool>("seeded");
    QTest::newRow("empty") << false;
    QTest::newRow("seeded") << true;
}

void ExclusionZoneControllerTest::_verifiedPushUpdatesPlan()
{
    QFETCH(bool, seeded);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController plan;
    plan.setFlyView(false);
    plan.start();
    if (seeded) {
        _seedFence(false);
        QVERIFY(!QTest::currentTestFailed());
    }
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    QVERIFY(finished.first().first().toBool());
    QCOMPARE(plan.geoFenceController()->polygons()->count(), seeded ? 3 : 1);
    QCOMPARE(plan.geoFenceController()->circles()->count(), seeded ? 2 : 0);
    QCOMPARE(plan.geoFenceController()->breachReturnPoint().isValid(), seeded);
    QVERIFY(plan.geoFenceController()->dirty());
    QVERIFY(plan.dirtyForSave());
    QVERIFY(plan.dirtyForUpload());
    QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("unsaved")));
    QVERIFY(finished.first().at(1).toString().contains(seeded ? QStringLiteral(" 5 shape(s) were added")
                                                              : QStringLiteral(" 1 shape(s) were added")));
    QSignalSpy uploaded(vehicle()->rallyPointManager(), &RallyPointManager::sendComplete);
    plan.sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(uploaded.count(), 1, TestTimeout::longMs());
    QVERIFY(!uploaded.first().first().toBool());
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    QSignalSpy readback(fence, &GeoFenceManager::loadComplete);
    fence->loadFromVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(readback.count(), 1, TestTimeout::longMs());
    QCOMPARE(fence->polygons().count(), seeded ? 3 : 1);
    QCOMPARE(fence->circles().count(), seeded ? 2 : 0);
}

void ExclusionZoneControllerTest::_verifiedPushPlanAlreadyMatches()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController plan;
    plan.setFlyView(false);
    plan.start();
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    const auto* staged = controller.stagedZones()->value<StagedExclusionZone*>(0);
    QVERIFY(staged);
    const QList<QGeoCoordinate> vertices = staged->polygon()->coordinateList();
    auto* polygon = new QGCFencePolygon(staged->polygon()->inclusion(), plan.geoFenceController());
    polygon->appendVertices(vertices);
    plan.geoFenceController()->polygons()->append(polygon);
    const bool fenceDirty = plan.geoFenceController()->dirty();
    const bool dirtyForSave = plan.dirtyForSave();
    const bool dirtyForUpload = plan.dirtyForUpload();
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    QVERIFY(finished.first().first().toBool());
    const QString message = finished.first().at(1).toString();
    QVERIFY(message.contains(QStringLiteral("No shapes needed to be added to the Plan.")));
    QVERIFY(!message.contains(QStringLiteral("shape(s) were added")));
    QCOMPARE(plan.geoFenceController()->polygons()->count(), 1);
    QCOMPARE(plan.geoFenceController()->polygons()->value<QGCFencePolygon*>(0), polygon);
    QVERIFY(!polygon->inclusion());
    QCOMPARE(polygon->coordinateList(), vertices);
    QCOMPARE(plan.geoFenceController()->dirty(), fenceDirty);
    QCOMPARE(plan.dirtyForSave(), dirtyForSave);
    QCOMPARE(plan.dirtyForUpload(), dirtyForUpload);
}

void ExclusionZoneControllerTest::_verifiedPushPreservesPlanFile()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController plan;
    plan.setFlyView(false);
    plan.start();
    auto* polygon = new QGCFencePolygon(true, plan.geoFenceController());
    const QList<QGeoCoordinate> vertices{QGeoCoordinate(47.01, 8.01), QGeoCoordinate(47.02, 8.01),
                                         QGeoCoordinate(47.02, 8.02)};
    polygon->appendVertices(vertices);
    plan.geoFenceController()->polygons()->append(polygon);
    const QString filename = _directory.filePath(QStringLiteral("inclusion.plan"));
    QVERIFY(plan.saveToFile(filename));
    plan.loadFromFile(filename);
    QCOMPARE(plan.currentPlanFile(), filename);
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    QVERIFY(finished.first().first().toBool());
    QCOMPARE(plan.currentPlanFile(), filename);
    QCOMPARE(plan.geoFenceController()->polygons()->count(), 2);
    const auto* inclusion = plan.geoFenceController()->polygons()->value<QGCFencePolygon*>(0);
    QVERIFY(inclusion);
    QVERIFY(inclusion->inclusion());
    QCOMPARE(inclusion->coordinateList(), vertices);
    QVERIFY(plan.dirtyForSave());
    QVERIFY(plan.dirtyForUpload());
    QSignalSpy uploaded(vehicle()->rallyPointManager(), &RallyPointManager::sendComplete);
    plan.sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(uploaded.count(), 1, TestTimeout::longMs());
    QVERIFY(!uploaded.first().first().toBool());
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    QSignalSpy readback(fence, &GeoFenceManager::loadComplete);
    fence->loadFromVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(readback.count(), 1, TestTimeout::longMs());
    QCOMPARE(fence->polygons().count(), 2);
    QVERIFY(fence->polygons().first().inclusion());
    QVERIFY(!fence->polygons().last().inclusion());
}

void ExclusionZoneControllerTest::_failedVerificationPreservesPlan()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController plan;
    plan.setFlyView(false);
    plan.start();
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    int loads = 0;
    connect(fence, &GeoFenceManager::loadComplete, &controller, [&]() {
        if (++loads == 2 && !fence->missionItems().isEmpty()) {
            fence->missionItems().first()->setParam5(46.0);
        }
    });
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    QVERIFY(!finished.first().first().toBool());
    QCOMPARE(plan.geoFenceController()->polygons()->count(), 0);
    QVERIFY(!plan.dirtyForSave());
    QVERIFY(!plan.dirtyForUpload());
}

void ExclusionZoneControllerTest::_pushRejectedWhileExclusive()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    QVERIFY(fence);
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    QObject owner;
    QVERIFY(fence->tryAcquireExclusive(&owner));
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    const int requests = mockLink()->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE);
    QVERIFY(!controller.pushApproved());
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toBool(), false);
    QCOMPARE(finished.first().at(1).toString(),
             QStringLiteral(
                 "Another plan or fence operation is in progress on this vehicle. Try again when it has finished."));
    QCOMPARE(mockLink()->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE), requests);
    fence->releaseExclusive(&owner);
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, TestTimeout::longMs());
    QVERIFY(finished.last().first().toBool());
    QVERIFY(!fence->exclusiveBusy());
}

void ExclusionZoneControllerTest::_testPushRejectedDuringPlanUpload()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    _connectOtherVehicle();
    QVERIFY(_otherLink);
    Vehicle* const target = MultiVehicleManager::instance()->getVehicleById(_otherLink->vehicleId());
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController plan;
    plan.startStaticActiveVehicle(target);
    ExclusionZoneController controller;
    controller.setTargetVehicle(target);
    _stageZone(controller);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QSignalSpy complete(target->rallyPointManager(), &RallyPointManager::sendComplete);
    QList<int> order;
    bool attempted = false;
    connect(target->missionManager(), &PlanManager::inProgressChanged, &controller, [&](bool inProgress) {
        if (!inProgress && !attempted) {
            attempted = true;
            order.append(0);
            const int requests = _otherLink->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE);
            QVERIFY(!controller.pushApproved());
            QCOMPARE(_otherLink->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE), requests);
        }
    });
    connect(target->geoFenceManager(), &GeoFenceManager::sendComplete, &controller, [&](bool error) {
        if (!error) {
            order.append(1);
        }
    });
    connect(target->rallyPointManager(), &RallyPointManager::sendComplete, &controller, [&](bool error) {
        if (!error) {
            order.append(2);
        }
    });
    plan.sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
    QVERIFY(attempted);
    QCOMPARE(order, (QList<int>{0, 1, 2}));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, TestTimeout::longMs());
    QVERIFY(finished.last().first().toBool());
}

void ExclusionZoneControllerTest::_testPlanUploadRejectedDuringPush()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController plan;
    plan.setFlyView(false);
    plan.start();
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QSignalSpy complete(vehicle()->rallyPointManager(), &RallyPointManager::sendComplete);
    QVERIFY(controller.pushApproved());
    const int sends = mockLink()->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT);
    expectAppMessage(QRegularExpression(QStringLiteral(
        "Another plan or fence operation is in progress on this vehicle\\. Try again when it has finished\\.")));
    plan.sendToVehicle();
    verifyExpectedLogMessage();
    QCOMPARE(mockLink()->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT), sends);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    QVERIFY(finished.first().first().toBool());
    plan.sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
    QVERIFY(!complete.first().first().toBool());
}

void ExclusionZoneControllerTest::_initialFenceValidation_data()
{
    QTest::addColumn<QString>("scenario");
    for (const char* scenario : {"trailing", "two-vertices", "too-many", "two-returns", "unsupported", "wrong-frame",
                                 "nan-latitude", "zero-radius", "negative-radius", "empty", "complex"}) {
        QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
}

void ExclusionZoneControllerTest::_initialFenceValidation()
{
    QFETCH(QString, scenario);
    QTemporaryDir testDirectory;
    QVERIFY(testDirectory.isValid());
    SettingsManager::instance()->appSettings()->savePath()->setRawValue(testDirectory.path());
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->initialPlanRequestComplete(), TestTimeout::longMs());
    if (scenario != QStringLiteral("empty")) {
        _seedFence(false);
        QVERIFY(!QTest::currentTestFailed());
    }
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    QVERIFY(fence);
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    const QDir approvals(SettingsManager::instance()->appSettings()->missionSavePath() +
                         QStringLiteral("/ExclusionZoneApprovals"));
    const QStringList beforeFiles = approvals.entryList(QDir::Files);
    const int beforeWrites = mockLink()->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    const bool valid = scenario == QStringLiteral("empty") || scenario == QStringLiteral("complex");
    if (!valid && scenario != QStringLiteral("nan-latitude")) {
        QList<mavlink_mission_item_int_t> raw;
        mavlink_mission_item_int_t item{};
        item.command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION;
        item.frame = MAV_FRAME_GLOBAL;
        item.param1 = scenario == QStringLiteral("two-vertices") ? 2 : 3;
        item.x = 470010000;
        item.y = 80010000;
        const int vertexCount = scenario == QStringLiteral("two-vertices") ? 2 : 3;
        for (int i = 0; i < vertexCount; ++i) {
            item.x += 1000;
            raw.append(item);
        }
        item.command = MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION;
        item.param1 = scenario == QStringLiteral("zero-radius")       ? 0
                      : scenario == QStringLiteral("negative-radius") ? -1
                                                                      : 25;
        raw.append(item);
        item.command = MAV_CMD_NAV_FENCE_RETURN_POINT;
        item.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        item.param1 = 0;
        raw.append(item);
        if (scenario == QStringLiteral("trailing") || scenario == QStringLiteral("too-many")) {
            item.command = MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
            item.frame = MAV_FRAME_GLOBAL;
            item.param1 = scenario == QStringLiteral("trailing") ? 3 : 8;
            raw.append(item);
        } else if (scenario == QStringLiteral("two-returns")) {
            raw.append(item);
        } else if (scenario == QStringLiteral("unsupported")) {
            item.command = MAV_CMD_NAV_WAYPOINT;
            raw.append(item);
            _expectFenceError(GeoFenceManager::UnsupportedCommand,
                              QStringLiteral("GeoFence load: Unsupported command 16"));
        } else if (scenario == QStringLiteral("wrong-frame")) {
            raw[0].frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        }
        mockLink()->setFenceItems(raw);
    }
    bool altered = false;
    const auto alter = connect(fence, &GeoFenceManager::loadComplete, &controller, [&]() {
        if (scenario == QStringLiteral("nan-latitude") && !altered) {
            altered = true;
            const auto& items = fence->missionItems();
            QVERIFY(!items.isEmpty());
            items.first()->setParam5(qQNaN());
        }
    });
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    if (scenario == QStringLiteral("unsupported")) {
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }
    QCOMPARE(finished.first().first().toBool(), valid);
    if (valid) {
        QCOMPARE(fence->missionItems().count(), scenario == QStringLiteral("empty") ? 3 : 14);
    }
    if (!valid) {
        QCOMPARE(finished.first().at(1).toString(),
                 QStringLiteral("The vehicle's fence could not be read completely, so nothing was sent."));
        QCOMPARE(mockLink()->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT), beforeWrites);
        QCOMPARE(approvals.entryList(QDir::Files), beforeFiles);
        QVERIFY(!fence->exclusiveBusy());
        disconnect(alter);
        _seedFence(false);
        QVERIFY(!QTest::currentTestFailed());
        QVERIFY(controller.pushApproved());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, TestTimeout::longMs());
        QVERIFY(finished.last().first().toBool());
        QCOMPARE(fence->missionItems().count(), 14);
    }
    QVERIFY(!fence->exclusiveBusy());
}

void ExclusionZoneControllerTest::_pushVerification_data()
{
    QTest::addColumn<int>("firmware");
    QTest::addColumn<bool>("restart");
    QTest::addColumn<QString>("scenario");
    const auto row = [](const char* name, int firmware, bool restart, const char* scenario) {
        QTest::newRow(name) << firmware << restart << QString::fromLatin1(scenario);
    };
    row("PX4", MAV_AUTOPILOT_PX4, false, "success");
    row("AP", MAV_AUTOPILOT_ARDUPILOTMEGA, false, "success");
    row("unspecified-breach-altitude", MAV_AUTOPILOT_PX4, false, "unspecified-altitude");
    row("AP-restart", MAV_AUTOPILOT_ARDUPILOTMEGA, true, "success");
    row("PX4-missing-stored-fence", MAV_AUTOPILOT_PX4, false, "missing");
    row("AP-restart-missing-stored-fence", MAV_AUTOPILOT_ARDUPILOTMEGA, true, "missing");
    row("read-error", MAV_AUTOPILOT_PX4, false, "read-error");
    row("AP-restart-read-error", MAV_AUTOPILOT_ARDUPILOTMEGA, true, "read-error");
    row("read-timeout", MAV_AUTOPILOT_PX4, false, "read-timeout");
    row("disconnect", MAV_AUTOPILOT_PX4, false, "disconnect");
    row("active-switch", MAV_AUTOPILOT_PX4, false, "active-switch");
    row("target-change", MAV_AUTOPILOT_PX4, false, "target-change");
    row("busy-before-readback", MAV_AUTOPILOT_PX4, false, "busy");
    row("second-push", MAV_AUTOPILOT_PX4, false, "second-push");
    row("superseded", MAV_AUTOPILOT_PX4, false, "superseded");
    row("reordered-equivalent", MAV_AUTOPILOT_PX4, false, "equivalent");
    row("AP-reordered-equivalent", MAV_AUTOPILOT_ARDUPILOTMEGA, false, "equivalent");
    row("staged-polygon-snapshot", MAV_AUTOPILOT_PX4, false, "snapshot");
    // Alter decoded readback before the controller's loadComplete handler to isolate comparison rules.
    for (const char* change : {"polygon-inclusion", "vertex-count",      "vertex-cycle",     "coordinate",
                               "circle-inclusion",  "circle-center",     "circle-radius",    "breach-value",
                               "breach-altitude",   "breach-nan",        "vertex-count-nan", "vertex-count-fractional",
                               "polygon-frame",     "circle-radius-nan", "coordinate-nan",   "breach-frame",
                               "breach-infinity",   "duplicate-return",  "extra-circle",     "missing-return",
                               "incomplete-group"}) {
        row(change, MAV_AUTOPILOT_PX4, false, change);
    }
}

void ExclusionZoneControllerTest::_pushVerification()
{
    QFETCH(int, firmware);
    QFETCH(bool, restart);
    QFETCH(QString, scenario);
    if (firmware == MAV_AUTOPILOT_ARDUPILOTMEGA && !apmFirmwareSupported()) {
        QSKIP("ArduPilot support not registered in this build");
    }
    _connectMockLink(static_cast<MAV_AUTOPILOT>(firmware));
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    _seedFence(scenario == QStringLiteral("unspecified-altitude"));
    QVERIFY(!QTest::currentTestFailed());
    if (scenario == QStringLiteral("active-switch") || scenario == QStringLiteral("target-change")) {
        _connectOtherVehicle();
        QVERIFY(!QTest::currentTestFailed());
    }
    Vehicle* const original = vehicle();
    QVERIFY(original);
    GeoFenceManager* const fence = original->geoFenceManager();
    QVERIFY(fence);
    QPointer<GeoFenceManager> guardedFence(fence);
    ExclusionZoneController controller;
    controller.setTargetVehicle(original);
    _stageZone(controller);
    auto* const staged = controller.stagedZones()->get(0);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QSignalSpy approved(&controller, &ExclusionZoneController::approvedCountChanged);
    QSignalSpy loads(fence, &GeoFenceManager::loadComplete);
    QSignalSpy sends(fence, &GeoFenceManager::sendComplete);
    bool uploadComplete = false;
    bool verificationStarted = false;
    bool readbackSeen = false;
    int rejectedPushes = 0;

    // Registered before pushApproved's handlers: every checkpoint must still retain the staged zone.
    const auto loadCheckpoint = connect(fence, &GeoFenceManager::loadComplete, &controller, [&]() {
        QCOMPARE(controller.stagedZones()->count(), 1);
        QCOMPARE(controller.stagedZones()->get(0), staged);
        QCOMPARE(controller.approvedCount(), 1);
        QCOMPARE(approved.count(), 0);
        if (!uploadComplete) {
            if (scenario == QStringLiteral("snapshot")) {
                auto* zone = controller.stagedZones()->value<StagedExclusionZone*>(0);
                zone->polygon()->adjustVertex(0, QGeoCoordinate(47.1, 8.1));
            }
            if (restart) {
                mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailWriteFinalAckFirstResponse,
                                                      MAV_MISSION_ERROR);
            }
            return;
        }
        readbackSeen = true;
        const auto& items = fence->missionItems();
        if (scenario == QStringLiteral("missing") || scenario == QStringLiteral("read-error") ||
            scenario == QStringLiteral("read-timeout")) {
            QVERIFY(items.isEmpty());
            return;
        }
        QCOMPARE(items.count(), 14);
        if (scenario == QStringLiteral("snapshot")) {
            QVERIFY(qAbs(items[8]->param5() - 47.005000089) <= 1.1e-7);
            QVERIFY(qAbs(items[8]->param6() - 8.005000089) <= 1.1e-7);
        }
        if (scenario == QStringLiteral("polygon-inclusion")) {
            items[0]->setCommand(MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION);
        } else if (scenario == QStringLiteral("vertex-count")) {
            items[0]->setParam1(5);
        } else if (scenario == QStringLiteral("vertex-cycle")) {
            // Swapping adjacent vertices of a quadrilateral changes its cycle (creates a crossing).
            const QGeoCoordinate vertex = items[1]->coordinate();
            items[1]->setParam5(items[2]->param5());
            items[1]->setParam6(items[2]->param6());
            items[2]->setParam5(vertex.latitude());
            items[2]->setParam6(vertex.longitude());
        } else if (scenario == QStringLiteral("equivalent")) {
            QList<MissionItem*> snapshot;
            for (const MissionItem* item : items) {
                snapshot.append(new MissionItem(*item, &controller));
            }
            // Return first, interleaved shapes, rotated inclusion and reversed exclusion cycles.
            const QList<int> order{13, 12, 9, 8, 10, 2, 3, 0, 1, 11, 7, 6, 5, 4};
            for (int i = 0; i < order.count(); ++i) {
                *items[i] = *snapshot[order[i]];
            }
            qDeleteAll(snapshot);
        } else if (scenario == QStringLiteral("coordinate")) {
            items[0]->setParam6(items[0]->param6() + 0.000001);
        } else if (scenario == QStringLiteral("circle-inclusion")) {
            items[11]->setCommand(MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION);
        } else if (scenario == QStringLiteral("circle-center")) {
            items[11]->setParam5(items[11]->param5() + 0.000001);
        } else if (scenario == QStringLiteral("circle-radius")) {
            items[11]->setParam1(items[11]->param1() + 1);
        } else if (scenario == QStringLiteral("breach-value")) {
            items[13]->setParam5(items[13]->param5() + 0.000001);
        } else if (scenario == QStringLiteral("breach-altitude")) {
            items[13]->setParam7(items[13]->param7() + 1);
        } else if (scenario == QStringLiteral("breach-nan")) {
            items[13]->setParam7(qQNaN());
        } else if (scenario == QStringLiteral("vertex-count-nan")) {
            items[0]->setParam1(qQNaN());
        } else if (scenario == QStringLiteral("vertex-count-fractional")) {
            items[0]->setParam1(4.5);
        } else if (scenario == QStringLiteral("polygon-frame")) {
            items[0]->setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
        } else if (scenario == QStringLiteral("circle-radius-nan")) {
            items[11]->setParam1(qQNaN());
        } else if (scenario == QStringLiteral("coordinate-nan")) {
            items[0]->setParam5(qQNaN());
        } else if (scenario == QStringLiteral("breach-frame")) {
            items[13]->setFrame(MAV_FRAME_GLOBAL);
        } else if (scenario == QStringLiteral("breach-infinity")) {
            items[13]->setParam7(qInf());
        } else if (scenario == QStringLiteral("duplicate-return")) {
            *items[12] = *items[13];
        } else if (scenario == QStringLiteral("extra-circle")) {
            *items[13] = *items[11];
        } else if (scenario == QStringLiteral("missing-return")) {
            // Remove the return through the real owning list; GeoFenceManager's cache stays populated.
            auto& mutableItems = const_cast<QList<MissionItem*>&>(items);
            delete mutableItems.takeLast();
        } else if (scenario == QStringLiteral("incomplete-group")) {
            items[2]->setCommand(MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION);
        }
    });
    const auto sendCheckpoint = connect(fence, &GeoFenceManager::sendComplete, &controller, [&](bool error) {
        if (uploadComplete) {
            return;
        }
        QVERIFY(!error);
        QCOMPARE(controller.stagedZones()->count(), 1);
        QCOMPARE(approved.count(), 0);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(fence->uploadRequiresReadback(), restart);
        uploadComplete = true;
        mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ERROR);
        if (scenario == QStringLiteral("missing")) {
            mockLink()->resetMissionItemHandler();
        } else if (scenario == QStringLiteral("read-error")) {
            _expectFenceError(PlanManager::VehicleAckError, QStringLiteral("Unspecified error."));
            mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailReadRequest0ErrorAck,
                                                  MAV_MISSION_ERROR);
        } else if (scenario == QStringLiteral("read-timeout")) {
            _expectFenceError(PlanManager::MaxRetryExceeded,
                              QStringLiteral("Mission request list failed, maximum retries exceeded."));
            mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailReadRequestListNoResponse,
                                                  MAV_MISSION_ERROR);
        } else if (scenario == QStringLiteral("busy")) {
            fence->loadFromVehicle();
        }
    });
    const auto progressCheckpoint =
        connect(fence, &GeoFenceManager::inProgressChanged, &controller, [&](bool inProgress) {
            if (!inProgress || !uploadComplete || verificationStarted) {
                return;
            }
            verificationStarted = true;
            QCOMPARE(controller.stagedZones()->count(), 1);
            QCOMPARE(finished.count(), 0);
            if (scenario == QStringLiteral("active-switch")) {
                auto* manager = MultiVehicleManager::instance();
                manager->setActiveVehicle(manager->getVehicleById(_otherLink->vehicleId()));
            } else if (scenario == QStringLiteral("target-change")) {
                controller.setTargetVehicle(MultiVehicleManager::instance()->getVehicleById(_otherLink->vehicleId()));
            } else if (scenario == QStringLiteral("disconnect")) {
                // Disconnect after loadFromVehicle has returned, at the start of actual verification.
                QTimer::singleShot(0, &controller, [this]() { _disconnectMockLink(); });
            } else if (scenario == QStringLiteral("second-push")) {
                QVERIFY(!controller.pushApproved());
                ++rejectedPushes;
                QCOMPARE(finished.count(), 1);
                QVERIFY(!finished.first().first().toBool());
            } else if (scenario == QStringLiteral("superseded")) {
                emit fence->sendComplete(false);
            }
        });

    QVERIFY(controller.pushApproved());
    QVERIFY(fence->exclusiveBusy());
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > rejectedPushes, TestTimeout::longMs());
    if (guardedFence) {
        QVERIFY(!guardedFence->exclusiveBusy());
    }
    if (scenario == QStringLiteral("read-error") || scenario == QStringLiteral("read-timeout")) {
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }
    const bool success = scenario == QStringLiteral("success") || scenario == QStringLiteral("active-switch") ||
                         scenario == QStringLiteral("second-push") ||
                         scenario == QStringLiteral("unspecified-altitude") ||
                         scenario == QStringLiteral("equivalent") || scenario == QStringLiteral("snapshot");
    QCOMPARE(finished.count(), 1 + rejectedPushes);
    QCOMPARE(finished.last().at(0).toBool(), success);
    QVERIFY(!finished.last().at(1).toString().isEmpty());
    QVERIFY(uploadComplete);
    QVERIFY(verificationStarted);
    QCOMPARE(approved.count(), int(success));
    QCOMPARE(controller.stagedZones()->count(), success ? 0 : 1);
    QCOMPARE(controller.approvedCount(), success ? 0 : 1);
    if (success) {
        QVERIFY(readbackSeen);
        QCOMPARE(loads.count(), 2);
        QCOMPARE(sends.count(), 1);
        QCOMPARE(mockLink()->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE), 2);
        QCOMPARE(fence->polygons().count(), 3);
        QCOMPARE(fence->circles().count(), 2);
    } else {
        QCOMPARE(controller.stagedZones()->get(0), staged);
        const QString message = finished.last().at(1).toString();
        QVERIFY(message.contains(QStringLiteral("Plan view")));
        if (scenario == QStringLiteral("missing") || scenario.startsWith(QStringLiteral("polygon-")) ||
            scenario.startsWith(QStringLiteral("vertex-")) || scenario == QStringLiteral("coordinate") ||
            scenario.startsWith(QStringLiteral("circle-")) || scenario.startsWith(QStringLiteral("breach-")) ||
            scenario == QStringLiteral("coordinate-nan") || scenario == QStringLiteral("duplicate-return") ||
            scenario == QStringLiteral("extra-circle") || scenario == QStringLiteral("missing-return") ||
            scenario == QStringLiteral("incomplete-group")) {
            QVERIFY(readbackSeen);
            QVERIFY(message.contains(QStringLiteral("does not match")));
        }
        if (restart) {
            QVERIFY(finished.last().at(1).toString().contains(QStringLiteral("full restart")));
        }
    }
    if (_otherLink) {
        auto* manager = MultiVehicleManager::instance();
        Vehicle* other = manager->getVehicleById(_otherLink->vehicleId());
        QVERIFY(other);
        if (success) {
            QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), other, TestTimeout::mediumMs());
            QCOMPARE(controller.targetVehicle(), original);
        } else {
            QCOMPARE(controller.targetVehicle(), other);
        }
        QCOMPARE(_otherLink->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE), 0);
        QCOMPARE(_otherLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT), 0);
    }
    if (mockLink()) {
        QTRY_VERIFY_WITH_TIMEOUT(!fence->inProgress(), TestTimeout::mediumMs());
        // PlanManager can report both error and completion. Stale terminal signals cannot finish twice.
        disconnect(loadCheckpoint);
        disconnect(sendCheckpoint);
        disconnect(progressCheckpoint);
        _expectFenceError(GeoFenceManager::InternalError, QStringLiteral("late error"));
        emit fence->error(GeoFenceManager::InternalError, QStringLiteral("late error"));
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
        emit fence->loadComplete();
        emit fence->sendComplete(true);
        QCOMPARE(finished.count(), 1 + rejectedPushes);
        if (scenario == QStringLiteral("read-error")) {
            mockLink()->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ERROR);
            QVERIFY(controller.pushApproved());
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, TestTimeout::mediumMs());
            QVERIFY(finished.last().first().toBool());
            QCOMPARE(controller.stagedZones()->count(), 0);
        }
    }
}

void ExclusionZoneControllerTest::_stagedZoneLifetime_data()
{
    QTest::addColumn<bool>("duringLoading");
    QTest::addColumn<bool>("removeAll");
    QTest::addColumn<bool>("mismatch");
    QTest::newRow("cleared-during-loading") << true << true << false;
    QTest::newRow("one-deleted-during-loading") << true << false << false;
    QTest::newRow("cleared-during-verification") << false << true << false;
    QTest::newRow("one-deleted-during-verification") << false << false << false;
    QTest::newRow("cleared-during-verification-mismatch") << false << true << true;
    QTest::newRow("one-deleted-during-verification-mismatch") << false << false << true;
}

void ExclusionZoneControllerTest::_stagedZoneLifetime()
{
    QFETCH(bool, duringLoading);
    QFETCH(bool, removeAll);
    QFETCH(bool, mismatch);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(vehicle());
    QVERIFY(mockLink());
    _seedFence(false);
    QVERIFY(!QTest::currentTestFailed());
    GeoFenceManager* const fence = vehicle()->geoFenceManager();
    QVERIFY(fence);
    ExclusionZoneController controller;
    controller.setTargetVehicle(vehicle());
    _stageZone(controller);
    _stageZone(controller);
    QVERIFY(!QTest::currentTestFailed());
    QPointer<StagedExclusionZone> first = controller.stagedZones()->value<StagedExclusionZone*>(0);
    QPointer<StagedExclusionZone> second = controller.stagedZones()->value<StagedExclusionZone*>(1);
    QSignalSpy finished(&controller, &ExclusionZoneController::pushFinished);
    QSignalSpy approved(&controller, &ExclusionZoneController::approvedCountChanged);
    QSignalSpy sends(fence, &GeoFenceManager::sendComplete);
    QSignalSpy loads(fence, &GeoFenceManager::loadComplete);
    bool deleted = false;
    connect(fence, &GeoFenceManager::inProgressChanged, &controller, [&](bool inProgress) {
        if (!inProgress || deleted || (!duringLoading && sends.isEmpty())) {
            return;
        }
        deleted = true;
        if (removeAll) {
            controller.stagedZones()->clear();
            delete first.data();
            delete second.data();
        } else {
            delete controller.stagedZones()->removeAt(0);
        }
        QVERIFY(!first);
        QCOMPARE(second.isNull(), removeAll);
        QCOMPARE(finished.count(), 0);
    });
    connect(fence, &GeoFenceManager::loadComplete, &controller, [&]() {
        if (mismatch && !sends.isEmpty()) {
            const auto& items = fence->missionItems();
            QVERIFY(!items.isEmpty());
            items.first()->setParam5(items.first()->param5() + 0.001);
        }
    });
    QVERIFY(controller.pushApproved());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, TestTimeout::longMs());
    QVERIFY(deleted);
    const bool success = !duringLoading && !removeAll && !mismatch;
    QCOMPARE(finished.first().first().toBool(), success);
    const QString message = finished.first().at(1).toString();
    QVERIFY(!message.isEmpty());
    QCOMPARE(sends.count(), duringLoading ? 0 : 1);
    QCOMPARE(loads.count(), duringLoading ? 1 : 2);
    QCOMPARE(mockLink()->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE), duringLoading ? 1 : 2);
    QCOMPARE(approved.count(), success ? 1 : 0);
    const int remaining = !removeAll && (duringLoading || mismatch) ? 1 : 0;
    QCOMPARE(controller.stagedZones()->count(), remaining);
    QCOMPARE(controller.approvedCount(), remaining);
    if (duringLoading) {
        QVERIFY(message.contains(QStringLiteral("staged zones changed")));
        QVERIFY(message.contains(QStringLiteral("No zones were sent")));
        QCOMPARE(mockLink()->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT), 0);
    } else if (mismatch) {
        QVERIFY(message.contains(QStringLiteral("does not match")));
        QVERIFY(message.contains(QStringLiteral("staged zones were removed")));
    } else {
        QVERIFY(message.contains(QStringLiteral("vehicle fence was verified")));
        QVERIFY(message.contains(QStringLiteral("%1 surviving staged zones").arg(success ? 1 : 0)));
        QVERIFY(message.contains(QStringLiteral("%1 approved staged zones were removed").arg(removeAll ? 2 : 1)));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!fence->inProgress(), TestTimeout::mediumMs());
    QCOMPARE(finished.count(), 1);
}

UT_REGISTER_TEST(ExclusionZoneControllerTest, TestLabel::Integration, TestLabel::Vehicle)
