#include "MissionManagerTest.h"

#include <iterator>

#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>

#include "FirmwarePlugin.h"
#include "MissionManager.h"
#include "MultiSignalSpy.h"
#include "UnitTestCoords.h"
#include "Vehicle.h"
const MissionManagerTest::TestCase_t MissionManagerTest::_rgTestCases[] = {
    {"0\t0\t3\t16\t10\t20\t30\t40\t-10\t-20\t-30\t1\r\n",
     {0, QGeoCoordinate(-10.0, -20.0, -30.0), MAV_CMD_NAV_WAYPOINT, 10.0, 20.0, 30.0, 40.0, true, false,
      MAV_FRAME_GLOBAL_RELATIVE_ALT}},
    {"1\t0\t3\t17\t10\t20\t30\t40\t-10\t-20\t-30\t1\r\n",
     {1, QGeoCoordinate(-10.0, -20.0, -30.0), MAV_CMD_NAV_LOITER_UNLIM, 10.0, 20.0, 30.0, 40.0, true, false,
      MAV_FRAME_GLOBAL_RELATIVE_ALT}},
    {"2\t0\t3\t18\t10\t20\t30\t40\t-10\t-20\t-30\t1\r\n",
     {2, QGeoCoordinate(-10.0, -20.0, -30.0), MAV_CMD_NAV_LOITER_TURNS, 10.0, 20.0, 30.0, 40.0, true, false,
      MAV_FRAME_GLOBAL_RELATIVE_ALT}},
    {"3\t0\t3\t19\t10\t20\t30\t40\t-10\t-20\t-30\t1\r\n",
     {3, QGeoCoordinate(-10.0, -20.0, -30.0), MAV_CMD_NAV_LOITER_TIME, 10.0, 20.0, 30.0, 40.0, true, false,
      MAV_FRAME_GLOBAL_RELATIVE_ALT}},
    {"4\t0\t3\t21\t10\t20\t30\t40\t-10\t-20\t-30\t1\r\n",
     {4, QGeoCoordinate(-10.0, -20.0, -30.0), MAV_CMD_NAV_LAND, 10.0, 20.0, 30.0, 40.0, true, false,
      MAV_FRAME_GLOBAL_RELATIVE_ALT}},
    {"6\t0\t2\t112\t10\t20\t30\t40\t-10\t-20\t-30\t1\r\n",
     {5, QGeoCoordinate(-10.0, -20.0, -30.0), MAV_CMD_CONDITION_DELAY, 10.0, 20.0, 30.0, 40.0, true, false,
      MAV_FRAME_MISSION}},
};
const size_t MissionManagerTest::_cTestCases = sizeof(_rgTestCases) / sizeof(_rgTestCases[0]);

void MissionManagerTest::init()
{
    MissionControllerManagerTest::init();
    // All failure-handling tests deliberately trigger mission transfer failures which
    // cause showAppMessage() debug logs and a warning. Ignore them for the whole fixture.
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("Mission transfer failed"));
    ignoreLogMessage("Vehicle.Vehicle", QtWarningMsg, QRegularExpression("Mission transfer failed"));
    // ArduPilot metadata includes an invalid enum value for RTL_CONE_SLOPE; skip warning is expected.
    ignoreLogMessage("FirmwarePlugin.ParameterMetaData", QtWarningMsg,
                     QRegularExpression("Skipping invalid enum value"));
}

