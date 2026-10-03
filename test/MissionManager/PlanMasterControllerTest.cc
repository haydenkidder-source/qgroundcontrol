#include "PlanMasterControllerTest.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "MissionManager.h"
#include "MultiSignalSpy.h"
#include "MultiVehicleManager.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "SurveyPlanCreator.h"
#include "TakeoffMissionItem.h"
#include "Vehicle.h"

void PlanMasterControllerTest::init()
{
    UnitTest::init();
    MultiVehicleManager::instance()->init();
    _masterController = new PlanMasterController(this);
    _masterController->setFlyView(false);
    _masterController->start();
}

void PlanMasterControllerTest::cleanup()
{
    delete _masterController;
    _masterController = nullptr;
    // Extra vehicles go first: otherwise MultiVehicleManager promotes one to active while the primary MockLink is
    // disconnected, and VehicleTest would track (and later dereference) a vehicle it does not own.
    if (!_extraLinks.isEmpty()) {
        for (const QPointer<MockLink>& link : std::as_const(_extraLinks)) {
            if (link) {
                link->disconnect();
            }
        }
        _extraLinks.clear();
        const int remainingVehicles = _mockLink ? 1 : 0;
        QVERIFY(UnitTest::waitForCondition(
            [remainingVehicles] { return MultiVehicleManager::instance()->vehicles()->count() == remainingVehicles; },
            TestTimeout::longMs(), QStringLiteral("extra vehicles removed")));
    }
    _disconnectMockLink();
    UnitTest::cleanup();
}

void PlanMasterControllerTest::_testMissionPlannerFileLoad()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 6);
}

void PlanMasterControllerTest::_testTakeoffTextFileLoad_data()
{
    QTest::addColumn<int>("firmwareClass");
    QTest::addColumn<int>("vehicleClass");
    QTest::addColumn<int>("takeoffCommand");

    QTest::newRow("ArduPilot takeoff")
        << int(QGCMAVLink::FirmwareClassArduPilot) << int(QGCMAVLink::VehicleClassMultiRotor) << int(MAV_CMD_NAV_TAKEOFF);
    QTest::newRow("PX4 VTOL multicopter takeoff")
        << int(QGCMAVLink::FirmwareClassPX4) << int(QGCMAVLink::VehicleClassVTOL) << int(MAV_CMD_NAV_TAKEOFF);
    QTest::newRow("PX4 VTOL takeoff")
        << int(QGCMAVLink::FirmwareClassPX4) << int(QGCMAVLink::VehicleClassVTOL) << int(MAV_CMD_NAV_VTOL_TAKEOFF);
}

void PlanMasterControllerTest::_testTakeoffTextFileLoad()
{
    QFETCH(int, firmwareClass);
    QFETCH(int, vehicleClass);
    QFETCH(int, takeoffCommand);

    // Plain-text mission file with home position, takeoff and one waypoint (#13167)
    const QByteArray takeoffMission = QByteArray(
        "QGC WPL 110\r\n"
        "0\t1\t0\t16\t0\t0\t0\t0\t34.577822\t-112.469101\t584.380005\t1\r\n"
        "1\t0\t3\t") + QByteArray::number(takeoffCommand) + QByteArray(
        "\t20.000000\t0.000000\t0.000000\t0.000000\t0.000000\t0.000000\t30.000000\t1\r\n"
        "2\t0\t3\t16\t0.000000\t0.000000\t0.000000\t0.000000\t34.469587\t-112.534801\t90.000000\t1\r\n");

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filename = tempDir.filePath(QStringLiteral("TakeoffMission.waypoints"));
    QFile file(filename);
    // No QIODevice::Text: write the CRLF line endings verbatim on all platforms to match
    // the original repro file from the issue.
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(takeoffMission) != -1);
    file.close();

    SettingsManager::instance()->appSettings()->offlineEditingFirmwareClass()->setRawValue(firmwareClass);
    SettingsManager::instance()->appSettings()->offlineEditingVehicleClass()->setRawValue(vehicleClass);

    _masterController->loadFromFile(filename);

    QmlObjectListModel* visualItems = _masterController->missionController()->visualItems();
    QCOMPARE(visualItems->count(), 3); // Mission settings, takeoff, waypoint

    // The original bug caused the takeoff item to consume the following waypoint line,
    // resulting in a single takeoff item carrying the waypoint's values.
    TakeoffMissionItem* takeoffItem = visualItems->value<TakeoffMissionItem*>(1);
    QVERIFY(takeoffItem);
    QCOMPARE(static_cast<MAV_CMD>(takeoffItem->command()), static_cast<MAV_CMD>(takeoffCommand));
    QCOMPARE(takeoffItem->missionItem().param7(), 30.0);

    SimpleMissionItem* waypointItem = visualItems->value<SimpleMissionItem*>(2);
    QVERIFY(waypointItem);
    QVERIFY(!waypointItem->isTakeoffItem());
    QCOMPARE(static_cast<MAV_CMD>(waypointItem->command()), MAV_CMD_NAV_WAYPOINT);
    QCOMPARE(waypointItem->missionItem().param7(), 90.0);
}

