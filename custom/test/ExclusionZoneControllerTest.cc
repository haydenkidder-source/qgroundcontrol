#include "ExclusionZoneControllerTest.h"

#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "ExclusionZoneController.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "MissionItem.h"
#include "MockConfiguration.h"
#include "MockLink.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "StagedExclusionZone.h"
#include "Vehicle.h"

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
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > rejectedPushes, TestTimeout::longMs());
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
    QCOMPARE(finished.last().at(1).toString().isEmpty(), success);
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
