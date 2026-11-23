#include "PlanMasterControllerTest.h"

#include <limits>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaProperty>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "GeoFenceController.h"
#include "GeoFenceManager.h"
#include "MissionController.h"
#include "MissionManager.h"
#include "MockLink.h"
#include "MultiSignalSpy.h"
#include "MultiVehicleManager.h"
#include "PlanCreator.h"
#include "PlanMasterController.h"
#include "QGCMapCircle.h"
#include "QGCMapPolygon.h"
#include "QGroundControlQmlGlobal.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "SurveyPlanCreator.h"
#include "TakeoffMissionItem.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

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
    if (!_mockLink) {
        QVERIFY(UnitTest::waitForCondition([] { return MultiVehicleManager::instance()->activeVehicle() == nullptr; },
                                           TestTimeout::longMs(), QStringLiteral("active vehicle removal settled")));
    }
    _disconnectMockLink();
    UnitTest::cleanup();
}

void PlanMasterControllerTest::_testMissionPlannerFileLoad()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 6);
}

void PlanMasterControllerTest::_testManualTestPlansLoad_data()
{
    QTest::addColumn<QString>("planPath");

    const QDir plansDir(QStringLiteral(":/unittest/manualTestPlans"));
    const QStringList planFiles = plansDir.entryList({QStringLiteral("*.plan")}, QDir::Files);
    QVERIFY(!planFiles.isEmpty());
    for (const QString& planFile : planFiles) {
        QTest::newRow(qPrintable(planFile)) << plansDir.filePath(planFile);
    }
}

// test/plans are hand-loaded for manual testing, so they must stay loadable
void PlanMasterControllerTest::_testManualTestPlansLoad()
{
    QFETCH(QString, planPath);

    _masterController->loadFromFile(planPath);

    QCOMPARE(_masterController->currentPlanFile(), planPath);
    QVERIFY(_masterController->missionController()->visualItems()->count() > 1);
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
    // Each emission triggers a showAppMessage debug log via PlanMasterController and a Vehicle warning.
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("Mission transfer failed"));
    ignoreLogMessage("Vehicle.Vehicle", QtWarningMsg, QRegularExpression("Mission transfer failed"));
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

void PlanMasterControllerTest::_testUploadInterruptedByVehicleChange_data()
{
    QTest::addColumn<int>("sequence");
    QTest::newRow("idle") << int(PlanMasterController::SyncSequence::Idle);
    QTest::newRow("mission") << int(PlanMasterController::SyncSequence::Mission);
    QTest::newRow("geofence") << int(PlanMasterController::SyncSequence::GeoFence);
    QTest::newRow("rally") << int(PlanMasterController::SyncSequence::RallyPoints);
}

void PlanMasterControllerTest::_testUploadInterruptedByVehicleChange()
{
    QFETCH(int, sequence);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const target = _connectExtraVehicle();
    verifyExpectedLogMessage();
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());

    const bool interrupted = sequence != int(PlanMasterController::SyncSequence::Idle);
    _masterController->_sendSequence = static_cast<PlanMasterController::SyncSequence>(sequence);
    if (interrupted) {
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression(QStringLiteral("Plan upload interrupted")));
    }
    _masterController->_activeVehicleChanged(target);
    if (interrupted) {
        verifyExpectedLogMessage();
        QVERIFY(_masterController->dirtyForUpload());
    }
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
        {UploadPreservesSaveDirty, "upload completion keeps save dirty", DirtyStateUnchanged, DirtyStateFalse},
        {UploadFalseOnPlanClear, "upload false on clear", DirtyStateFalse, DirtyStateFalse},
        {UploadTrueWhenSaveTrue, "upload true when save true", DirtyStateTrue, DirtyStateTrue},
        {UploadTrueOnNewPlanLoad, "upload true on new plan load", DirtyStateFalse, DirtyStateTrue},
        {SaveToFilePreservesUploadDirty, "saveToFile keeps upload dirty", DirtyStateFalse, DirtyStateUnchanged},
        {SaveFalseOnSuccessfulLoad, "save false on successful load", DirtyStateFalse, DirtyStateTrue},
        {ClearSaveDirtyPreservesUpload, "clear save dirty keeps upload", DirtyStateFalse, DirtyStateUnchanged},
        {DownloadWithItemsNotDirtyForSave, "download with items stays clean", DirtyStateFalse, DirtyStateFalse},
        {DownloadEmptyNotDirtyForSave, "download empty keeps save clean", DirtyStateFalse, DirtyStateFalse},
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
        case UploadPreservesSaveDirty:
            _masterController->_sendSequence = PlanMasterController::SyncSequence::RallyPoints;
            _masterController->_sendRallyPointsComplete();
            break;
        case UploadFalseOnPlanClear:
            _masterController->removeAll();
            break;
        case UploadTrueWhenSaveTrue:
            _masterController->_setDirtyForSave(true);
            break;
        case UploadTrueOnNewPlanLoad:
            _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
            break;
        case SaveToFilePreservesUploadDirty: {
            const QString saveFile = QDir::temp().filePath(
                QStringLiteral("qgc_planmaster_test_%1.plan").arg(QDateTime::currentMSecsSinceEpoch()));
            QVERIFY(_masterController->saveToFile(saveFile));
            QFile::remove(saveFile);
            break;
        }
        case SaveFalseOnSuccessfulLoad:
            _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
            break;
        case ClearSaveDirtyPreservesUpload:
            _masterController->_setDirtyForSave(false);
            break;
        case DownloadWithItemsNotDirtyForSave: {
            QVERIFY(_masterController->containsItems());
            _masterController->_loadVehicleId = 1;
            _masterController->_loadSequence = PlanMasterController::SyncSequence::RallyPoints;
            _masterController->_loadRallyPointsComplete();
            break;
        }
        case DownloadEmptyNotDirtyForSave: {
            QVERIFY(!_masterController->containsItems());
            _masterController->_loadVehicleId = 1;
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
    _masterController->_loadVehicleId = 1;
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
    QCOMPARE(completedSpy.first().at(1).toUInt(), _vehicle->id());
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
    QCOMPARE(completedSpy.first().at(1).toUInt(), _vehicle->id());
}

void PlanMasterControllerTest::_testRemoveAllFromVehicleCompletedOnVehicleDisconnect()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);

    QSignalSpy completedSpy(_masterController, &PlanMasterController::removeAllFromVehicleCompleted);

    _masterController->removeAllFromVehicle();
    QVERIFY(completedSpy.isEmpty());
    const quint32 vehicleId = _vehicle->id();

    // The vehicle disconnects before mission/geoFence/rallyPoint ever report removeAllComplete.
    // The pending request can never complete, so it must be reported as an error rather than left hanging.
    _disconnectMockLink();

    QCOMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.first().at(0).toBool(), true);
    QCOMPARE(completedSpy.first().at(1).toUInt(), vehicleId);

    // A stray completion arriving after the reset (e.g. a delayed signal from the old vehicle)
    // must not crash or re-emit the aggregate signal.
    completedSpy.clear();
    _masterController->_removeAllFromVehicleStepComplete(false /* error */);
    QVERIFY(completedSpy.isEmpty());
}