void PlanMasterControllerTest::_testActiveVehicleChanged()
{
    // The test emits missionManager->error() twice to verify signal propagation.
    // Each emission triggers a showAppMessage debug log via PlanMasterController.
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("Mission transfer failed"));
    // There was a defect where the PlanMasterController would, upon a new active vehicle,
    // overzelously disconnect all subscribers interested in the outgoing active vechicle.
    Vehicle* outgoingManagerVehicle = _masterController->managerVehicle();
    // spyMissionManager emulates a subscriber that should not be disconnected when
    // the active vehicle changes
    MultiSignalSpy spyMissionManager;
    spyMissionManager.init(outgoingManagerVehicle->missionManager());
    MultiSignalSpy spyMasterController;
    spyMasterController.init(_masterController);
    // Since MissionManager works with actual vehicles (which we don't have in the test cycle)
    // we have to be a bit creative emulating a signal emitted by a MissionManager.
    emit outgoingManagerVehicle->missionManager()->error(0, "");
    QVERIFY(spyMissionManager.onlyEmittedOnce("error"));
    spyMissionManager.clearSignal("error");
    QVERIFY(spyMissionManager.noneEmitted());

    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(spyMasterController.emittedOnce("managerVehicleChanged"));

    emit outgoingManagerVehicle->missionManager()->error(0, "");
    // This signal was affected by the defect - it wouldn't reach the subscriber. Here
    // we make sure it does.
    QVERIFY(spyMissionManager.onlyEmittedOnce("error"));
}

void PlanMasterControllerTest::_testDirtyFlagsMatrix_data()
{
    // Dirty-state transition matrix ("unchanged" means preserve prior value):
    //
    // | State \ Action | Upload OK | Clear | SaveDirty=true | Load plan | Save file OK | Clear save-dirty | Download w/ items | Download empty |
    // |----------------|-----------|-------|----------------|-----------|--------------|------------------|-------------------|----------------|
    // | dirtyForSave   | unchanged | false | true           | false     | false        | false            | false             | false          |
    // | dirtyForUpload | false     | false | true           | true      | unchanged    | unchanged        | false             | false          |

    // Data columns:
    //  - scenario: DirtyScenario enum value selecting which action path to execute
    //  - initialDirtyForSave: initial dirtyForSave state before action (DirtyStateTrue/False)
    //  - initialDirtyForUpload: initial dirtyForUpload state before action (DirtyStateTrue/False)
    //  - expectedDirtyForSave: expected final dirtyForSave state (DirtyState)
    //  - expectedDirtyForUpload: expected final dirtyForUpload state (DirtyState)

    QTest::addColumn<int>("scenario");
    QTest::addColumn<int>("initialDirtyForSave");
    QTest::addColumn<int>("initialDirtyForUpload");
    QTest::addColumn<int>("expectedDirtyForSave");
    QTest::addColumn<int>("expectedDirtyForUpload");

    struct ScenarioExpectation {
        DirtyScenario scenario;
        const char* name;
        DirtyState expectedDirtyForSave;
        DirtyState expectedDirtyForUpload;
    };

    const QList<ScenarioExpectation> scenarioExpectations = {
        { UploadPreservesSaveDirtyFalse,      "upload completion keeps save false",  DirtyStateUnchanged, DirtyStateFalse },
        { UploadPreservesSaveDirtyTrue,       "upload completion keeps save true",   DirtyStateUnchanged, DirtyStateFalse },
        { UploadFalseOnPlanClear,             "upload false on clear",               DirtyStateFalse,     DirtyStateFalse },
        { UploadTrueWhenSaveTrue,             "upload true when save true",          DirtyStateTrue,      DirtyStateTrue },
        { UploadTrueOnNewPlanLoad,            "upload true on new plan load",        DirtyStateFalse,     DirtyStateTrue },
        { SaveToFilePreservesUploadDirtyTrue, "saveToFile keeps upload true",        DirtyStateFalse,     DirtyStateUnchanged },
        { SaveToFilePreservesUploadDirtyFalse,"saveToFile keeps upload false",       DirtyStateFalse,     DirtyStateUnchanged },
        { SaveFalseOnSuccessfulLoad,          "save false on successful load",       DirtyStateFalse,     DirtyStateTrue },
        { ClearSaveDirtyPreservesUploadTrue,  "clear save dirty keeps upload true",  DirtyStateFalse,     DirtyStateUnchanged },
        { ClearSaveDirtyPreservesUploadFalse, "clear save dirty keeps upload false", DirtyStateFalse,     DirtyStateUnchanged },
        { DownloadWithItemsNotDirtyForSave,   "download with items stays clean",     DirtyStateFalse,     DirtyStateFalse },
        { DownloadEmptyNotDirtyForSave,       "download empty keeps save clean",     DirtyStateFalse,     DirtyStateFalse },
    };

    const QList<DirtyState> initialStates = {
        DirtyStateFalse,
        DirtyStateTrue,
    };

    for (const ScenarioExpectation& expectation : scenarioExpectations) {
        for (const DirtyState initialDirtyForSave : initialStates) {
            for (const DirtyState initialDirtyForUpload : initialStates) {
                DirtyState expectedDirtyForSave = expectation.expectedDirtyForSave;
                DirtyState expectedDirtyForUpload = expectation.expectedDirtyForUpload;

                if ((expectation.scenario == UploadTrueWhenSaveTrue) && (initialDirtyForSave == DirtyStateTrue)) {
                    // _setDirtyForSave(true) only drives dirtyForUpload when dirtyForSave transitions false->true.
                    // If dirtyForSave already starts true, dirtyForUpload is preserved.
                    expectedDirtyForUpload = DirtyStateUnchanged;
                }

                const QString rowName = QStringLiteral("%1 [init save=%2 upload=%3]")
                                            .arg(expectation.name)
                                            .arg(initialDirtyForSave == DirtyStateTrue ? QStringLiteral("true") : QStringLiteral("false"))
                                            .arg(initialDirtyForUpload == DirtyStateTrue ? QStringLiteral("true") : QStringLiteral("false"));
                QTest::newRow(rowName.toLatin1().constData())
                    << +expectation.scenario
                    << +initialDirtyForSave
                    << +initialDirtyForUpload
                    << +expectedDirtyForSave
                    << +expectedDirtyForUpload;
            }
        }
    }
}

