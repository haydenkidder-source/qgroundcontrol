#include "UDPLinkReceiveTest.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QScopeGuard>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QTest>

#include "LinkManager.h"
#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "MultiSignalSpy.h"
#include "UDPLink.h"

namespace {

constexpr uint8_t UDP_RECEIVE_TEST_SYSTEM_ID = 77;

QByteArray udpReceiveTestFrame(mavlink_status_t& packStatus, uint16_t ordinal)
{
    // The generated packer copies the entire fixed-size param_id field.
    const char paramId[MAVLINK_MSG_PARAM_VALUE_FIELD_PARAM_ID_LEN] = "UDP_RECEIVE";
    mavlink_message_t message{};
    (void) mavlink_msg_param_value_pack_status(UDP_RECEIVE_TEST_SYSTEM_ID, MAV_COMP_ID_AUTOPILOT1, &packStatus,
                                               &message, paramId, 1.0f, MAV_PARAM_TYPE_REAL32, 6, ordinal);
    uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char*>(bytes), size);
}

}  // namespace

quint16 UDPLinkReceiveTest::_unusedUdpPort()
{
    QUdpSocket reservation;
    if (!reservation.bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress)) {
        return 0;
    }
    const quint16 port = reservation.localPort();
    reservation.close();
    return (port == 14550) ? 0 : port;
}

SharedLinkConfigurationPtr UDPLinkReceiveTest::_addUdpConfig(quint16 port)
{
    auto* const udpConfig = new UDPConfiguration(QStringLiteral("UDP receive test"));
    udpConfig->setLocalPort(port);
    return linkManager()->addConfiguration(udpConfig);
}

void UDPLinkReceiveTest::_cleanupUdpConfig(const SharedLinkConfigurationPtr& config, quint16 port)
{
    config->setSuppressAutoReconnect(true);
    linkManager()->removeConfiguration(config.get());
    // Poll during failure teardown too; QSignalSpy::wait stops waiting after a failed assertion.
    const bool stopped = QTest::qWaitFor(
        [this, &config] {
            if (config->link()) {
                return false;
            }
            for (const SharedLinkInterfacePtr& registeredLink : linkManager()->links()) {
                if (registeredLink->linkConfiguration() == config) {
                    return false;
                }
            }
            return true;
        },
        TestTimeout::mediumMs());
    QUdpSocket probe;
    const bool portFree = probe.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress);
    QVERIFY2(stopped, "UDP cleanup left a registered link");
    QVERIFY2(portFree, "UDP cleanup left the port occupied");
}

void UDPLinkReceiveTest::_testReceiveFrames(int framesBeforeEmpty, int framesAfterEmpty)
{
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(port);
    const auto cleanupConfig = qScopeGuard([this, config, port] { _cleanupUdpConfig(config, port); });
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    LinkInterface* const link = config->link();
    MultiSignalSpy linkSpy;
    QVERIFY(linkSpy.init(link, {"communicationError"}));

    QList<quint16> ordinals;
    QObject receiver;
    (void) connect(MAVLinkProtocol::instance(), &MAVLinkProtocol::messageReceived, &receiver,
                   [link, &ordinals](LinkInterface* receivedLink, const mavlink_message_t& message) {
                       if ((receivedLink == link) && (message.sysid == UDP_RECEIVE_TEST_SYSTEM_ID) &&
                           (message.msgid == MAVLINK_MSG_ID_PARAM_VALUE)) {
                           ordinals.append(mavlink_msg_param_value_get_param_index(&message));
                       }
                   });

    QUdpSocket peer;
    QVERIFY(peer.bind(QHostAddress::LocalHost, 0, QAbstractSocket::DontShareAddress));
    mavlink_status_t packStatus{};
    for (int ordinal = 0; ordinal < framesBeforeEmpty; ++ordinal) {
        const QByteArray frame = udpReceiveTestFrame(packStatus, static_cast<uint16_t>(ordinal));
        QCOMPARE(peer.writeDatagram(frame, QHostAddress::LocalHost, port), qint64(frame.size()));
    }
    if (framesBeforeEmpty > 0) {
        QTRY_COMPARE_WITH_TIMEOUT(ordinals.size(), framesBeforeEmpty, TestTimeout::mediumMs());
    }
    QCOMPARE(peer.writeDatagram(QByteArray(), QHostAddress::LocalHost, port), qint64{0});
    const int frameCount = framesBeforeEmpty + framesAfterEmpty;
    for (int ordinal = framesBeforeEmpty; ordinal < frameCount; ++ordinal) {
        const QByteArray frame = udpReceiveTestFrame(packStatus, static_cast<uint16_t>(ordinal));
        QCOMPARE(peer.writeDatagram(frame, QHostAddress::LocalHost, port), qint64(frame.size()));
    }

    QTRY_COMPARE_WITH_TIMEOUT(ordinals.size(), frameCount, TestTimeout::mediumMs());
    for (int ordinal = 0; ordinal < frameCount; ++ordinal) {
        QCOMPARE(ordinals.at(ordinal), static_cast<quint16>(ordinal));
    }
    QVERIFY(linkSpy.notEmitted("communicationError"));
}

void UDPLinkReceiveTest::_testEmptyDatagramDoesNotBlockLaterDatagrams()
{
    _testReceiveFrames(0, 5);
}

void UDPLinkReceiveTest::_testEmptyDatagramBetweenFrames()
{
    _testReceiveFrames(3, 3);
}

void UDPLinkReceiveTest::_testEmptyDatagramDoesNotLearnTarget()
{
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(port);
    const auto cleanupConfig = qScopeGuard([this, config, port] { _cleanupUdpConfig(config, port); });
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    LinkInterface* const link = config->link();
    MultiSignalSpy linkSpy;
    QVERIFY(linkSpy.init(link, {"communicationError", "bytesReceived", "bytesSent"}));

    QUdpSocket peer;
    QVERIFY(peer.bind(QHostAddress::LocalHost, 0, QAbstractSocket::DontShareAddress));
    QCOMPARE(peer.writeDatagram(QByteArray(), QHostAddress::LocalHost, port), qint64{0});
    const QByteArray reply("empty-datagram-target-test");
    // Repeat writes while polling so a write racing ahead of readyRead cannot hide target learning.
    QElapsedTimer observationTimer;
    observationTimer.start();
    const auto observePeer = [&] {
        link->writeBytesThreadSafe(reply.constData(), static_cast<int>(reply.size()));
        return peer.hasPendingDatagrams() || (observationTimer.elapsed() >= TestTimeout::shortMs());
    };
    QTRY_VERIFY_WITH_TIMEOUT(observePeer(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(linkSpy.count("bytesSent") > 0, TestTimeout::mediumMs());
    QVERIFY(!peer.hasPendingDatagrams());
    QVERIFY(linkSpy.notEmitted("bytesReceived", "communicationError"));
}

UT_REGISTER_TEST(UDPLinkReceiveTest, TestLabel::Integration, TestLabel::Comms)