Vehicle* PlanMasterControllerTest::_connectExtraVehicle(bool withMission)
{
    MultiVehicleManager* const manager = MultiVehicleManager::instance();
    MockLink* const link = withMission ? MockLink::startPX4MockLinkWithMission() : MockLink::startPX4MockLink();
    if (!link) {
        return nullptr;
    }
    _extraLinks.append(link);
    const quint32 vehicleId = link->vehicleId();
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
        _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
        QVERIFY(_vehicle);
        QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
        _masterController->removeAllFromVehicle();
        QVERIFY_TRUE_WAIT(completedSpy.count() >= 1, TestTimeout::mediumMs());
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
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
    QCOMPARE(completedSpy.first().at(1).toUInt(), _vehicle->id());
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
    QCOMPARE(completedSpy.at(0).at(1).toUInt(), _vehicle->id());

    QVERIFY_TRUE_WAIT(completedSpy.count() >= 2, TestTimeout::mediumMs());
    QVERIFY_NO_SIGNAL_WAIT(completedSpy, 100);  // Short on purpose: the first vehicle's late acks must be ignored
    QCOMPARE(completedSpy.count(), 2);
    QCOMPARE(completedSpy.at(1).at(0).toBool(), false);
    QCOMPARE(completedSpy.at(1).at(1).toUInt(), otherVehicle->id());
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
        const quint32 targetId = target->id();
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
        QVERIFY2(completedSpy.first().at(1).toUInt() == targetId, qPrintable(context));
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

void PlanMasterControllerTest::_testEditMarksDirtyForSave_data()
{
    QTest::addColumn<bool>("reloadAfterSave");
    QTest::addColumn<QString>("editTarget");

    for (const bool reload : {false, true}) {
        for (const QString& target :
             {QStringLiteral("waypoint"), QStringLiteral("fencePolygon"), QStringLiteral("fenceCircle"),
              QStringLiteral("rallyPoint"), QStringLiteral("rallyPointFact")}) {
            QTest::newRow(qPrintable(QStringLiteral("%1, %2").arg(reload ? "after load" : "after save", target)))
                << reload << target;
        }
    }
}

void PlanMasterControllerTest::_testEditMarksDirtyForSave()
{
    QFETCH(bool, reloadAfterSave);
    QFETCH(QString, editTarget);

    const QGeoCoordinate center(47.3977, 8.5456);
    MissionController* const missionController = _masterController->missionController();
    GeoFenceController* const geoFenceController = _masterController->geoFenceController();
    RallyPointController* const rallyPointController = _masterController->rallyPointController();
    missionController->setHomePosition(center);
    QVERIFY(missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(200, 0), 1, false));
    geoFenceController->addInclusionPolygon(center.atDistanceAndAzimuth(500, 315),
                                            center.atDistanceAndAzimuth(500, 135));
    geoFenceController->addInclusionCircle(center.atDistanceAndAzimuth(300, 315),
                                           center.atDistanceAndAzimuth(300, 135));
    rallyPointController->addPoint(center.atDistanceAndAzimuth(100, 180));
    QVERIFY(_masterController->dirtyForSave());

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString planFile = tempDir.filePath(QStringLiteral("edit.plan"));
    QVERIFY(_masterController->saveToFile(planFile));
    if (reloadAfterSave) {
        _masterController->removeAll();
        _masterController->loadFromFile(planFile);
        QCOMPARE(missionController->visualItems()->count(), 2);
        QCOMPARE(geoFenceController->polygons()->count(), 1);
        QCOMPARE(geoFenceController->circles()->count(), 1);
        QCOMPARE(rallyPointController->points()->count(), 1);
    }
    QVERIFY(!_masterController->dirtyForSave());

    if (editTarget == QStringLiteral("waypoint")) {
        SimpleMissionItem* const waypoint = missionController->visualItems()->value<SimpleMissionItem*>(1);
        QVERIFY(waypoint);
        waypoint->altitude()->setRawValue(waypoint->altitude()->rawValue().toDouble() + 10);
    } else if (editTarget == QStringLiteral("fencePolygon")) {
        QGCMapPolygon* const polygon = geoFenceController->polygons()->value<QGCMapPolygon*>(0);
        QVERIFY(polygon);
        polygon->adjustVertex(0, polygon->vertexCoordinate(0).atDistanceAndAzimuth(50, 0));
    } else if (editTarget == QStringLiteral("fenceCircle")) {
        QGCMapCircle* const circle = geoFenceController->circles()->value<QGCMapCircle*>(0);
        QVERIFY(circle);
        circle->radius()->setRawValue(circle->radius()->rawValue().toDouble() + 50);
    } else if (editTarget == QStringLiteral("rallyPointFact")) {
        // The rally point editor writes these Facts directly
        RallyPoint* const rallyPoint = rallyPointController->points()->value<RallyPoint*>(0);
        QVERIFY(rallyPoint);
        Fact* const altitudeFact = rallyPoint->altitude();
        altitudeFact->setRawValue(altitudeFact->rawValue().toDouble() + 10);
    } else {
        RallyPoint* const rallyPoint = rallyPointController->points()->value<RallyPoint*>(0);
        QVERIFY(rallyPoint);
        rallyPoint->setCoordinate(rallyPoint->coordinate().atDistanceAndAzimuth(50, 0));
    }

    QVERIFY(_masterController->dirtyForSave());
}

void PlanMasterControllerTest::_testRallyPointFileFrames_data()
{
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<bool>("expectLoaded");
    QTest::addColumn<QList<int>>("expectedFrames");

    const int relative = QGroundControlQmlGlobal::AltitudeFrameRelative;
    const int absolute = QGroundControlQmlGlobal::AltitudeFrameAbsolute;
    const int terrain = QGroundControlQmlGlobal::AltitudeFrameTerrain;

    // Frames are stored as MAV_FRAME numbers: 0 global (AMSL), 3 global relative alt, 10 global terrain alt
    QTest::newRow("version 3 keeps each frame") << QByteArray(R"({"version":3,"points":[
               {"coordinate":[47.1,8.1,10],"frame":3},
               {"coordinate":[47.2,8.2,500],"frame":0},
               {"coordinate":[47.3,8.3,20],"frame":10}]})")
                                                << true << QList<int>{relative, absolute, terrain};
    QTest::newRow("version 2 has no frames so it is relative")
        << QByteArray(R"({"version":2,"points":[[47.1,8.1,10],[47.2,8.2,20]]})") << true
        << QList<int>{relative, relative};
    QTest::newRow("version 3 unsupported frame")
        << QByteArray(R"({"version":3,"points":[{"coordinate":[47.1,8.1,10],"frame":1}]})") << false << QList<int>{};
    QTest::newRow("version 3 missing frame")
        << QByteArray(R"({"version":3,"points":[{"coordinate":[47.1,8.1,10]}]})") << false << QList<int>{};
    QTest::newRow("version 3 point is not an object")
        << QByteArray(R"({"version":3,"points":[[47.1,8.1,10]]})") << false << QList<int>{};
    QTest::newRow("newer version") << QByteArray(R"({"version":4,"points":[]})") << false << QList<int>{};
}

