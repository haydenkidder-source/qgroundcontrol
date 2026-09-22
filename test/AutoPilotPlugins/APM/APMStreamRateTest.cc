#include "APMStreamRateTest.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>

#include "Fixtures/RAIIFixtures.h"
#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "Vehicle.h"

void APMStreamRateTest::_testBackoffAndRecovery_data()
{
    QTest::addColumn<bool>("batteryRecovery");
    QTest::newRow("battery-status") << true;
    QTest::newRow("home-position") << false;
}

void APMStreamRateTest::_testBackoffAndRecovery()
{
    QFETCH(bool, batteryRecovery);
    QVERIFY(_vehicle);
    QVERIFY(_mockLink);

    // Finish command retries before the next stream retry; neither ACKs nor telemetry can get through.
    TestFixtures::MavCommandAckTimeoutFixture shortAckTimeout(50);
    ignoreLogMessage("Vehicle.MavCommandQueue", QtWarningMsg,
                     QRegularExpression("Giving up sending command after max retries: MAV_CMD_SET_MESSAGE_INTERVAL"));
    _mockLink->setCommLost(true);

    QElapsedTimer elapsed;
    elapsed.start();
    QList<qint64> attempts;
    mavlink_message_t receiveBuffer{};
    mavlink_status_t receiveStatus{};
    QObject observer;
    connect(_mockLink, &MockLink::writeBytesQueuedSignal, &observer, [&](const QByteArray& bytes) {
        for (const char byte : bytes) {
            mavlink_message_t message{};
            mavlink_status_t status{};
            if (mavlink_frame_char_buffer(&receiveBuffer, &receiveStatus, static_cast<uint8_t>(byte), &message,
                                          &status) != MAVLINK_FRAMING_OK ||
                message.msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
                continue;
            }
            mavlink_command_long_t command{};
            mavlink_msg_command_long_decode(&message, &command);
            // MAV_CMD_SET_MESSAGE_INTERVAL is excluded from MavCommandQueue's retry list (see
            // MavCommandQueue::_shouldRetry()), so it is only ever sent once per initializeStreamRates()
            // call; no confirmation-based de-duplication is needed to count one entry per attempt.
            if (command.command == MAV_CMD_SET_MESSAGE_INTERVAL && command.param1 == MAVLINK_MSG_ID_HOME_POSITION) {
                attempts.append(elapsed.elapsed());
            }
        }
    });

    // No incoming messages are needed to drive retries. Test-mode intervals are one tenth of production.
    QTRY_VERIFY_WITH_TIMEOUT(attempts.size() >= 5, TestTimeout::longMs());
    QCOMPARE(attempts.size(), 5);
    const QList<qint64> expectedIntervals{2000, 4000, 6000, 6000};
    for (qsizetype i = 0; i < expectedIntervals.size(); ++i) {
        const qint64 interval = attempts[i + 1] - attempts[i];
        // Allow event-loop scheduling jitter, while rejecting both flat retries and an uncapped 8-second interval.
        QVERIFY2(qAbs(interval - expectedIntervals[i]) < 750,
                 qPrintable(QStringLiteral("Retry interval %1: expected %2 ms, got %3 ms")
                                .arg(i)
                                .arg(expectedIntervals[i])
                                .arg(interval)));
    }

    // Inject exactly one real recovery message through the protocol while MockLink's streams stay silent.
    mavlink_message_t recovery{};
    mavlink_status_t packStatus{};
    if (batteryRecovery) {
        mavlink_battery_status_t battery{};
        battery.voltages[0] = 12000;
        battery.battery_remaining = 80;
        mavlink_msg_battery_status_encode_status(_mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1, &packStatus, &recovery,
                                                 &battery);
    } else {
        mavlink_home_position_t home{};
        home.latitude = 470000000;
        home.longitude = 80000000;
        home.q[0] = 1;
        mavlink_msg_home_position_encode_status(_mockLink->vehicleId(), MAV_COMP_ID_AUTOPILOT1, &packStatus, &recovery,
                                                &home);
    }
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    const int length = mavlink_msg_to_send_buffer(buffer, &recovery);
    MAVLinkProtocol::instance()->receiveBytes(_mockLink, QByteArray(reinterpret_cast<const char*>(buffer), length));

    // The other stream is still stale: either recovery message must restore the base retry interval.
    QTRY_VERIFY_WITH_TIMEOUT(attempts.size() >= 6, TestTimeout::mediumMs());
    QCOMPARE(attempts.size(), 6);
    const qint64 recoveryInterval = attempts[5] - attempts[4];
    QVERIFY2(qAbs(recoveryInterval - 1000) < 750,
             qPrintable(QStringLiteral("Recovery retry interval: expected 1000 ms, got %1 ms").arg(recoveryInterval)));
}

UT_REGISTER_TEST(APMStreamRateTest, TestLabel::Integration, TestLabel::Vehicle)