void PlanMasterControllerTest::_testDirtyFlagsMatrix()
{
    QFETCH(int, scenario);
    QFETCH(int, initialDirtyForSave);
    QFETCH(int, initialDirtyForUpload);
    QFETCH(int, expectedDirtyForSave);
    QFETCH(int, expectedDirtyForUpload);

    QVERIFY(initialDirtyForSave != DirtyStateUnchanged);
    QVERIFY(initialDirtyForUpload != DirtyStateUnchanged);

    // Pre-load items for scenarios that need containsItems() == true
    if (scenario == DownloadWithItemsNotDirtyForSave) {
        _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    }

    const auto dirtyStateToBool = [](int state) -> bool {
        switch (state) {
        case DirtyStateFalse:
            return false;
        case DirtyStateTrue:
            return true;
        default:
            Q_ASSERT(false); // Invalid test data
            return false;
        }
    };

    _masterController->_setDirtyForSaveUnitTest(dirtyStateToBool(initialDirtyForSave));
    _masterController->_setDirtyForUploadUnitTest(dirtyStateToBool(initialDirtyForUpload));

    const bool initialDirtyForSaveBool = _masterController->dirtyForSave();
    const bool initialDirtyForUploadBool = _masterController->dirtyForUpload();

    QSignalSpy dirtyForSaveChangedSpy(_masterController, &PlanMasterController::dirtyForSaveChanged);
    QSignalSpy dirtyForUploadChangedSpy(_masterController, &PlanMasterController::dirtyForUploadChanged);

    switch (scenario) {
    case UploadPreservesSaveDirtyFalse: {
        _masterController->_sendSequence = PlanMasterController::SyncSequence::RallyPoints;
        _masterController->_sendRallyPointsComplete();
        break;
    }
    case UploadPreservesSaveDirtyTrue: {
        _masterController->_sendSequence = PlanMasterController::SyncSequence::RallyPoints;
        _masterController->_sendRallyPointsComplete();
        break;
    }
    case UploadFalseOnPlanClear:
        _masterController->removeAll();
        break;
    case UploadTrueWhenSaveTrue:
        _masterController->_setDirtyForSave(true);
        break;
    case UploadTrueOnNewPlanLoad:
        _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
        break;
    case SaveToFilePreservesUploadDirtyTrue: {
        const QString saveFile = QDir::temp().filePath(QStringLiteral("qgc_planmaster_test_%1.plan").arg(QDateTime::currentMSecsSinceEpoch()));
        QVERIFY(_masterController->saveToFile(saveFile));
        QFile::remove(saveFile);
        break;
    }
    case SaveToFilePreservesUploadDirtyFalse: {
        const QString saveFile = QDir::temp().filePath(QStringLiteral("qgc_planmaster_test_%1.plan").arg(QDateTime::currentMSecsSinceEpoch()));
        QVERIFY(_masterController->saveToFile(saveFile));
        QFile::remove(saveFile);
        break;
    }
    case SaveFalseOnSuccessfulLoad:
        _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
        break;
    case ClearSaveDirtyPreservesUploadTrue:
        _masterController->_setDirtyForSave(false);
        break;
    case ClearSaveDirtyPreservesUploadFalse:
        _masterController->_setDirtyForSave(false);
        break;
    case DownloadWithItemsNotDirtyForSave: {
        QVERIFY(_masterController->containsItems());
        _masterController->_loadSequence = PlanMasterController::SyncSequence::RallyPoints;
        _masterController->_loadRallyPointsComplete();
        break;
    }
    case DownloadEmptyNotDirtyForSave: {
        QVERIFY(!_masterController->containsItems());
        _masterController->_loadSequence = PlanMasterController::SyncSequence::RallyPoints;
        _masterController->_loadRallyPointsComplete();
        break;
    }
    }

    const auto resolveExpected = [](int expectedState, bool unchangedValue) -> bool {
        switch (expectedState) {
        case DirtyStateFalse:
            return false;
        case DirtyStateTrue:
            return true;
        case DirtyStateUnchanged:
            return unchangedValue;
        }
        return unchangedValue;
    };

    const bool expectedDirtyForSaveBool = resolveExpected(expectedDirtyForSave, initialDirtyForSaveBool);
    const bool expectedDirtyForUploadBool = resolveExpected(expectedDirtyForUpload, initialDirtyForUploadBool);
    const int expectedDirtyForSaveSignalCount = (expectedDirtyForSaveBool != initialDirtyForSaveBool) ? 1 : 0;
    const int expectedDirtyForUploadSignalCount = (expectedDirtyForUploadBool != initialDirtyForUploadBool) ? 1 : 0;

    QCOMPARE(_masterController->dirtyForSave(), expectedDirtyForSaveBool);
    QCOMPARE(_masterController->dirtyForUpload(), expectedDirtyForUploadBool);

    QCOMPARE(dirtyForSaveChangedSpy.count(), expectedDirtyForSaveSignalCount);
    QCOMPARE(dirtyForUploadChangedSpy.count(), expectedDirtyForUploadSignalCount);

    if (dirtyForSaveChangedSpy.count() > 0) {
        const QList<QVariant>& args = dirtyForSaveChangedSpy.at(dirtyForSaveChangedSpy.count() - 1);
        QCOMPARE(args.count(), 1);
        QCOMPARE(args.first().toBool(), _masterController->dirtyForSave());
    }

    if (dirtyForUploadChangedSpy.count() > 0) {
        const QList<QVariant>& args = dirtyForUploadChangedSpy.at(dirtyForUploadChangedSpy.count() - 1);
        QCOMPARE(args.count(), 1);
        QCOMPARE(args.first().toBool(), _masterController->dirtyForUpload());
    }
}