void PlanMasterControllerTest::_testRallyPointFileFrames()
{
    QFETCH(QByteArray, json);
    QFETCH(bool, expectLoaded);
    QFETCH(QList<int>, expectedFrames);

    RallyPointController* const rallyPointController = _masterController->rallyPointController();
    QString errorString;

    QCOMPARE(rallyPointController->load(QJsonDocument::fromJson(json).object(), errorString), expectLoaded);
    QCOMPARE(errorString.isEmpty(), expectLoaded);
    if (!expectLoaded) {
        return;
    }

    QCOMPARE(rallyPointController->points()->count(), expectedFrames.count());
    for (int i = 0; i < expectedFrames.count(); i++) {
        const RallyPoint* const rallyPoint = rallyPointController->points()->value<RallyPoint*>(i);
        QVERIFY(rallyPoint);
        QCOMPARE(int(rallyPoint->altitudeFrame()), expectedFrames[i]);
    }
}

void PlanMasterControllerTest::_testRallyPointFramesSurviveSave()
{
    RallyPointController* const rallyPointController = _masterController->rallyPointController();
    rallyPointController->points()->append(new RallyPoint(
        QGeoCoordinate(47.1, 8.1, 10), QGroundControlQmlGlobal::AltitudeFrameRelative, rallyPointController));
    rallyPointController->points()->append(new RallyPoint(
        QGeoCoordinate(47.2, 8.2, 500), QGroundControlQmlGlobal::AltitudeFrameAbsolute, rallyPointController));

    QJsonObject json;
    rallyPointController->save(json);
    QString errorString;
    QVERIFY2(rallyPointController->load(json, errorString), qPrintable(errorString));

    QCOMPARE(rallyPointController->points()->count(), 2);
    const RallyPoint* const relativePoint = rallyPointController->points()->value<RallyPoint*>(0);
    const RallyPoint* const absolutePoint = rallyPointController->points()->value<RallyPoint*>(1);
    QCOMPARE(int(relativePoint->altitudeFrame()), int(QGroundControlQmlGlobal::AltitudeFrameRelative));
    QCOMPARE(relativePoint->coordinate().altitude(), 10.0);
    QCOMPARE(int(absolutePoint->altitudeFrame()), int(QGroundControlQmlGlobal::AltitudeFrameAbsolute));
    QCOMPARE(absolutePoint->coordinate().altitude(), 500.0);
}

void PlanMasterControllerTest::_testRallyPointDefaultAltitude_data()
{
    QTest::addColumn<int>("firmwareType");
    QTest::addColumn<double>("expectedFirstAltitude");
    QTest::addColumn<double>("expectedAltitude");
    QTest::addColumn<int>("expectedFrame");

    const double missionAltitude =
        SettingsManager::instance()->appSettings()->defaultMissionItemAltitude()->rawValue().toDouble();

    // ArduPilot altitudes are flight altitudes: the first point uses the default mission altitude and the next
    // point starts from the previous one
    QTest::newRow("ArduPilot first point uses the default mission altitude, then copies the previous")
        << int(MAV_AUTOPILOT_ARDUPILOTMEGA) << missionAltitude << 77.0
        << int(QGroundControlQmlGlobal::AltitudeFrameAbsolute);
    // PX4 altitudes are ground heights that differ per point, so the previous value must not be reused
    QTest::newRow("PX4 starts every point from the default")
        << int(MAV_AUTOPILOT_PX4) << RallyPoint::getDefaultFactAltitude() << RallyPoint::getDefaultFactAltitude()
        << int(QGroundControlQmlGlobal::AltitudeFrameRelative);
}

void PlanMasterControllerTest::_testRallyPointDefaultAltitude()
{
    QFETCH(int, firmwareType);
    QFETCH(double, expectedFirstAltitude);
    QFETCH(double, expectedAltitude);
    QFETCH(int, expectedFrame);

    PlanMasterController* const masterController =
        new PlanMasterController(static_cast<MAV_AUTOPILOT>(firmwareType), MAV_TYPE_QUADROTOR, this);
    RallyPointController* const rallyPointController = masterController->rallyPointController();
    const QGeoCoordinate coordinate(47.3977, 8.5456);

    rallyPointController->addPoint(coordinate);
    RallyPoint* const firstPoint = rallyPointController->points()->value<RallyPoint*>(0);
    QVERIFY(firstPoint);
    QCOMPARE(firstPoint->coordinate().altitude(), expectedFirstAltitude);
    firstPoint->setAltitudeFrame(QGroundControlQmlGlobal::AltitudeFrameAbsolute);
    firstPoint->setCoordinate(QGeoCoordinate(coordinate.latitude(), coordinate.longitude(), 77.0));

    rallyPointController->addPoint(coordinate.atDistanceAndAzimuth(100, 0));

    const RallyPoint* const secondPoint = rallyPointController->points()->value<RallyPoint*>(1);
    QVERIFY(secondPoint);
    QCOMPARE(secondPoint->coordinate().altitude(), expectedAltitude);
    QCOMPARE(int(secondPoint->altitudeFrame()), expectedFrame);

    delete masterController;
}

void PlanMasterControllerTest::_testRallyPointFramesSurviveVehicleRoundTrip()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), 10000);

    RallyPointManager* const rallyPointManager = _vehicle->rallyPointManager();
    QSignalSpy sendSpy(rallyPointManager, &RallyPointManager::sendComplete);
    QSignalSpy loadSpy(rallyPointManager, &RallyPointManager::loadComplete);

    const QList<RallyPointData> sentPoints = {
        {QGeoCoordinate(47.1, 8.1, 10), MAV_FRAME_GLOBAL_RELATIVE_ALT},
        {QGeoCoordinate(47.2, 8.2, 500), MAV_FRAME_GLOBAL},
    };
    rallyPointManager->sendToVehicle(sentPoints);
    QTRY_COMPARE_WITH_TIMEOUT(sendSpy.count(), 1, 10000);
    QVERIFY(!sendSpy.first().at(0).toBool());

    loadSpy.clear();
    rallyPointManager->loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!loadSpy.isEmpty(), 10000);

    const QList<RallyPointData> loadedPoints = rallyPointManager->points();
    QCOMPARE(loadedPoints.count(), sentPoints.count());
    for (int i = 0; i < sentPoints.count(); i++) {
        QCOMPARE(loadedPoints[i].coordinate.altitude(), sentPoints[i].coordinate.altitude());
        QCOMPARE(int(loadedPoints[i].frame), int(sentPoints[i].frame));
    }
}

