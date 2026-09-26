#include "LinkManagerTest.h"

#include <algorithm>

#include <QtCore/QScopeGuard>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "AutoConnectSettings.h"
#include "LinkManager.h"
#include "MAVLinkProtocol.h"
#include "MockLink.h"
#include "SettingsManager.h"
#include "UDPLink.h"
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <fcntl.h>
#include <unistd.h>
#endif

SharedLinkConfigurationPtr LinkManagerTest::_addMockConfig(const QString &name, bool dynamic, bool autoConnect)
{
    MockConfiguration *const mockConfig = new MockConfiguration(name);
    mockConfig->setDynamic(dynamic);
    mockConfig->setAutoConnect(autoConnect);

    SharedLinkConfigurationPtr config = linkManager()->addConfiguration(mockConfig);
    config->setAutoConnectStarted(true);
    if (!linkManager()->createConnectedLink(config)) {
        linkManager()->removeConfiguration(config.get());
        return nullptr;
    }
    return config;
}

void LinkManagerTest::_reconnect()
{
    linkManager()->_reconnectAutoConnectLinks();
}

void LinkManagerTest::_testReconnectsDroppedAutoConnectLink()
{
    SharedLinkConfigurationPtr config = _addMockConfig(QStringLiteral("ReconnectMock"), false /*dynamic*/, true /*autoConnect*/);
    QVERIFY(config);
    QVERIFY(config->link());

    config->link()->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());

    _reconnect();
    QVERIFY(config->link());

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testSuppressedLinkNotReconnected()
{
    SharedLinkConfigurationPtr config = _addMockConfig(QStringLiteral("SuppressMock"), false /*dynamic*/, true /*autoConnect*/);
    QVERIFY(config);
    QVERIFY(config->link());

    // Manual disconnect sets suppressAutoReconnect so the timer leaves it alone.
    linkManager()->disconnectLink(config->link());
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    QVERIFY(config->suppressAutoReconnect());

    _reconnect();
    QVERIFY(config->link() == nullptr);

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testDynamicLinkNotReconnected()
{
    SharedLinkConfigurationPtr config = _addMockConfig(QStringLiteral("DynamicMock"), true /*dynamic*/, true /*autoConnect*/);
    QVERIFY(config);
    QVERIFY(config->link());

    config->link()->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());

    _reconnect();
    QVERIFY(config->link() == nullptr);

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testNonAutoConnectLinkNotReconnected()
{
    SharedLinkConfigurationPtr config = _addMockConfig(QStringLiteral("ManualMock"), false /*dynamic*/, false /*autoConnect*/);
    QVERIFY(config);
    QVERIFY(config->link());

    config->link()->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());

    _reconnect();
    QVERIFY(config->link() == nullptr);

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testNeverStartedLinkNotConnected()
{
    MockConfiguration *const mockConfig = new MockConfiguration(QStringLiteral("NeverStartedMock"));
    mockConfig->setDynamic(false);
    mockConfig->setAutoConnect(true);
    SharedLinkConfigurationPtr config = linkManager()->addConfiguration(mockConfig);
    QVERIFY(!config->autoConnectStarted());
    QVERIFY(config->link() == nullptr);

    _reconnect();
    QVERIFY(config->link() == nullptr);

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testLinkActiveStableAcrossReconnect()
{
    SharedLinkConfigurationPtr config = _addMockConfig(QStringLiteral("ActiveMock"), false /*dynamic*/, true /*autoConnect*/);
    QVERIFY(config);
    QVERIFY(config->linkActive());

    config->link()->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    QVERIFY(config->linkActive());

    linkManager()->disconnectLinkConfiguration(config.get());
    QVERIFY(!config->linkActive());
    _reconnect();
    QVERIFY(config->link() == nullptr);

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testStaleBytesNotAttributedToRecycledLinkAddress()
{
    SharedLinkConfigurationPtr staleConfig =
        _addMockConfig(QStringLiteral("StaleMock"), true /*dynamic*/, false /*autoConnect*/);
    SharedLinkConfigurationPtr liveConfig =
        _addMockConfig(QStringLiteral("LiveMock"), true /*dynamic*/, false /*autoConnect*/);
    QVERIFY(staleConfig && liveConfig);
    const auto removeConfigs = qScopeGuard([this, staleConfig, liveConfig] {
        linkManager()->removeConfiguration(staleConfig.get());
        linkManager()->removeConfiguration(liveConfig.get());
    });
    LinkInterface* const staleLink = staleConfig->link();
    LinkInterface* const liveLink = liveConfig->link();
    QVERIFY(staleLink && liveLink);

    // GCS-typed so the heartbeat is reported without spawning a Vehicle
    constexpr uint8_t kForeignSysid = 77;
    mavlink_message_t message{};
    (void) mavlink_msg_heartbeat_pack_chan(kForeignSysid, MAV_COMP_ID_AUTOPILOT1, liveLink->mavlinkChannel(), &message,
                                           MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
    const QByteArray bytes(reinterpret_cast<const char*>(buffer), mavlink_msg_to_send_buffer(buffer, &message));

    QSignalSpy heartbeatSpy(MAVLinkProtocol::instance(), &MAVLinkProtocol::vehicleHeartbeatInfo);
    QVERIFY(heartbeatSpy.isValid());
    const auto foreignHeartbeatsOn = [&heartbeatSpy](const LinkInterface* link) {
        return std::count_if(heartbeatSpy.cbegin(), heartbeatSpy.cend(), [link](const QList<QVariant>& args) {
            return (args.at(0).value<LinkInterface*>() == link) && (args.at(1).toInt() == kForeignSysid);
        });
    };

    // A delivery from one link carrying another live link's pointer is exactly what a stale queued delivery looks like
    // once the sender's address has been recycled for a new link: it must not be attributed to that new link.
    emit staleLink->bytesReceived(liveLink, bytes);
    QCOMPARE(foreignHeartbeatsOn(liveLink), 0);

    // The link's own deliveries are still processed
    emit liveLink->bytesReceived(liveLink, bytes);
    QCOMPARE(foreignHeartbeatsOn(liveLink), 1);
}

UT_REGISTER_TEST(LinkManagerTest, TestLabel::Integration, TestLabel::Comms)

#ifndef QGC_NO_SERIAL_LINK
#include "SerialAutoConnect.h"
#include "SerialLink.h"
#include "SerialPortManager.h"

void LinkManagerTest::_testSerialReservationFollowsLink()
{
    SerialPortManager ports;
    const QString name = QStringLiteral("/test/link-claim");
    auto config = std::make_shared<SerialConfiguration>(QStringLiteral("Reservation"));
    config->setPortName(name);
    SharedLinkConfigurationPtr sharedConfig = config;
    auto claim = ports.reservePort(name);
    QVERIFY(claim);
    {
        SerialLink link(sharedConfig, std::move(claim));
        QVERIFY(!claim);
        QVERIFY(ports.isPortReserved(name));
        QVERIFY(!ports.reservePort(name));
    }
    QVERIFY(ports.reservePort(name));
}

void LinkManagerTest::_testReservedSerialPortNotOpened()
{
    const QString port = QStringLiteral("/test/gps-reserved");
    auto reservation = SerialPortManager::instance()->reservePort(port);
    QVERIFY(reservation);
    auto config = std::make_shared<SerialConfiguration>(QStringLiteral("Reserved GPS port"));
    config->setPortName(port);
    SharedLinkConfigurationPtr sharedConfig = config;
    QVERIFY(!linkManager()->createConnectedLink(sharedConfig));
    QVERIFY(!config->link());
    QVERIFY(SerialPortManager::instance()->isPortReserved(port));
}

void LinkManagerTest::_testOccupiedSerialAutoConnectRecovers()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    linkManager()->init();
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    QVERIFY(master >= 0);
    const auto closeMaster = qScopeGuard([master] { ::close(master); });
    QCOMPARE(::grantpt(master), 0);
    QCOMPARE(::unlockpt(master), 0);
    const char* name = ::ptsname(master);
    QVERIFY(name);
    const QString location = QString::fromLocal8Bit(name);
    QSerialPort occupied(location);
    QVERIFY(occupied.open(QIODevice::ReadWrite));
    const QList<SerialPortManager::Port> ports = {
        {location, location, QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")}};
    const auto removePort = qScopeGuard([&] {
        linkManager()->_serialAutoConnect->_configs.remove(location);
        linkManager()->_serialAutoConnect->_waitingPorts.remove(location);
    });
    linkManager()->_serialAutoConnect->update(ports, {.pixhawk = true});
    linkManager()->_serialAutoConnect->_waitingPorts[location].setRemainingTime(0);
    linkManager()->_serialAutoConnect->update(ports, {.pixhawk = true});
    const auto config = linkManager()->_serialAutoConnect->_configs.value(location);
    QVERIFY(config);
    QCOMPARE(config->type(), LinkConfiguration::TypeSerial);
    QTRY_VERIFY_WITH_TIMEOUT(!config->link(), TestTimeout::shortMs());
    QCOMPARE(linkManager()->_serialAutoConnect->_configs.value(location), config);
    QVERIFY(!config->reconnectReady());
    linkManager()->_serialAutoConnect->update(ports, {.pixhawk = true});
    QVERIFY(!config->link());

    occupied.close();
    QTRY_VERIFY_WITH_TIMEOUT(config->reconnectReady(), TestTimeout::mediumMs());
    linkManager()->_serialAutoConnect->update(ports, {.pixhawk = true});
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::shortMs());
    QCOMPARE(linkManager()->_serialAutoConnect->_configs.value(location), config);
    QVERIFY(!SerialPortManager::instance()->canAutoConnectPort(location));
    linkManager()->disconnectLink(config->link());
    QTRY_VERIFY_WITH_TIMEOUT(!config->link(), TestTimeout::shortMs());
#else
    QSKIP("Occupied serial reconnect coverage requires a Linux pseudo-terminal");
#endif
}
#endif

void LinkManagerTest::_testUdpAutoConnectDefaults()
{
    linkManager()->init();
    auto* defaults = SettingsManager::instance()->autoConnectSettings();
    const QVariant oldEnabled = defaults->autoConnectUDP()->rawValue();
    const QVariant oldListen = defaults->udpListenPort()->rawValue();
    const QVariant oldHost = defaults->udpTargetHostIP()->rawValue();
    const QVariant oldTarget = defaults->udpTargetHostPort()->rawValue();
    const auto restore = qScopeGuard([&] {
        defaults->autoConnectUDP()->setRawValue(oldEnabled);
        defaults->udpListenPort()->setRawValue(oldListen);
        defaults->udpTargetHostIP()->setRawValue(oldHost);
        defaults->udpTargetHostPort()->setRawValue(oldTarget);
    });
    QUdpSocket probe;
    QVERIFY(probe.bind(QHostAddress::LocalHost, 0));
    const quint16 port = probe.localPort();
    probe.close();
    defaults->autoConnectUDP()->setRawValue(true);
    defaults->udpListenPort()->setRawValue(port);
    defaults->udpTargetHostIP()->setRawValue(QStringLiteral("127.0.0.2"));
    defaults->udpTargetHostPort()->setRawValue(14560);
    linkManager()->_addUDPAutoConnectLink();
    QCOMPARE(linkManager()->links().size(), 1);
    const auto link = linkManager()->links().constFirst();
    auto* config = qobject_cast<UDPConfiguration*>(link->linkConfiguration().get());
    QVERIFY(config);
    QVERIFY(config->isDynamic());
    QVERIFY(config->isAutoConnect());
    QCOMPARE(config->localPort(), port);
    QCOMPARE(config->hostList(), QStringList{QStringLiteral("127.0.0.2:14560")});
    QTRY_VERIFY_WITH_TIMEOUT(link->isConnected(), TestTimeout::mediumMs());
    linkManager()->disconnectLink(link.get());
    QTRY_VERIFY_WITH_TIMEOUT(!config->link(), TestTimeout::mediumMs());
    linkManager()->removeConfiguration(config);
}