void PlanMasterControllerTest::_testFileAssociationSetOnLoad()
{
    QSignalSpy currentFileSpy(_masterController, &PlanMasterController::currentPlanFileChanged);

    // Before load, file association should be empty
    QVERIFY(_masterController->currentPlanFile().isEmpty());
    QVERIFY(_masterController->currentPlanFileName().isEmpty());

    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");

    // After successful load, the file association should be set
    QVERIFY(!_masterController->currentPlanFile().isEmpty());
    QCOMPARE(_masterController->currentPlanFileName(), QStringLiteral("MissionPlanner"));
    QVERIFY(currentFileSpy.count() >= 1);
}

void PlanMasterControllerTest::_testFailedLoadClearsFileAssociation()
{
    struct MalformedFileCase {
        const char* fileName;
        const char* contents;
    };
    const QList<MalformedFileCase> malformedCases = {
        { "Malformed.waypoints", "not a mission file" }, // Text parse failure
        { "Malformed.plan",      "{ not valid json" },   // JSON validation failure
    };

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    for (const MalformedFileCase& malformedCase : malformedCases) {
        const QString context = QString::fromLatin1(malformedCase.fileName);

        _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
        QVERIFY2(!_masterController->currentPlanFile().isEmpty(), qPrintable(context));

        const QString malformedPath = tempDir.filePath(context);
        QFile malformedFile(malformedPath);
        QVERIFY2(malformedFile.open(QIODevice::WriteOnly), qPrintable(context));
        QVERIFY2(malformedFile.write(malformedCase.contents) != -1, qPrintable(context));
        malformedFile.close();

        // A failed load clears the file association
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression("Error loading Plan file"));
        _masterController->loadFromFile(malformedPath);
        verifyExpectedLogMessage();

        QVERIFY2(_masterController->currentPlanFile().isEmpty(), qPrintable(context));
        QVERIFY2(_masterController->currentPlanFileName().isEmpty(), qPrintable(context));
    }
}