void PlanMasterControllerTest::_testPendingRallyTerrainBlocksSaveAndUpload()
{
    RallyPointController* const rallyPointController = _masterController->rallyPointController();
    QCOMPARE(_masterController->readyForSaveState(), int(VisualMissionItem::ReadyForSave));

    rallyPointController->addPoint(QGeoCoordinate(47.3977, 8.5456));
    RallyPoint* const rallyPoint = rallyPointController->points()->value<RallyPoint*>(0);
    QVERIFY(rallyPoint);
    rallyPoint->useTerrainAltitude();
    QCOMPARE(_masterController->readyForSaveState(), int(VisualMissionItem::NotReadyForSaveTerrain));

    rallyPoint->altitude()->setRawValue(50.0);
    QCOMPARE(_masterController->readyForSaveState(), int(VisualMissionItem::ReadyForSave));
}

void PlanMasterControllerTest::_testPlanVehicleIdOfflineAndNotify()
{
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    Vehicle* const helper = _masterController->controllerVehicle();
    QVERIFY(helper);
    QVERIFY(helper->isOfflineEditingVehicle());
    QCOMPARE(_masterController->managerVehicle(), helper);

    const QMetaObject* const metaObject = _masterController->metaObject();
    const QMetaProperty property = metaObject->property(metaObject->indexOfProperty("planVehicleId"));
    QVERIFY(property.isValid());
    QVERIFY(!property.isWritable());
    QVERIFY(property.hasNotifySignal());
    QCOMPARE(property.metaType(), QMetaType::fromType<quint32>());

    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    _masterController->removeAll();
    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(changed.count(), 0);

    const quint32 fullWidthId = std::numeric_limits<quint32>::max();
    _masterController->_setPlanVehicleId(fullWidthId);
    QCOMPARE(property.read(_masterController).toULongLong(), qulonglong(fullWidthId));
    QCOMPARE(changed.count(), 1);
    _masterController->_setPlanVehicleId(fullWidthId);
    QCOMPARE(changed.count(), 1);
    _masterController->removeAll();
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    QCOMPARE(changed.count(), 2);
    _masterController->removeAll();
    QCOMPARE(changed.count(), 2);
}

void PlanMasterControllerTest::_testPlanVehicleIdInitialAdoption()
{
    PlanMasterController retained;
    QSignalSpy retainedChanged(&retained, &PlanMasterController::planVehicleIdChanged);
    QObject observer;
    int startedSections = 0;
    QList<quint32> idsDuringTransfer;
    connect(_masterController, &PlanMasterController::managerVehicleChanged, &observer, [&](Vehicle* vehicle) {
        if (!vehicle || vehicle->isOfflineEditingVehicle()) {
            return;
        }
        retained.startStaticActiveVehicle(vehicle);
        QCOMPARE(retained.planVehicleId(), quint32(0));
        const QList<PlanManager*> managers = {vehicle->missionManager(), vehicle->geoFenceManager(),
                                              vehicle->rallyPointManager()};
        for (int i = 0; i < managers.count(); i++) {
            connect(managers[i], &PlanManager::inProgressChanged, &observer, [&, i](bool inProgress) {
                if (inProgress) {
                    startedSections |= 1 << i;
                    idsDuringTransfer.append(_masterController->planVehicleId());
                    idsDuringTransfer.append(retained.planVehicleId());
                }
            });
        }
    });
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    Vehicle* const vehicle = _connectExtraVehicle(true);
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(_masterController->planVehicleId(), vehicle->id(), TestTimeout::mediumMs());
    QCOMPARE(startedSections, 7);
    for (const quint32 vehicleId : std::as_const(idsDuringTransfer)) {
        QCOMPARE(vehicleId, quint32(0));
    }
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 4);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(retained.planVehicleId(), vehicle->id());
    QCOMPARE(retained.missionController()->visualItems()->count(), 4);
    QCOMPARE(retainedChanged.count(), 1);

    changed.clear();
    _masterController->showPlanFromManagerVehicle();
    _masterController->_setPlanVehicleId(vehicle->id());
    QCOMPARE(_masterController->planVehicleId(), vehicle->id());
    QCOMPARE(changed.count(), 0);
}

void PlanMasterControllerTest::_testPlanVehicleIdKeepAndLoad()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_COMPARE_WITH_TIMEOUT(_masterController->planVehicleId(), _vehicle->id(), TestTimeout::longMs());
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    MissionController* const mission = _masterController->missionController();
    const QGeoCoordinate keptHome = mission->plannedHomePosition();
    QVERIFY(mission->insertSimpleMissionItem(keptHome.atDistanceAndAzimuth(200, 45), -1));
    QVERIFY(_masterController->dirtyForSave());
    const int keptCount = _masterController->missionController()->visualItems()->count();
    const quint32 keptId = _vehicle->id();
    QList<QGeoCoordinate> keptCoordinates;
    for (int i = 0; i < keptCount; i++) {
        keptCoordinates.append(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate());
    }

    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const otherVehicle = _connectExtraVehicle(true);
    verifyExpectedLogMessage();
    QVERIFY(otherVehicle);
    QTRY_VERIFY_WITH_TIMEOUT(otherVehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    QSignalSpy prompted(_masterController, &PlanMasterController::promptForPlanUsageOnVehicleChange);
    Vehicle* managerAtPrompt = nullptr;
    quint32 idAtPrompt = 0;
    bool dirtyAtPrompt = false;
    QObject observer;
    connect(_masterController, &PlanMasterController::promptForPlanUsageOnVehicleChange, &observer, [&] {
        managerAtPrompt = _masterController->managerVehicle();
        idAtPrompt = _masterController->planVehicleId();
        dirtyAtPrompt = _masterController->dirtyForSave();
    });
    MultiVehicleManager::instance()->setActiveVehicle(otherVehicle);
    QCOMPARE(MultiVehicleManager::instance()->activeVehicle(), _vehicle);
    QCOMPARE(_masterController->managerVehicle(), _vehicle);
    QCOMPARE(prompted.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), otherVehicle, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(_masterController->managerVehicle(), otherVehicle, TestTimeout::mediumMs());
    QCOMPARE(prompted.count(), 1);
    QCOMPARE(managerAtPrompt, otherVehicle);
    QCOMPARE(idAtPrompt, keptId);
    QVERIFY(dirtyAtPrompt);
    // Keep Current Plan closes the prompt without calling the controller.
    QCOMPARE(_masterController->planVehicleId(), keptId);
    QCOMPARE(_masterController->missionController()->visualItems()->count(), keptCount);
    QCOMPARE(mission->plannedHomePosition(), keptHome);
    for (int i = 0; i < keptCount; i++) {
        QCOMPARE(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate(), keptCoordinates[i]);
    }
    QVERIFY(_masterController->dirtyForSave());
    QVERIFY(_masterController->dirtyForUpload());
    QCOMPARE(changed.count(), 0);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(_masterController->saveToFile(directory.filePath(QStringLiteral("kept.plan"))));
    QVERIFY(!_masterController->dirtyForSave());
    QVERIFY(_masterController->dirtyForUpload());
    QCOMPARE(_masterController->planVehicleId(), keptId);
    QCOMPARE(changed.count(), 0);

    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(_masterController->planVehicleId(), otherVehicle->id());
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 4);
    QVERIFY(!_masterController->dirtyForSave());
    QVERIFY(!_masterController->dirtyForUpload());
    QCOMPARE(changed.count(), 2);
    changed.clear();
    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(changed.count(), 0);

    const QGeoCoordinate loadedHome = mission->plannedHomePosition();
    QVERIFY(mission->insertSimpleMissionItem(loadedHome.atDistanceAndAzimuth(200, 45), -1));
    QVERIFY(_masterController->dirtyForSave());
    MultiVehicleManager::instance()->setActiveVehicle(_vehicle);
    QCOMPARE(MultiVehicleManager::instance()->activeVehicle(), otherVehicle);
    QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), _vehicle, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(_masterController->managerVehicle(), _vehicle, TestTimeout::mediumMs());
    QCOMPARE(prompted.count(), 2);
    QCOMPARE(managerAtPrompt, _vehicle);
    QCOMPARE(idAtPrompt, otherVehicle->id());
    QVERIFY(dirtyAtPrompt);
    QCOMPARE(_masterController->planVehicleId(), otherVehicle->id());
    QCOMPARE(mission->visualItems()->count(), 5);
    QCOMPARE(mission->plannedHomePosition(), loadedHome);
    QCOMPARE(changed.count(), 0);

    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(_masterController->planVehicleId(), keptId);
    QCOMPARE(mission->visualItems()->count(), 1);
    QVERIFY(!_masterController->dirtyForSave());
    QVERIFY(!_masterController->dirtyForUpload());
    QCOMPARE(changed.count(), 2);
}