void MissionManagerTest::_writeItems(MockLinkMissionItemHandler::FailureMode_t failureMode,
                                     MAV_MISSION_RESULT failureAckResult, bool shouldFail, int expectedErrorCode)
{
    _mockLink->setMissionItemFailureMode(failureMode, failureAckResult);
    // Setup our test case data
    QList<MissionItem*> missionItems;
    // Editor has a home position item on the front, so we do the same
    MissionItem* homeItem = new MissionItem(nullptr /* Vehicle */, this);
    homeItem->setCommand(MAV_CMD_NAV_WAYPOINT);
    homeItem->setParam5(47.3769);
    homeItem->setParam6(8.549444);
    homeItem->setParam7(0);
    homeItem->setSequenceNumber(0);
    missionItems.append(homeItem);
    for (size_t i = 0; i < _cTestCases; i++) {
        const TestCase_t* testCase = &_rgTestCases[i];
        MissionItem* missionItem = new MissionItem(this);
        QTextStream loadStream(testCase->itemStream, QIODevice::ReadOnly);
        QVERIFY(missionItem->load(loadStream));
        // Mission Manager expects to get 1-base sequence numbers for write
        missionItem->setSequenceNumber(missionItem->sequenceNumber() + 1);
        missionItems.append(missionItem);
    }
    // Send the items to the vehicle
    _missionManager->writeMissionItems(missionItems);
    // writeMissionItems should emit these signals before returning:
    //      inProgressChanged
    QVERIFY(_missionManager->inProgress());
    QVERIFY(_multiSpyMissionManager->emittedOnce("inProgressChanged"));
    _checkInProgressValues(true);
    _multiSpyMissionManager->clearAllSignals();
    if (shouldFail) {
        // This should be a failed run
        // Wait for write sequence to complete. We should get:
        //      inProgressChanged(false) signal
        //      error(errorCode, QString) signal
        QVERIFY_WAIT_SIGNAL((*_multiSpyMissionManager), "inProgressChanged", _missionManagerSignalWaitTime);
        QVERIFY(_multiSpyMissionManager->emitted("inProgressChanged", "error"));
        // Validate inProgressChanged signal value
        _checkInProgressValues(false);
        // Validate error signal values
        QSignalSpy* spy = _multiSpyMissionManager->spy("error");
        QList<QVariant> signalArgs = spy->takeFirst();
        QCOMPARE(signalArgs.count(), 2);
        if (expectedErrorCode >= 0) {
            QCOMPARE(signalArgs[0].toInt(), expectedErrorCode);
        }
        qCDebug(UnitTestLog) << signalArgs[1].toString();
    } else {
        // This should be clean run
        // Wait for write sequence to complete. We should get:
        //      inProgressChanged(false) signal
        //      sendComplete signal
        QVERIFY_WAIT_SIGNAL((*_multiSpyMissionManager), "sendComplete", _missionManagerSignalWaitTime);
        QVERIFY(_multiSpyMissionManager->emitted("inProgressChanged", "sendComplete"));
        // sendComplete is also emitted on failure, with its error argument set
        QVERIFY(_multiSpyMissionManager->notEmitted("error"));
        // Validate inProgressChanged signal value
        _checkInProgressValues(false);
        // Validate item count in mission manager
        int expectedCount = (int)_cTestCases;
        if (_mockLink->getFirmwareType() == MAV_AUTOPILOT_ARDUPILOTMEGA) {
            // Home position at position 0 comes from vehicle
            expectedCount++;
        }
        QCOMPARE(_missionManager->missionItems().count(), expectedCount);
    }
    QSignalSpy* sendCompleteSpy = _multiSpyMissionManager->spy("sendComplete");
    QCOMPARE(sendCompleteSpy->count(), 1);
    QCOMPARE(sendCompleteSpy->first().at(0).toBool(), shouldFail);
    // Two ACK intervals catch a stale timeout producing a second terminal result.
    QVERIFY_NO_SIGNAL_WAIT(*sendCompleteSpy, 2 * PlanManager::kTestAckTimeoutMs);
    QCOMPARE(sendCompleteSpy->count(), 1);
    _multiSpyMissionManager->clearAllSignals();
}