void PlanMasterControllerTest::_testDownloadClearsFileAssociation()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->currentPlanFile().isEmpty());

    // Download completion replaces editor contents with the vehicle's plan, so the
    // previous file association no longer describes what is in the editor
    _masterController->_loadSequence = PlanMasterController::SyncSequence::RallyPoints;
    _masterController->_loadRallyPointsComplete();

    QVERIFY(_masterController->currentPlanFile().isEmpty());
    QVERIFY(_masterController->currentPlanFileName().isEmpty());
    QVERIFY(!_masterController->dirtyForSave());
    QVERIFY(!_masterController->dirtyForUpload());
}

void PlanMasterControllerTest::_testBackgroundSyncPreservesFileAssociation()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->currentPlanFile().isEmpty());
    _masterController->_setDirtyForSaveUnitTest(true);

    // Manager loadComplete also fires for the automatic download at vehicle connect. When no
    // download was requested the editor contents are preserved, so the file association and
    // dirty state must survive.
    _masterController->_loadRallyPointsComplete();

    QVERIFY(!_masterController->currentPlanFile().isEmpty());
    QCOMPARE(_masterController->currentPlanFileName(), QStringLiteral("MissionPlanner"));
    QVERIFY(_masterController->dirtyForSave());
}

void PlanMasterControllerTest::_testUnrequestedSendCompletePreservesDirtyForUpload()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(_masterController->dirtyForUpload());

    // Manager sendComplete can fire for sends this controller did not request (Fly view and
    // Plan view controllers share the same vehicle managers), so dirtyForUpload must survive.
    _masterController->_sendRallyPointsComplete();

    QVERIFY(_masterController->dirtyForUpload());
}

void PlanMasterControllerTest::_testStaleInitialPlanLoadRallyCompletePreservesPlan()
{
    // A rally-unsupported vehicle never emits rally loadComplete during initial connect, so the
    // sequence can still be InitialPlanLoad long after the download. A later unrelated rally
    // completion must not clear a newly opened plan.
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->currentPlanFile().isEmpty());

    _masterController->_loadSequence = PlanMasterController::SyncSequence::InitialPlanLoad;
    _masterController->_loadRallyPointsComplete();

    QVERIFY(!_masterController->currentPlanFile().isEmpty());
    QCOMPARE(_masterController->currentPlanFileName(), QStringLiteral("MissionPlanner"));
}

void PlanMasterControllerTest::_testShowPlanFromVehicleClearsFileAssociation()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QTRY_VERIFY_WITH_TIMEOUT(_masterController->managerVehicle()->initialPlanRequestComplete(), 10000);

    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->currentPlanFile().isEmpty());

    // Showing the vehicle's plan replaces the editor contents, so the previous file
    // association no longer describes them
    _masterController->showPlanFromManagerVehicle();

    QVERIFY(_masterController->currentPlanFile().isEmpty());
    QVERIFY(!_masterController->dirtyForSave());
}

void PlanMasterControllerTest::_testFileAssociationClearedOnRemoveAll()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->currentPlanFile().isEmpty());

    QSignalSpy currentFileSpy(_masterController, &PlanMasterController::currentPlanFileChanged);

    _masterController->removeAll();

    QVERIFY(_masterController->currentPlanFile().isEmpty());
    QVERIFY(_masterController->currentPlanFileName().isEmpty());
    QVERIFY(currentFileSpy.count() >= 1);
}

void PlanMasterControllerTest::_testFileAssociationClearedOnRemoveAllFromVehicle()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->currentPlanFile().isEmpty());

    QSignalSpy currentFileSpy(_masterController, &PlanMasterController::currentPlanFileChanged);

    _masterController->removeAllFromVehicle();

    QVERIFY(_masterController->currentPlanFile().isEmpty());
    QVERIFY(_masterController->currentPlanFileName().isEmpty());
    QVERIFY(currentFileSpy.count() >= 1);
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleCompletedOnSuccess()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();
    QVERIFY(completedSpy.isEmpty());  // Aggregate signal must wait for every plan element that was actually requested

    const bool geoFenceSupported = _masterController->geoFenceController()->supported();
    const bool rallySupported = _masterController->rallyPointController()->supported();

    _masterController->_removeAllFromVehicleStepComplete(false /* error */);  // Mission
    if (geoFenceSupported) {
        QVERIFY(completedSpy.isEmpty());
        _masterController->_removeAllFromVehicleStepComplete(false /* error */);  // GeoFence
    }
    if (rallySupported) {
        QVERIFY(completedSpy.isEmpty());
        _masterController->_removeAllFromVehicleStepComplete(false /* error */);  // RallyPoints
    }

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.first().at(0).toBool(), false);
    QCOMPARE(completedSpy.first().at(1).toInt(), _vehicle->id());
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleCompletedOnFailure()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();

    const bool geoFenceSupported = _masterController->geoFenceController()->supported();
    const bool rallySupported = _masterController->rallyPointController()->supported();

    // Mission removal is always requested, so failing it deterministically exercises the
    // aggregate error path regardless of which optional elements this vehicle supports.
    _masterController->_removeAllFromVehicleStepComplete(true /* error */);  // Mission fails
    if (geoFenceSupported) {
        QVERIFY(completedSpy.isEmpty());
        _masterController->_removeAllFromVehicleStepComplete(false /* error */);  // GeoFence succeeds
    }
    if (rallySupported) {
        QVERIFY(completedSpy.isEmpty());
        _masterController->_removeAllFromVehicleStepComplete(false /* error */);  // RallyPoints succeeds
    }

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.first().at(0).toBool(), true);  // One failed element makes the whole aggregate an error
    QCOMPARE(completedSpy.first().at(1).toInt(), _vehicle->id());
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleCompletedOnVehicleDisconnect()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();
    QVERIFY(completedSpy.isEmpty());
    const int vehicleId = _vehicle->id();

    // The vehicle disconnects before mission/geoFence/rallyPoint ever report removeAllComplete.
    // The pending request can never complete, so it must be reported as an error rather than left hanging.
    _disconnectMockLink();

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.first().at(0).toBool(), true);
    QCOMPARE(completedSpy.first().at(1).toInt(), vehicleId);

    // A stray completion arriving after the reset (e.g. a delayed signal from the old vehicle)
    // must not crash or re-emit the aggregate signal.
    completedSpy.clear();
    _masterController->_removeAllFromVehicleStepComplete(false /* error */);
    QVERIFY(completedSpy.isEmpty());
}