void PlanMasterControllerTest::_testPlanVehicleIdDirtyDisconnectKeepsAssociation()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_COMPARE_WITH_TIMEOUT(_masterController->planVehicleId(), _vehicle->id(), TestTimeout::longMs());
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(_masterController->missionController()->insertSimpleMissionItem(
        _masterController->missionController()->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1));
    const quint32 vehicleId = _vehicle->id();
    const int count = _masterController->missionController()->visualItems()->count();
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    _disconnectMockLink();
    QVERIFY(_masterController->offline());
    QCOMPARE(_masterController->managerVehicle(), _masterController->controllerVehicle());
    QCOMPARE(_masterController->planVehicleId(), vehicleId);
    QCOMPARE(_masterController->missionController()->visualItems()->count(), count);
    QCOMPARE(changed.count(), 0);
    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    QCOMPARE(changed.count(), 1);
}

void PlanMasterControllerTest::_testPlanVehicleIdExplicitDownloads()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    QVERIFY(_mockLink);
    _mockLink->loadSimpleMultirotorMission();
    _masterController->removeAll();
    QSignalSpy firstComplete(_vehicle->rallyPointManager(), &RallyPointManager::loadComplete);
    _masterController->loadFromVehicle();
    QVERIFY(_masterController->syncInProgress());
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    QTRY_COMPARE_WITH_TIMEOUT(firstComplete.count(), 1, TestTimeout::longMs());
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 4);

    QVERIFY(_masterController->missionController()->insertSimpleMissionItem(
        _masterController->missionController()->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1));
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const otherVehicle = _connectExtraVehicle(true);
    verifyExpectedLogMessage();
    QVERIFY(otherVehicle);
    QTRY_VERIFY_WITH_TIMEOUT(otherVehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->_activeVehicleChanged(otherVehicle);
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
    QObject observer;
    quint32 idAfterMissionReplacement = std::numeric_limits<quint32>::max();
    connect(_masterController->missionController(), &MissionController::newItemsFromVehicle, &observer,
            [&] { idAfterMissionReplacement = _masterController->planVehicleId(); });
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    QSignalSpy complete(otherVehicle->rallyPointManager(), &RallyPointManager::loadComplete);
    _masterController->loadFromVehicle();
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
    QCOMPARE(idAfterMissionReplacement, quint32(0));
    QCOMPARE(changed.count(), 2);
    QCOMPARE(_masterController->planVehicleId(), otherVehicle->id());
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 4);
    QCOMPARE(MultiVehicleManager::instance()->activeVehicle(), _vehicle);
}

void PlanMasterControllerTest::_testPlanVehicleIdEditDuringDownload_data()
{
    QTest::addColumn<int>("editAfterSection");
    QTest::newRow("edit before mission replacement is discarded") << -1;
    QTest::newRow("edit after mission adoption leaves mixture unattached") << 0;
    QTest::newRow("edit after fence adoption leaves mixture unattached") << 1;
}

void PlanMasterControllerTest::_testPlanVehicleIdEditDuringDownload()
{
    QFETCH(int, editAfterSection);
    AppSettings* const settings = SettingsManager::instance()->appSettings();
    settings->offlineEditingFirmwareClass()->setRawValue(int(QGCMAVLink::FirmwareClassPX4));
    settings->offlineEditingVehicleClass()->setRawValue(int(QGCMAVLink::VehicleClassMultiRotor));
    MissionController* const mission = _masterController->missionController();
    GeoFenceController* const fence = _masterController->geoFenceController();
    RallyPointController* const rally = _masterController->rallyPointController();

    // Keep an edited offline buffer through the initial connection, then explicitly replace it with A's plan.
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(mission->insertSimpleMissionItem(mission->plannedHomePosition().atDistanceAndAzimuth(100, 45), -1));
    QVERIFY(_masterController->dirtyForSave());
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QVERIFY(_mockLink);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    QCOMPARE(_masterController->managerVehicle(), _vehicle);
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    QCOMPARE(mission->visualItems()->count(), 7);
    QVERIFY(fence->supported());
    QVERIFY(rally->supported());
    QVERIFY(!_masterController->syncInProgress());
    _mockLink->loadSimpleMultirotorMission();

    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    QSignalSpy missionComplete(mission, &MissionController::newItemsFromVehicle);
    QSignalSpy fenceComplete(fence, &GeoFenceController::loadComplete);
    QSignalSpy complete(_vehicle->rallyPointManager(), &RallyPointManager::loadComplete);
    QList<quint32> notifiedIds;
    QGeoCoordinate editedCoordinate;
    int editCount = 0;
    const auto insertEdit = [&] {
        VisualMissionItem* const item =
            mission->insertSimpleMissionItem(mission->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1);
        QVERIFY(item);
        editedCoordinate = item->coordinate();
        ++editCount;
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
        QCOMPARE(changed.count(), 0);
    };
    QObject observer;
    connect(_masterController, &PlanMasterController::planVehicleIdChanged, &observer,
            [&] { notifiedIds.append(_masterController->planVehicleId()); });
    // The master's earlier direct connections have started the next section before these callbacks run.
    connect(mission, &MissionController::newItemsFromVehicle, &observer, [&] {
        QCOMPARE(mission->visualItems()->count(), 4);
        QVERIFY(fence->syncInProgress());
        QCOMPARE(complete.count(), 0);
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
        QCOMPARE(changed.count(), 0);
        if (editAfterSection == 0) {
            insertEdit();
        }
    });
    connect(fence, &GeoFenceController::loadComplete, &observer, [&] {
        QVERIFY(rally->syncInProgress());
        QCOMPARE(complete.count(), 0);
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
        QCOMPARE(changed.count(), 0);
        if (editAfterSection == 1) {
            insertEdit();
        }
    });

    _masterController->loadFromVehicle();
    QVERIFY(mission->syncInProgress());
    QCOMPARE(missionComplete.count(), 0);
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    QCOMPARE(changed.count(), 0);
    if (editAfterSection == -1) {
        insertEdit();
        QCOMPARE(mission->visualItems()->count(), 8);
    }

    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
    QCOMPARE(missionComplete.count(), 1);
    QCOMPARE(fenceComplete.count(), 1);
    QVERIFY(!_masterController->syncInProgress());
    QCOMPARE(editCount, 1);
    QCOMPARE(_masterController->planVehicleId(), editAfterSection == -1 ? _vehicle->id() : quint32(0));
    QCOMPARE(changed.count(), editAfterSection == -1 ? 1 : 0);
    QCOMPARE(notifiedIds, editAfterSection == -1 ? QList<quint32>{_vehicle->id()} : QList<quint32>{});
    QCOMPARE(mission->visualItems()->count(), editAfterSection == -1 ? 4 : 5);
    if (editAfterSection == -1) {
        for (int i = 0; i < mission->visualItems()->count(); ++i) {
            QVERIFY(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate() != editedCoordinate);
        }
    } else {
        QCOMPARE(mission->visualItems()->value<VisualMissionItem*>(4)->coordinate(), editedCoordinate);
    }
}