void MissionManagerTest::_roundTripItems(MockLinkMissionItemHandler::FailureMode_t failureMode,
                                         MAV_MISSION_RESULT failureAckResult, bool shouldFail)
{
    _writeItems(MockLinkMissionItemHandler::FailNone, failureAckResult, false);
    _mockLink->setMissionItemFailureMode(failureMode, failureAckResult);
    // Read the items back from the vehicle
    _missionManager->loadFromVehicle();
    // requestMissionItems should emit inProgressChanged signal before returning so no need to wait for it
    QVERIFY(_missionManager->inProgress());
    QVERIFY(_multiSpyMissionManager->onlyEmittedOnce("inProgressChanged"));
    _checkInProgressValues(true);
    _multiSpyMissionManager->clearAllSignals();
    if (shouldFail) {
        // This should be a failed run
        // Wait for read sequence to complete. We should get:
        //      inProgressChanged(false) signal to signal completion
        //      error(errorCode, QString) signal
        //      newMissionItemsAvailable signal
        QVERIFY_WAIT_SIGNAL((*_multiSpyMissionManager), "inProgressChanged", _missionManagerSignalWaitTime);
        QVERIFY(_multiSpyMissionManager->emitted("newMissionItemsAvailable", "inProgressChanged", "error"));
        // Validate inProgressChanged signal value
        _checkInProgressValues(false);
        // Validate error signal values
        QSignalSpy* spy = _multiSpyMissionManager->spy("error");
        QList<QVariant> signalArgs = spy->takeFirst();
        QCOMPARE(signalArgs.count(), 2);
        qCDebug(UnitTestLog) << signalArgs[1].toString();
    } else {
        // This should be clean run
        // Now wait for read sequence to complete. We should get:
        //      inProgressChanged(false) signal to signal completion
        //      newMissionItemsAvailable signal
        QVERIFY_WAIT_SIGNAL((*_multiSpyMissionManager), "inProgressChanged", _missionManagerSignalWaitTime);
        QVERIFY(_multiSpyMissionManager->emitted("newMissionItemsAvailable", "inProgressChanged"));
        _checkInProgressValues(false);
    }
    _multiSpyMissionManager->clearAllSignals();
    // Validate returned items
    int cMissionItemsExpected;
    if (shouldFail) {
        cMissionItemsExpected = 0;
    } else {
        cMissionItemsExpected = (int)_cTestCases;
        if (_mockLink->getFirmwareType() == MAV_AUTOPILOT_ARDUPILOTMEGA) {
            // Home position at position 0 comes from vehicle
            cMissionItemsExpected++;
        }
    }
    QCOMPARE(_missionManager->missionItems().count(), (int)cMissionItemsExpected);
    int firstActualItem = 0;
    if (_mockLink->getFirmwareType() == MAV_AUTOPILOT_ARDUPILOTMEGA) {
        // First item is home position, don't validate it
        firstActualItem++;
    }
    int testCaseIndex = 0;
    for (int actualItemIndex = firstActualItem; actualItemIndex < cMissionItemsExpected; actualItemIndex++) {
        const TestCase_t* testCase = &_rgTestCases[testCaseIndex];
        int expectedSequenceNumber = testCase->expectedItem.sequenceNumber;
        if (_mockLink->getFirmwareType() == MAV_AUTOPILOT_ARDUPILOTMEGA) {
            // Account for home position in first item
            expectedSequenceNumber++;
        }
        MissionItem* actual = _missionManager->missionItems()[actualItemIndex];
        qCDebug(UnitTestLog) << "Test case" << testCaseIndex;
        QCOMPARE(actual->sequenceNumber(), expectedSequenceNumber);
        QCOMPARE(actual->coordinate().latitude(), testCase->expectedItem.coordinate.latitude());
        QCOMPARE(actual->coordinate().longitude(), testCase->expectedItem.coordinate.longitude());
        QCOMPARE(actual->coordinate().altitude(), testCase->expectedItem.coordinate.altitude());
        QCOMPARE((int)actual->command(), (int)testCase->expectedItem.command);
        QCOMPARE(actual->param1(), testCase->expectedItem.param1);
        QCOMPARE(actual->param2(), testCase->expectedItem.param2);
        QCOMPARE(actual->param3(), testCase->expectedItem.param3);
        QCOMPARE(actual->param4(), testCase->expectedItem.param4);
        QCOMPARE(actual->autoContinue(), testCase->expectedItem.autocontinue);
        QCOMPARE(actual->frame(), testCase->expectedItem.frame);
        testCaseIndex++;
    }
}