Vehicle* PlanMasterControllerTest::_connectExtraVehicle()
{
    MultiVehicleManager* const manager = MultiVehicleManager::instance();
    MockLink* const link = MockLink::startPX4MockLink();
    if (!link) {
        return nullptr;
    }
    _extraLinks.append(link);
    const int vehicleId = link->vehicleId();
    const bool connected = UnitTest::waitForCondition(
        [manager, vehicleId] {
            const Vehicle* const vehicle = manager->getVehicleById(vehicleId);
            return vehicle && vehicle->isInitialConnectComplete();
        },
        TestTimeout::longMs(), QStringLiteral("extra vehicle initial connect"));
    return connected ? manager->getVehicleById(vehicleId) : nullptr;
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleCompletedFromVehicleAcks()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    // Repeated clears drive the real MISSION_CLEAR_ALL acks through the fan-in, so any state leaking
    // from one request into the next shows up as a missing, duplicate or failed completion.
    for (int i = 0; i < 5; i++) {
        QVERIFY_TRUE_WAIT(!_masterController->syncInProgress(), TestTimeout::mediumMs());
        completedSpy.clear();
        _masterController->removeAllFromVehicle();
        QVERIFY_TRUE_WAIT(completedSpy.count() >= 1, TestTimeout::mediumMs());
        QCOMPARE(completedSpy.first().at(0).toBool(), false);
        QVERIFY_NO_SIGNAL_WAIT(completedSpy, 100);  // Short on purpose: only guards against a duplicate emission
        QCOMPARE(completedSpy.count(), 1);
    }
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleRejectsOverlappingRequest()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();
    QVERIFY(_masterController->syncInProgress());

    // A second request before the first completes must not reset the pending count mid-flight
    expectLogMessage("PlanManager.PlanMasterController", QtCriticalMsg,
                     QRegularExpression(QStringLiteral("removeAllFromVehicle called while syncInProgress")));
    _masterController->removeAllFromVehicle();
    verifyExpectedLogMessage();

    QVERIFY_TRUE_WAIT(completedSpy.count() >= 1, TestTimeout::mediumMs());
    QVERIFY_NO_SIGNAL_WAIT(completedSpy, 100);  // Short on purpose: only guards against a duplicate emission
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.first().at(0).toBool(), false);
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleFollowsRequestedVehicle()
{
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle* const otherVehicle = _connectExtraVehicle();
    QVERIFY(otherVehicle);
    QCOMPARE(_masterController->managerVehicle(), _vehicle);
    QVERIFY_TRUE_WAIT(!_masterController->syncInProgress(), TestTimeout::mediumMs());

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();

    // The active vehicle changes while the requested vehicle's acks are still in flight - on a real radio link this
    // is the common case, and MultiVehicleManager itself switches active vehicle whenever any other vehicle drops out.
    // Delivered synchronously here so the requested vehicle's acks deterministically arrive after the switch.
    _masterController->_activeVehicleChanged(otherVehicle);

    // The requested vehicle is still connected and confirms the removal, so the result must be a success
    // rather than a spurious failure prompting the operator to "try again" against a different vehicle.
    QVERIFY_TRUE_WAIT(completedSpy.count() >= 1, TestTimeout::mediumMs());
    QVERIFY_NO_SIGNAL_WAIT(completedSpy, 100);  // Short on purpose: only guards against a duplicate emission
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.first().at(0).toBool(), false);
    QCOMPARE(completedSpy.first().at(1).toInt(), _vehicle->id());
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleSupersededOnOtherVehicle()
{
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle* const otherVehicle = _connectExtraVehicle();
    QVERIFY(otherVehicle);
    QVERIFY_TRUE_WAIT(!_masterController->syncInProgress(), TestTimeout::mediumMs());

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();
    _masterController->_activeVehicleChanged(otherVehicle);
    QVERIFY(!_masterController->syncInProgress());

    // A new request on the now-active vehicle before the first one's acks arrive: the first can no longer be
    // attributed, so it resolves as unconfirmed rather than hanging or being counted against the new request.
    _masterController->removeAllFromVehicle();
    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.at(0).at(0).toBool(), true);
    QCOMPARE(completedSpy.at(0).at(1).toInt(), _vehicle->id());

    QVERIFY_TRUE_WAIT(completedSpy.count() >= 2, TestTimeout::mediumMs());
    QVERIFY_NO_SIGNAL_WAIT(completedSpy, 100);  // Short on purpose: the first vehicle's late acks must be ignored
    QCOMPARE(completedSpy.count(), 2);
    QCOMPARE(completedSpy.at(1).at(0).toBool(), false);
    QCOMPARE(completedSpy.at(1).at(1).toInt(), otherVehicle->id());
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleUnderFleetChurn()
{
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    // A vehicle torn down while its available-modes request is in flight fails that request
    ignoreLogMessage("Vehicle.StandardModes", QtWarningMsg, QRegularExpression("Failed to retrieve available modes"));
    MultiVehicleManager* const manager = MultiVehicleManager::instance();

    for (int i = 0; i < 3; i++) {
        QVERIFY(_connectExtraVehicle());
    }

    const quint32 seed = QRandomGenerator::global()->generate();  // Reported in every failure message for replay
    QRandomGenerator random(seed);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    for (int iteration = 0; iteration < 20; iteration++) {
        const QString context = QStringLiteral("seed %1 iteration %2").arg(seed).arg(iteration);
        const int vehicleCount = manager->vehicles()->count();
        QVERIFY2(vehicleCount >= 2, qPrintable(context));

        QPointer<Vehicle> target = manager->vehicles()->value<Vehicle*>(random.bounded(vehicleCount));
        manager->setActiveVehicle(target);
        QVERIFY2(UnitTest::waitForCondition(
                     [this, target] {
                         return target && _masterController->managerVehicle() == target &&
                                !_masterController->syncInProgress();
                     },
                     TestTimeout::longMs(), context),
                 qPrintable(context));

        completedSpy.clear();
        const int targetId = target->id();
        _masterController->removeAllFromVehicle();

        Vehicle* bystander = nullptr;
        for (int i = 0; i < manager->vehicles()->count(); i++) {
            Vehicle* const vehicle = manager->vehicles()->value<Vehicle*>(i);
            if (vehicle != target) {
                bystander = vehicle;
                break;
            }
        }
        QVERIFY2(bystander, qPrintable(context));

        switch (random.bounded(3)) {
            case 0:
                // Operator switches to another vehicle mid-clear
                manager->setActiveVehicle(bystander);
                break;
            case 1: {
                // An unrelated vehicle drops out mid-clear (MultiVehicleManager then re-picks vehicles[0] as active),
                // and a replacement joins so the fleet keeps its size
                for (const QPointer<MockLink>& link : std::as_const(_extraLinks)) {
                    if (link && link->vehicleId() == bystander->id()) {
                        link->disconnect();
                        break;
                    }
                }
                QVERIFY2(_connectExtraVehicle(), qPrintable(context));
                break;
            }
            default:
                break;
        }

        QVERIFY2(UnitTest::waitForCondition([&completedSpy] { return completedSpy.count() >= 1; },
                                            TestTimeout::mediumMs(), QStringLiteral("completed")),
                 qPrintable(context));
        QVERIFY2(UnitTest::waitForNoSignal(completedSpy, 100, QStringLiteral("completed")), qPrintable(context));
        QVERIFY2(completedSpy.count() == 1, qPrintable(context));
        // The requested vehicle stayed connected throughout, so its confirmed removal must be reported as a success
        QVERIFY2(!completedSpy.first().at(0).toBool(), qPrintable(context));
        QVERIFY2(completedSpy.first().at(1).toInt() == targetId, qPrintable(context));
    }
}