void PlanMasterControllerTest::_testPlanVehicleIdCachedShowWaitsForReplacement()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(_masterController->missionController()->insertSimpleMissionItem(
        _masterController->missionController()->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1));
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const target = _connectExtraVehicle(true);
    verifyExpectedLogMessage();
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->_activeVehicleChanged(target);
    target->missionManager()->loadFromVehicle();
    QVERIFY(target->missionManager()->inProgress());
    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(_masterController->planVehicleId(), target->id(), TestTimeout::longMs());
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 4);
}

void PlanMasterControllerTest::_testPlanVehicleIdPartialSectionAdoption_data()
{
    QTest::addColumn<bool>("fence");
    QTest::newRow("fence replacement leaves mixed content unattached") << true;
    QTest::newRow("rally replacement leaves mixed content unattached") << false;
}

void PlanMasterControllerTest::_testPlanVehicleIdPartialSectionAdoption()
{
    QFETCH(bool, fence);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _populatePlanForIdentityTransfer();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const target = _connectExtraVehicle();
    verifyExpectedLogMessage();
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());
    GeoFenceController* const geoFence = _masterController->geoFenceController();
    RallyPointController* const rally = _masterController->rallyPointController();
    if (fence) {
        QSignalSpy sent(target->geoFenceManager(), &GeoFenceManager::sendComplete);
        target->geoFenceManager()->sendToVehicle(QGeoCoordinate(), *geoFence->polygons(), *geoFence->circles());
        QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, TestTimeout::longMs());
    } else {
        QSignalSpy sent(target->rallyPointManager(), &RallyPointManager::sendComplete);
        target->rallyPointManager()->sendToVehicle(
            {{QGeoCoordinate(47.6607, -122.1045, 10), MAV_FRAME_GLOBAL_RELATIVE_ALT}});
        QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, TestTimeout::longMs());
    }
    geoFence->removeAll();
    rally->removeAll();
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(_masterController->missionController()->insertSimpleMissionItem(
        _masterController->missionController()->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1));
    _masterController->_activeVehicleChanged(target);
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
    if (fence) {
        QSignalSpy loaded(geoFence, &GeoFenceController::loadComplete);
        geoFence->loadFromVehicle();
        QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, TestTimeout::longMs());
        QCOMPARE(geoFence->circles()->count(), 1);
    } else {
        QSignalSpy loaded(rally, &RallyPointController::loadComplete);
        rally->loadFromVehicle();
        QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, TestTimeout::longMs());
        QCOMPARE(rally->points()->count(), 1);
    }
    QCOMPARE(_masterController->missionController()->visualItems()->count(), 7);
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    _masterController->showPlanFromManagerVehicle();
    QCOMPARE(_masterController->planVehicleId(), target->id());
}

void PlanMasterControllerTest::_testPlanVehicleIdStaleDownloadKeepsAssociation()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(_masterController->missionController()->insertSimpleMissionItem(
        _masterController->missionController()->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1));
    const int count = _masterController->missionController()->visualItems()->count();
    const quint32 vehicleId = _vehicle->id();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const otherVehicle = _connectExtraVehicle(true);
    verifyExpectedLogMessage();
    QVERIFY(otherVehicle);
    QTRY_VERIFY_WITH_TIMEOUT(otherVehicle->initialPlanRequestComplete(), TestTimeout::longMs());

    QSignalSpy oldComplete(_vehicle->missionManager(), &MissionManager::newMissionItemsAvailable);
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    _masterController->loadFromVehicle();
    QVERIFY(_vehicle->missionManager()->inProgress());
    _masterController->_activeVehicleChanged(otherVehicle);
    QTRY_COMPARE_WITH_TIMEOUT(oldComplete.count(), 1, TestTimeout::longMs());
    QCOMPARE(_masterController->planVehicleId(), vehicleId);
    QCOMPARE(_masterController->missionController()->visualItems()->count(), count);
    QCOMPARE(changed.count(), 0);
}

void PlanMasterControllerTest::_testPlanVehicleIdPendingDownloadDisconnectKeepsAssociation()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QVERIFY(_mockLink);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QVERIFY(_masterController->missionController()->insertSimpleMissionItem(
        _masterController->missionController()->plannedHomePosition().atDistanceAndAzimuth(200, 45), -1));
    const quint32 vehicleId = _vehicle->id();
    const int count = _masterController->missionController()->visualItems()->count();
    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailReadRequestListNoResponse, MAV_MISSION_ERROR);
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    _masterController->loadFromVehicle();
    QVERIFY(_masterController->syncInProgress());
    _disconnectMockLink();
    QVERIFY(_masterController->offline());
    QCOMPARE(_masterController->planVehicleId(), vehicleId);
    QCOMPARE(_masterController->missionController()->visualItems()->count(), count);
    QCOMPARE(changed.count(), 0);
}

void PlanMasterControllerTest::_testPlanVehicleIdFileLoad_data()
{
    QTest::addColumn<bool>("online");
    QTest::newRow("offline") << false;
    QTest::newRow("real target") << true;
}