void MissionManagerTest::_testWriteFailureHandlingWorker()
{
    struct WriteTestCase_t {
        const char* failureText;
        MockLinkMissionItemHandler::FailureMode_t failureMode;
        bool shouldFail;
        int expectedErrorCode = -1;
    };

    const bool apm = _mockLink->getFirmwareType() == MAV_AUTOPILOT_ARDUPILOTMEGA;
    const WriteTestCase_t rgTestCases[] = {
        {"No Failure", MockLinkMissionItemHandler::FailNone, false},
        {"FailWriteMissionCountNoResponse", MockLinkMissionItemHandler::FailWriteMissionCountNoResponse, true},
        {"FailWriteMissionCountFirstResponse", MockLinkMissionItemHandler::FailWriteMissionCountFirstResponse, false},
        {"FailWriteRequest1NoResponse", MockLinkMissionItemHandler::FailWriteRequest1NoResponse, true},
        {"FailWriteRequest1FirstResponse", MockLinkMissionItemHandler::FailWriteRequest1FirstResponse, false},
        {"FailWriteRequest0IncorrectSequence", MockLinkMissionItemHandler::FailWriteRequest0IncorrectSequence, true},
        {"FailWriteRequest1IncorrectSequence", MockLinkMissionItemHandler::FailWriteRequest1IncorrectSequence, true},
        {"FailWriteRequest0ErrorAck", MockLinkMissionItemHandler::FailWriteRequest0ErrorAck, true},
        {"FailWriteRequest1ErrorAck", MockLinkMissionItemHandler::FailWriteRequest1ErrorAck, true},
        {"FailWriteFinalAckNoResponse", MockLinkMissionItemHandler::FailWriteFinalAckNoResponse, true,
         PlanManager::MaxRetryExceeded},
        {"FailWriteFinalAckFirstResponse", MockLinkMissionItemHandler::FailWriteFinalAckFirstResponse, !apm,
         apm ? -1 : PlanManager::ProtocolError},
        {"FailWriteFinalAckDuplicateAccepted", MockLinkMissionItemHandler::FailWriteFinalAckDuplicateAccepted, apm,
         apm ? PlanManager::MaxRetryExceeded : -1},
        {"FailWriteFinalAckErrorOnResend", MockLinkMissionItemHandler::FailWriteFinalAckErrorOnResend, true,
         apm ? PlanManager::MaxRetryExceeded : PlanManager::ProtocolError},
        {"FailWriteFinalAckErrorAck", MockLinkMissionItemHandler::FailWriteFinalAckErrorAck, true},
        {"FailWriteFinalAckMissingRequests", MockLinkMissionItemHandler::FailWriteFinalAckMissingRequests, true},
    };
    for (size_t i = 0; i < std::size(rgTestCases); i++) {
        const WriteTestCase_t* pCase = &rgTestCases[i];
        qCDebug(UnitTestLog) << "TEST CASE _testWriteFailureHandlingWorker" << pCase->failureText;
        _writeItems(pCase->failureMode, MAV_MISSION_ERROR, pCase->shouldFail, pCase->expectedErrorCode);
        _mockLink->resetMissionItemHandler();
    }
}

void MissionManagerTest::_testReadFailureHandlingWorker()
{
    struct ReadTestCase_t {
        const char* failureText;
        MockLinkMissionItemHandler::FailureMode_t failureMode;
        bool shouldFail;
    };

    /*
    static const ReadTestCase_t rgTestCases[] = {
        { "FailReadRequest1FirstResponse",      MockLinkMissionItemHandler::FailReadRequest1FirstResponse,      false },
    };*/
    static const ReadTestCase_t rgTestCases[] = {
        {"No Failure", MockLinkMissionItemHandler::FailNone, false},
        {"FailReadRequestListNoResponse", MockLinkMissionItemHandler::FailReadRequestListNoResponse, true},
        {"FailReadRequestListFirstResponse", MockLinkMissionItemHandler::FailReadRequestListFirstResponse, false},
        {"FailReadRequest0NoResponse", MockLinkMissionItemHandler::FailReadRequest0NoResponse, true},
        {"FailReadRequest1NoResponse", MockLinkMissionItemHandler::FailReadRequest1NoResponse, true},
        {"FailReadRequest1FirstResponse", MockLinkMissionItemHandler::FailReadRequest1FirstResponse, false},
        {"FailReadRequest0IncorrectSequence", MockLinkMissionItemHandler::FailReadRequest0IncorrectSequence, true},
        {"FailReadRequest1IncorrectSequence", MockLinkMissionItemHandler::FailReadRequest1IncorrectSequence, true},
        {"FailReadRequest0ErrorAck", MockLinkMissionItemHandler::FailReadRequest0ErrorAck, true},
        {"FailReadRequest1ErrorAck", MockLinkMissionItemHandler::FailReadRequest1ErrorAck, true},
    };
    for (size_t i = 0; i < std::size(rgTestCases); i++) {
        const ReadTestCase_t* pCase = &rgTestCases[i];
        qCDebug(UnitTestLog) << "TEST CASE _testReadFailureHandlingWorker" << pCase->failureText;
        _roundTripItems(pCase->failureMode, MAV_MISSION_ERROR, pCase->shouldFail);
        _mockLink->resetMissionItemHandler();
        _multiSpyMissionManager->clearAllSignals();
    }
}