void PlanMasterControllerTest::_testSaveUpdatesFileName()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QCOMPARE(_masterController->currentPlanFileName(), QStringLiteral("MissionPlanner"));

    // Save to a completely different path
    const QString saveFile = QDir::temp().filePath(
        QStringLiteral("qgc_planmaster_rename_%1.plan").arg(QDateTime::currentMSecsSinceEpoch()));
    QVERIFY(_masterController->saveToFile(saveFile));

    // Name should now reflect the new file base name
    QCOMPARE(_masterController->currentPlanFileName(), QFileInfo(saveFile).completeBaseName());

    // Clean up
    QFile::remove(saveFile);
}

void PlanMasterControllerTest::_testTemplateModeHidesTemplatesOnPlanCreatorSelection()
{
    // Initial state: empty plan → templates shown
    QVERIFY(_masterController->showCreateFromTemplate());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);

    // User selects a plan creator (e.g. Survey) — adds items to the plan
    SurveyPlanCreator creator(_masterController);
    creator.createPlan(QGeoCoordinate(47.0, -122.0));

    QVERIFY(_masterController->containsItems());
    QVERIFY(!_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
}

void PlanMasterControllerTest::_testTemplateModeHidesTemplatesOnFileLoad()
{
    // Initial state: empty plan, not manual creation → templates shown
    QVERIFY(_masterController->showCreateFromTemplate());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);

    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");

    QVERIFY(_masterController->containsItems());
    QVERIFY(!_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
}