void PlanMasterControllerTest::_testPlanVehicleIdFileLoad()
{
    QFETCH(bool, online);
    quint32 targetId = 0;
    if (online) {
        _connectMockLink(MAV_AUTOPILOT_PX4);
        QVERIFY(_vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
        targetId = _vehicle->id();
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QCOMPARE(_masterController->planVehicleId(), targetId);
    const QString planFile = directory.filePath(QStringLiteral("saved.plan"));
    QVERIFY(_masterController->saveToFile(planFile));
    QCOMPARE(_masterController->planVehicleId(), targetId);
    QVERIFY(!_masterController->saveToJson().object().contains(QStringLiteral("planVehicleId")));
    _masterController->removeAll();
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    _masterController->loadFromFile(planFile);
    QCOMPARE(_masterController->planVehicleId(), targetId);
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Error loading Plan file")));
    _masterController->loadFromFile(directory.filePath(QStringLiteral("missing.plan")));
    verifyExpectedLogMessage();
    QCOMPARE(_masterController->planVehicleId(), targetId);

    for (const QString& section : {QStringLiteral("geoFence"), QStringLiteral("rallyPoints")}) {
        _masterController->loadFromFile(planFile);
        QCOMPARE(_masterController->planVehicleId(), targetId);
        QJsonObject json = _masterController->saveToJson().object();
        QJsonObject mission = json[QStringLiteral("mission")].toObject();
        mission[QStringLiteral("items")] = QJsonArray();
        json[QStringLiteral("mission")] = mission;
        json[section] = QJsonObject{{QStringLiteral("version"), -1}};
        const QString brokenFile = directory.filePath(QStringLiteral("partial.plan"));
        QFile file(brokenFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QJsonDocument(json).toJson()) > 0);
        file.close();
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression(QStringLiteral("Error loading Plan file")));
        _masterController->loadFromFile(brokenFile);
        verifyExpectedLogMessage();
        QCOMPARE(_masterController->missionController()->visualItems()->count(), 1);
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
    }

    for (const QString& filename : {QStringLiteral("invalid.waypoints"), QStringLiteral("invalid.plan")}) {
        _masterController->loadFromFile(planFile);
        QFile file(directory.filePath(filename));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("invalid contents") > 0);
        file.close();
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression(QStringLiteral("Error loading Plan file")));
        _masterController->loadFromFile(file.fileName());
        verifyExpectedLogMessage();
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
    }
}

void PlanMasterControllerTest::_testPlanVehicleIdNewBuffer_data()
{
    QTest::addColumn<bool>("online");
    QTest::addColumn<QString>("creation");
    for (const bool online : {false, true}) {
        for (const QString& creation : {QStringLiteral("waypoint"), QStringLiteral("fence"), QStringLiteral("rally"),
                                        QStringLiteral("template")}) {
            QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(online ? "online" : "offline", creation)))
                << online << creation;
        }
    }
}

void PlanMasterControllerTest::_testPlanVehicleIdNewBuffer()
{
    QFETCH(bool, online);
    QFETCH(QString, creation);
    quint32 targetId = 0;
    if (online) {
        _connectMockLink(MAV_AUTOPILOT_PX4);
        QVERIFY(_vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
        targetId = _vehicle->id();
    }
    _masterController->removeAll();
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    const QGeoCoordinate coordinate(47.3977, 8.5456);
    if (creation == QStringLiteral("waypoint")) {
        QVERIFY(_masterController->missionController()->insertSimpleMissionItem(coordinate, -1));
    } else if (creation == QStringLiteral("fence")) {
        _masterController->geoFenceController()->addInclusionCircle(coordinate,
                                                                    coordinate.atDistanceAndAzimuth(100, 135));
    } else if (creation == QStringLiteral("rally")) {
        _masterController->rallyPointController()->addPoint(coordinate);
    } else {
        QmlObjectListModel* const creators = _masterController->planCreators();
        QVERIFY(creators);
        PlanCreator* creator = nullptr;
        for (int i = 0; i < creators->count(); i++) {
            creator = qobject_cast<SurveyPlanCreator*>(creators->get(i));
            if (creator) {
                break;
            }
        }
        QVERIFY(creator);
        creator->createPlan(coordinate);
    }
    QVERIFY(_masterController->containsItems());
    QCOMPARE(_masterController->planVehicleId(), targetId);
    _masterController->removeAll();
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
}

void PlanMasterControllerTest::_populatePlanForIdentityTransfer()
{
    _masterController->loadFromFile(":/unittest/MissionPlanner.waypoints");
    QGeoCoordinate coordinate = _masterController->missionController()->plannedHomePosition();
    coordinate.setAltitude(50);
    _masterController->geoFenceController()->addInclusionCircle(coordinate, coordinate.atDistanceAndAzimuth(200, 135));
    RallyPointController* const rally = _masterController->rallyPointController();
    rally->points()->append(new RallyPoint(coordinate, QGroundControlQmlGlobal::AltitudeFrameRelative, rally));
}

void PlanMasterControllerTest::_testPlanVehicleIdFailedDownloadClearsAssociation_data()
{
    QTest::addColumn<int>("failureElement");
    QTest::newRow("failed mission replacement clears association") << 0;
    QTest::newRow("partial fence replacement clears association") << 1;
    QTest::newRow("partial rally replacement clears association") << 2;
}

void PlanMasterControllerTest::_testPlanVehicleIdFailedDownloadClearsAssociation()
{
    QFETCH(int, failureElement);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QVERIFY(_mockLink);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _populatePlanForIdentityTransfer();
    QSignalSpy sent(_vehicle->rallyPointManager(), &RallyPointManager::sendComplete);
    _masterController->sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(sent.count(), 1, TestTimeout::longMs());
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());

    {
        QObject observer;
        const QList<PlanManager*> managers = {_vehicle->missionManager(), _vehicle->geoFenceManager(),
                                              _vehicle->rallyPointManager()};
        for (int i = 0; i < managers.count(); i++) {
            connect(
                managers[i], &PlanManager::inProgressChanged, &observer, [this, failureElement, i](bool inProgress) {
                    if (inProgress) {
                        _mockLink->setMissionItemFailureMode(i == failureElement
                                                                 ? MockLinkMissionItemHandler::FailReadRequest0ErrorAck
                                                                 : MockLinkMissionItemHandler::FailNone,
                                                             MAV_MISSION_ERROR);
                    }
                });
        }
        const QString errorPrefix = QStringList{QStringLiteral("Mission"), QStringLiteral("GeoFence"),
                                                QStringLiteral("Rally Point")}[failureElement];
        expectLogMessage("Vehicle.Vehicle", QtWarningMsg, QRegularExpression(errorPrefix + " transfer failed"));
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression(errorPrefix + " transfer failed"));
        QSignalSpy complete(_vehicle->rallyPointManager(), &RallyPointManager::loadComplete);
        _masterController->loadFromVehicle();
        QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
        QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
        QCOMPARE(_masterController->planVehicleId(), quint32(0));
        QVERIFY(_masterController->dirtyForUpload());
        QVERIFY(!_masterController->dirtyForSave());
    }
    _mockLink->setMissionItemFailureMode(MockLinkMissionItemHandler::FailNone, MAV_MISSION_ACCEPTED);
    QSignalSpy recovered(_vehicle->rallyPointManager(), &RallyPointManager::loadComplete);
    _masterController->loadFromVehicle();
    QCOMPARE(_masterController->planVehicleId(), quint32(0));
    QTRY_COMPARE_WITH_TIMEOUT(recovered.count(), 1, TestTimeout::longMs());
    QCOMPARE(_masterController->planVehicleId(), _vehicle->id());
    QVERIFY(!_masterController->dirtyForUpload());
    QVERIFY(!_masterController->dirtyForSave());
}