void MissionManagerTest::_testWriteFailureHandlingAPM()
{
    if (!apmFirmwareSupported()) {
        QSKIP("ArduPilot support not registered in this build");
    }
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    _testWriteFailureHandlingWorker();
}

void MissionManagerTest::_testReadFailureHandlingAPM()
{
    if (!apmFirmwareSupported()) {
        QSKIP("ArduPilot support not registered in this build");
    }
    _initForFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    _testReadFailureHandlingWorker();
}

void MissionManagerTest::_testWriteFailureHandlingPX4()
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);
    _testWriteFailureHandlingWorker();
}

void MissionManagerTest::_testReadFailureHandlingPX4()
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);
    _testReadFailureHandlingWorker();
}

void MissionManagerTest::_testErrorAckFailureStrings()
{
    _initForFirmwareType(MAV_AUTOPILOT_PX4);

    struct ErrorStringTestCase_t {
        const char* ackResultStr;
        MAV_MISSION_RESULT ackResult;
    };

    static const ErrorStringTestCase_t rgTestCases[] = {
        {"MAV_MISSION_UNSUPPORTED_FRAME", MAV_MISSION_UNSUPPORTED_FRAME},
        {"MAV_MISSION_UNSUPPORTED", MAV_MISSION_UNSUPPORTED},
        {"MAV_MISSION_INVALID_PARAM1", MAV_MISSION_INVALID_PARAM1},
        {"MAV_MISSION_INVALID_PARAM2", MAV_MISSION_INVALID_PARAM2},
        {"MAV_MISSION_INVALID_PARAM3", MAV_MISSION_INVALID_PARAM3},
        {"MAV_MISSION_INVALID_PARAM4", MAV_MISSION_INVALID_PARAM4},
        {"MAV_MISSION_INVALID_PARAM5_X", MAV_MISSION_INVALID_PARAM5_X},
        {"MAV_MISSION_INVALID_PARAM6_Y", MAV_MISSION_INVALID_PARAM6_Y},
        {"MAV_MISSION_INVALID_PARAM7", MAV_MISSION_INVALID_PARAM7},
        {"MAV_MISSION_INVALID_SEQUENCE", MAV_MISSION_INVALID_SEQUENCE},
    };
    for (size_t i = 0; i < std::size(rgTestCases); i++) {
        const ErrorStringTestCase_t* pCase = &rgTestCases[i];
        qCDebug(UnitTestLog) << "TEST CASE _testErrorAckFailureStrings" << pCase->ackResultStr;
        _writeItems(MockLinkMissionItemHandler::FailWriteRequest1ErrorAck, pCase->ackResult, true /* shouldFail */);
        _mockLink->resetMissionItemHandler();
    }
}

void MissionManagerTest::_testUploadRecovery_data()
{
    QTest::addColumn<int>("firmware");
    QTest::addColumn<int>("planType");
    for (const auto firmware : {MAV_AUTOPILOT_GENERIC, MAV_AUTOPILOT_PX4, MAV_AUTOPILOT_ARDUPILOTMEGA}) {
        for (const auto planType : {MAV_MISSION_TYPE_MISSION, MAV_MISSION_TYPE_FENCE, MAV_MISSION_TYPE_RALLY}) {
            const QByteArray name = QStringLiteral("firmware-%1-plan-%2").arg(firmware).arg(planType).toLatin1();
            QTest::newRow(name.constData()) << int(firmware) << int(planType);
        }
    }
}