void PlanMasterControllerTest::_testTemplateModeRestoredOnRemoveAll()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->showCreateFromTemplate());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);

    _masterController->removeAll();

    QVERIFY(!_masterController->containsItems());
    QVERIFY(_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
}

void PlanMasterControllerTest::_testTemplateModeRestoredOnIndividualItemRemoval()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->showCreateFromTemplate());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);

    _masterController->missionController()->removeAll();
    _masterController->geoFenceController()->removeAll();
    _masterController->rallyPointController()->removeAll();

    QVERIFY(!_masterController->containsItems());
    QVERIFY(_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
}

void PlanMasterControllerTest::_testManualCreationHidesTemplates()
{
    // Initial state: empty plan → templates shown
    QVERIFY(_masterController->showCreateFromTemplate());
    QVERIFY(!_masterController->userSelectedManualCreation());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);
    QSignalSpy spyManual(_masterController, &PlanMasterController::userSelectedManualCreationChanged);

    // User clicks "No Template" — hides templates even though plan is empty
    _masterController->setUserSelectedManualCreation(true);

    QVERIFY(_masterController->userSelectedManualCreation());
    QVERIFY(!_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
    QCOMPARE(spyManual.count(), 1);

    // Setting the same value again should not re-emit
    _masterController->setUserSelectedManualCreation(true);
    QCOMPARE(spyShow.count(), 1);
    QCOMPARE(spyManual.count(), 1);
}

void PlanMasterControllerTest::_testManualCreationRestoredOnRemoveAll()
{
    _masterController->setUserSelectedManualCreation(true);
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->showCreateFromTemplate());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);
    QSignalSpy spyManual(_masterController, &PlanMasterController::userSelectedManualCreationChanged);

    _masterController->removeAll();

    QVERIFY(!_masterController->containsItems());
    QVERIFY(!_masterController->userSelectedManualCreation());
    QVERIFY(_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
    QCOMPARE(spyManual.count(), 1);
}

void PlanMasterControllerTest::_testManualCreationRestoredOnIndividualItemRemoval()
{
    _masterController->setUserSelectedManualCreation(true);
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(!_masterController->showCreateFromTemplate());

    QSignalSpy spyShow(_masterController, &PlanMasterController::showCreateFromTemplateChanged);
    QSignalSpy spyManual(_masterController, &PlanMasterController::userSelectedManualCreationChanged);

    _masterController->missionController()->removeAll();
    _masterController->geoFenceController()->removeAll();
    _masterController->rallyPointController()->removeAll();

    QVERIFY(!_masterController->containsItems());
    QVERIFY(!_masterController->userSelectedManualCreation());
    QVERIFY(_masterController->showCreateFromTemplate());
    QCOMPARE(spyShow.count(), 1);
    QCOMPARE(spyManual.count(), 1);
}

void PlanMasterControllerTest::_testPlanCreatorsFiltered()
{
    // MultiRotor supports StructureScan — expect all 4 creators
    PlanMasterController multiRotorController(MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR);
    multiRotorController.setFlyView(false);
    multiRotorController.start();
    QVERIFY(multiRotorController.planCreators() != nullptr);
    const int multiRotorCount = multiRotorController.planCreators()->count();
    QVERIFY(multiRotorCount > 0);

    // FixedWing does not support StructureScan — expect one fewer creator
    PlanMasterController fixedWingController(MAV_AUTOPILOT_PX4, MAV_TYPE_FIXED_WING);
    fixedWingController.setFlyView(false);
    fixedWingController.start();
    QVERIFY(fixedWingController.planCreators() != nullptr);
    const int fixedWingCount = fixedWingController.planCreators()->count();
    QVERIFY(fixedWingCount > 0);

    QCOMPARE(fixedWingCount, multiRotorCount - 1);
}

#include "UnitTest.h"

UT_REGISTER_TEST(PlanMasterControllerTest, TestLabel::Integration, TestLabel::MissionManager)