void PlanMasterControllerTest::_testPlanVehicleIdUpload_data()
{
    QTest::addColumn<int>("failureElement");
    QTest::newRow("completed upload adopts target") << -1;
    QTest::newRow("failed mission upload keeps source") << 0;
    QTest::newRow("failed fence upload keeps source") << 1;
    QTest::newRow("failed rally upload keeps source") << 2;
}

void PlanMasterControllerTest::_testPlanVehicleIdUpload()
{
    QFETCH(int, failureElement);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _populatePlanForIdentityTransfer();
    const quint32 sourceId = _vehicle->id();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const target = _connectExtraVehicle();
    verifyExpectedLogMessage();
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->_activeVehicleChanged(target);
    QCOMPARE(_masterController->planVehicleId(), sourceId);
    MockLink* const link = _extraLinks.last();
    QVERIFY(link);
    QObject observer;
    const QList<PlanManager*> managers = {target->missionManager(), target->geoFenceManager(),
                                          target->rallyPointManager()};
    for (int i = 0; i < managers.count(); i++) {
        connect(managers[i], &PlanManager::inProgressChanged, &observer, [link, failureElement, i](bool inProgress) {
            if (inProgress) {
                link->setMissionItemFailureMode(i == failureElement
                                                    ? MockLinkMissionItemHandler::FailWriteFinalAckErrorAck
                                                    : MockLinkMissionItemHandler::FailNone,
                                                MAV_MISSION_ERROR);
            }
        });
    }
    if (failureElement >= 0) {
        const QString errorPrefix = QStringList{QStringLiteral("Mission"), QStringLiteral("GeoFence"),
                                                QStringLiteral("Rally Point")}[failureElement];
        expectLogMessage("Vehicle.Vehicle", QtWarningMsg, QRegularExpression(errorPrefix + " transfer failed"));
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression(errorPrefix + " transfer failed"));
    }
    QSignalSpy changed(_masterController, &PlanMasterController::planVehicleIdChanged);
    QSignalSpy complete(target->rallyPointManager(), &RallyPointManager::sendComplete);
    _masterController->sendToVehicle();
    QCOMPARE(_masterController->planVehicleId(), sourceId);
    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
    if (failureElement >= 0) {
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }
    QCOMPARE(_masterController->planVehicleId(), failureElement < 0 ? target->id() : sourceId);
    QCOMPARE(_masterController->dirtyForUpload(), failureElement >= 0);
    QCOMPARE(changed.count(), failureElement < 0 ? 1 : 0);
    QCOMPARE(MultiVehicleManager::instance()->activeVehicle(), _vehicle);
}

void PlanMasterControllerTest::_testPlanVehicleIdUploadRetryAfterFailure_data()
{
    QTest::addColumn<int>("failureElement");
    QTest::newRow("failed mission upload then retry") << 0;
    QTest::newRow("failed fence upload then retry") << 1;
    QTest::newRow("failed rally upload then retry") << 2;
}

void PlanMasterControllerTest::_testPlanVehicleIdUploadRetryAfterFailure()
{
    QFETCH(int, failureElement);
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    _populatePlanForIdentityTransfer();
    const quint32 sourceId = _vehicle->id();
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const target = _connectExtraVehicle();
    verifyExpectedLogMessage();
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());
    _masterController->_activeVehicleChanged(target);
    MockLink* const link = _extraLinks.last();
    QVERIFY(link);
    QObject observer;
    const QList<PlanManager*> managers = {target->missionManager(), target->geoFenceManager(),
                                          target->rallyPointManager()};
    for (int i = 0; i < managers.count(); i++) {
        connect(managers[i], &PlanManager::inProgressChanged, &observer, [link, &failureElement, i](bool inProgress) {
            if (inProgress) {
                link->setMissionItemFailureMode(i == failureElement
                                                    ? MockLinkMissionItemHandler::FailWriteFinalAckErrorAck
                                                    : MockLinkMissionItemHandler::FailNone,
                                                MAV_MISSION_ERROR);
            }
        });
    }
    const QString errorPrefix = QStringList{QStringLiteral("Mission"), QStringLiteral("GeoFence"),
                                            QStringLiteral("Rally Point")}[failureElement];
    expectLogMessage("Vehicle.Vehicle", QtWarningMsg, QRegularExpression(errorPrefix + " transfer failed"));
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg, QRegularExpression(errorPrefix + " transfer failed"));
    QSignalSpy complete(target->rallyPointManager(), &RallyPointManager::sendComplete);
    _masterController->sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 1, TestTimeout::longMs());
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();
    QCOMPARE(_masterController->dirtyForUpload(), true);
    QCOMPARE(_masterController->planVehicleId(), sourceId);

    failureElement = -1;
    _masterController->sendToVehicle();
    QTRY_COMPARE_WITH_TIMEOUT(complete.count(), 2, TestTimeout::longMs());
    QCOMPARE(_masterController->dirtyForUpload(), false);
    QCOMPARE(_masterController->planVehicleId(), target->id());
}

void PlanMasterControllerTest::_testPlanVehicleIdStaticVehicle()
{
    _connectMockLink(MAV_AUTOPILOT_PX4);
    QVERIFY(_vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(_vehicle->initialPlanRequestComplete(), TestTimeout::longMs());
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    Vehicle* const target = _connectExtraVehicle(true);
    verifyExpectedLogMessage();
    QVERIFY(target);
    QTRY_VERIFY_WITH_TIMEOUT(target->initialPlanRequestComplete(), TestTimeout::longMs());
    PlanMasterController retained;
    QSignalSpy changed(&retained, &PlanMasterController::planVehicleIdChanged);
    retained.startStaticActiveVehicle(target);
    QCOMPARE(retained.planVehicleId(), target->id());
    QCOMPARE(retained.missionController()->visualItems()->count(), 4);
    QCOMPARE(changed.count(), 1);
    MultiVehicleManager::instance()->setActiveVehicle(target);
    QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), target, TestTimeout::mediumMs());
    MultiVehicleManager::instance()->setActiveVehicle(_vehicle);
    QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), _vehicle, TestTimeout::mediumMs());
    QCOMPARE(retained.planVehicleId(), target->id());
    QCOMPARE(changed.count(), 1);
    retained.removeAll();
    QCOMPARE(retained.planVehicleId(), quint32(0));
    retained.showPlanFromManagerVehicle();
    QCOMPARE(retained.planVehicleId(), target->id());
    QPointer<Vehicle> guardedTarget(target);
    MockLink* const link = _extraLinks.last();
    QVERIFY(link);
    link->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(guardedTarget.isNull(), TestTimeout::longMs());
    QVERIFY(retained.offline());
    QCOMPARE(retained.planVehicleId(), quint32(0));
}

#include "UnitTest.h"

UT_REGISTER_TEST(PlanMasterControllerTest, TestLabel::Integration, TestLabel::MissionManager)