void MissionManagerTest::_testUploadRecovery()
{
    QFETCH(int, firmware);
    QFETCH(int, planType);
    const bool apm = firmware == MAV_AUTOPILOT_ARDUPILOTMEGA;
    if (apm && !apmFirmwareSupported()) {
        QSKIP("ArduPilot support not registered in this build");
    }
    _initForFirmwareType(static_cast<MAV_AUTOPILOT>(firmware));
    QVERIFY(_vehicle);
    PlanManager manager(_vehicle, static_cast<MAV_MISSION_TYPE>(planType));
    const bool mission = planType == MAV_MISSION_TYPE_MISSION;
    const bool resend = !apm && mission;
    const auto expectedPolicy = apm ? FirmwarePlugin::MissionUploadRecoveryPolicy::RestartUpload
                                    : (mission ? FirmwarePlugin::MissionUploadRecoveryPolicy::ResendLastItem
                                               : FirmwarePlugin::MissionUploadRecoveryPolicy::NoRecovery);
    QCOMPARE(_vehicle->firmwarePlugin()->missionUploadRecoveryPolicy(static_cast<MAV_MISSION_TYPE>(planType)),
             expectedPolicy);

    using Handler = MockLinkMissionItemHandler;
    const int attempts = 1 + PlanManager::_maxUploadRestartCount;
    constexpr int itemCount = 4;

    struct RecoveryCase
    {
        Handler::FailureMode_t mode;
        bool empty;
        int errorCode;  // -1 means success
        int counts;
        int items;
        bool readback;
        int uploads = 1;
    };

    const RecoveryCase cases[] = {
        {Handler::FailWriteMissionCountFirstResponse, false, -1, 2, itemCount, false},
        {Handler::FailWriteMissionCountNoResponse, false, PlanManager::MaxRetryExceeded,
         1 + PlanManager::_maxRetryCount, 0, false},
        {Handler::FailWriteRequest1FirstResponse, false, -1, 2, 1 + itemCount, !mission},
        {Handler::FailWriteRequest1NoResponse, false, PlanManager::MaxRetryExceeded, attempts, attempts, !mission, 2},
        {Handler::FailWriteRestartEarlyAck, false, PlanManager::VehicleAckError, 2, 1, !mission},
        {Handler::FailWriteFinalAckFirstResponse, false, apm ? -1 : PlanManager::ProtocolError, apm ? 2 : 1,
         apm ? 2 * itemCount : itemCount + int(resend), apm && !mission},
        {Handler::FailWriteFinalAckDuplicateAccepted, false,
         apm ? PlanManager::MaxRetryExceeded : (mission ? -1 : PlanManager::ProtocolError), apm ? attempts : 1,
         apm ? attempts * itemCount : itemCount + int(resend), apm && !mission},
        {Handler::FailWriteFinalAckErrorOnResend, false,
         apm ? PlanManager::MaxRetryExceeded : PlanManager::ProtocolError, apm ? attempts : 1,
         apm ? attempts * itemCount : itemCount + int(resend), apm && !mission},
        {Handler::FailWriteFinalAckNoResponse, false,
         apm || mission ? PlanManager::MaxRetryExceeded : PlanManager::ProtocolError, apm ? attempts : 1,
         apm ? attempts * itemCount : itemCount + (resend ? PlanManager::_maxRetryCount : 0), apm && !mission, 2},
        // The missing request and final-ACK loss must share one full-restart budget.
        {Handler::FailWriteRequest1ThenFinalAckNoResponse, false,
         apm || mission ? PlanManager::MaxRetryExceeded : PlanManager::ProtocolError, apm ? attempts : 2,
         apm ? 1 + PlanManager::_maxUploadRestartCount * itemCount
             : 1 + itemCount + (resend ? PlanManager::_maxRetryCount : 0),
         !mission},
        {Handler::FailWriteFinalAckErrorAck, false, PlanManager::VehicleAckError, 1, itemCount, false},
        {Handler::FailWriteFinalAckNoResponse, true, PlanManager::ProtocolError, 1, 0, false},
        {Handler::FailWriteFinalAckErrorAck, true, PlanManager::VehicleAckError, 1, 0, false},
        {Handler::FailNone, true, -1, 1, 0, false},
        {Handler::FailNone, false, -1, 1, itemCount, false},
    };

    QSignalSpy completeSpy(&manager, &PlanManager::sendComplete);
    QSignalSpy errorSpy(&manager, &PlanManager::error);
    for (const auto& testCase : cases) {
        for (int upload = 0; upload < testCase.uploads; upload++) {
            qCDebug(UnitTestLog) << "Recovery mode" << testCase.mode << "upload" << upload;
            _mockLink->setMissionItemFailureMode(testCase.mode, MAV_MISSION_ERROR);
            _mockLink->clearReceivedMavlinkMessageCounts();
            completeSpy.clear();
            errorSpy.clear();
            QList<MissionItem*> items;
            const bool skipHome = mission && !_vehicle->firmwarePlugin()->sendHomePositionToVehicle();
            const int count = testCase.empty ? 0 : itemCount + int(skipHome);
            for (int seq = 0; seq < count; seq++) {
                auto* item = new MissionItem(&manager);
                item->setSequenceNumber(seq);
                item->setCommand(mission
                                     ? MAV_CMD_NAV_WAYPOINT
                                     : (planType == MAV_MISSION_TYPE_FENCE ? MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION
                                                                           : MAV_CMD_NAV_RALLY_POINT));
                item->setFrame(MAV_FRAME_GLOBAL_RELATIVE_ALT);
                item->setParam1(mission ? 0 : itemCount);
                item->setParam5(47.0 + seq * 0.001);
                item->setParam6(8.0 + seq * 0.001);
                item->setParam7(50);
                items.append(item);
            }
            manager.writeMissionItems(items);
            QVERIFY(manager.inProgress());
            QVERIFY(!manager.uploadRequiresReadback());
            QVERIFY_SIGNAL_COUNT_WAIT(completeSpy, 1, TestTimeout::mediumMs());
            QVERIFY(!manager.inProgress());
            const bool shouldFail = testCase.errorCode >= 0;
            QCOMPARE(completeSpy.count(), 1);
            QCOMPARE(completeSpy.first().at(0).toBool(), shouldFail);
            QCOMPARE(errorSpy.count(), int(shouldFail));
            if (shouldFail) {
                QCOMPARE(errorSpy.first().at(0).toInt(), testCase.errorCode);
            }
            QCOMPARE(_mockLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_COUNT), testCase.counts);
            QCOMPARE(_mockLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_ITEM_INT), testCase.items);
            QCOMPARE(manager.uploadRequiresReadback(), testCase.readback);
            // Two ACK intervals expose a stale timeout or delayed ACK producing a second terminal result.
            QVERIFY_NO_SIGNAL_WAIT(completeSpy, 2 * PlanManager::kTestAckTimeoutMs);
            QCOMPARE(completeSpy.count(), 1);
            QCOMPARE(errorSpy.count(), int(shouldFail));

            if (!shouldFail && testCase.readback) {
                _mockLink->setMissionItemFailureMode(Handler::FailNone, MAV_MISSION_ERROR);
                QSignalSpy readSpy(&manager, &PlanManager::newMissionItemsAvailable);
                manager.loadFromVehicle();
                QVERIFY_SIGNAL_COUNT_WAIT(readSpy, 1, TestTimeout::mediumMs());
                QCOMPARE(errorSpy.count(), 0);
                QCOMPARE(manager.missionItems().count(), itemCount);
                for (int seq = 0; seq < itemCount; seq++) {
                    QCOMPARE(manager.missionItems()[seq]->sequenceNumber(), seq);
                    QVERIFY(qAbs(manager.missionItems()[seq]->param5() - (47.0 + seq * 0.001)) < 1e-6);
                    QVERIFY(qAbs(manager.missionItems()[seq]->param6() - (8.0 + seq * 0.001)) < 1e-6);
                }
                // A download alone does not certify equality; the caller owns comparison/verification.
                QVERIFY(manager.uploadRequiresReadback());
                QCOMPARE(completeSpy.count(), 1);
            }
        }
    }
}

#include "UnitTest.h"

UT_REGISTER_TEST(MissionManagerTest, TestLabel::Integration, TestLabel::MissionManager, TestLabel::Serial)
