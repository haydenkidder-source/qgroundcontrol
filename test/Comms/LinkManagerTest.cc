#include "LinkManagerTest.h"

#include <algorithm>
#include <limits>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QScopeGuard>
#include <QtCore/QSemaphore>
#include <QtCore/QThread>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "AutoConnectSettings.h"
#include "LinkManager.h"
#include "MAVLinkProtocol.h"
#include "MavlinkSettings.h"
#include "MockLink.h"
#include "ScopedConnectionLogCapture.h"
#include "SettingsManager.h"
#include "UDPLink.h"
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
class LinkManagerUdpStartupGate : public QObject
{
public:
    explicit LinkManagerUdpStartupGate(bool enabled = true)
    {
        if (enabled) {
            QCoreApplication::instance()->installEventFilter(this);
        }
    }

    ~LinkManagerUdpStartupGate() override
    {
        QCoreApplication::instance()->removeEventFilter(this);
        _release();
    }

    bool holdWorker(QThread* workerThread)
    {
        if (!workerThread || _released) {
            return false;
        }
        const auto state = std::make_shared<State>();
        state->helper = new QObject;
        state->helper->moveToThread(workerThread);
        (void) connect(workerThread, &QThread::finished, state->helper.data(), &QObject::deleteLater);
        state->queued = QMetaObject::invokeMethod(
            state->helper.data(),
            [state] {
                state->entered.release();
                state->resume.acquire();
            },
            Qt::QueuedConnection);
        _states.append(state);
        return state->queued;
    }

    bool entered() const
    {
        return !_states.isEmpty() && std::ranges::all_of(_states, [](const auto& state) {
            return state->queued && (state->entered.available() > 0);
        });
    }

    bool releaseAndDrain()
    {
        bool queued = !_states.isEmpty();
        for (const auto& state : _states) {
            // This worker marker follows all previously queued operations. Its main-thread marker
            // also follows their queued signal deliveries to UDPLink.
            queued =
                state->helper &&
                QMetaObject::invokeMethod(
                    state->helper.data(),
                    [state] {
                        (void) QMetaObject::invokeMethod(
                            QCoreApplication::instance(), [state] { state->drained = true; }, Qt::QueuedConnection);
                    },
                    Qt::QueuedConnection) &&
                queued;
        }
        _release();
        return queued;
    }

    bool drained() const
    {
        return !_states.isEmpty() && std::ranges::all_of(_states, [](const auto& state) { return state->drained; });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!_released && (event->type() == QEvent::ChildAdded) && qobject_cast<UDPLink*>(watched)) {
            QObject* const child = static_cast<QChildEvent*>(event)->child();
            // ChildAdded arrives during QObject construction, before the child can be cast to QThread.
            // UDPLink names its thread before starting it or queuing connectLink.
            (void) connect(child, &QObject::objectNameChanged, this, [this, child] {
                auto* const workerThread = qobject_cast<QThread*>(child);
                if (!workerThread) {
                    return;
                }
                (void) holdWorker(workerThread);
            });
        }
        return QObject::eventFilter(watched, event);
    }

private:
    struct State
    {
        QPointer<QObject> helper;
        QSemaphore entered;
        QSemaphore resume;
        bool queued = false;
        bool drained = false;
    };

    void _release()
    {
        if (!_released) {
            _released = true;
            for (const auto& state : _states) {
                state->resume.release();
            }
        }
    }

    QList<std::shared_ptr<State>> _states;
    bool _released = false;
};
}  // namespace

quint16 LinkManagerTest::_unusedUdpPort()
{
    QUdpSocket reservation;
    if (!reservation.bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress)) {
        return 0;
    }
    const quint16 port = reservation.localPort();
    reservation.close();
    return (port == 14550) ? 0 : port;
}

bool LinkManagerTest::_udpPortIsFree(quint16 port)
{
    QUdpSocket probe;
    return probe.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress);
}

SharedLinkConfigurationPtr LinkManagerTest::_addUdpConfig(const QString& name, quint16 port)
{
    // Multicast availability depends on the host's interfaces; these tests exercise loopback unicast.
    ignoreLogMessage("Comms.UDPLink", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Failed to join multicast group")));
    auto* const udpConfig = new UDPConfiguration(name);
    udpConfig->setLocalPort(port);
    return linkManager()->addConfiguration(udpConfig);
}

QList<SharedLinkInterfacePtr> LinkManagerTest::_udpLinksForConfig(const SharedLinkConfigurationPtr& config)
{
    QList<SharedLinkInterfacePtr> links;
    for (const SharedLinkInterfacePtr& link : linkManager()->links()) {
        if (link->linkConfiguration() == config) {
            links.append(link);
        }
    }
    return links;
}

void LinkManagerTest::_cleanupUdpConfig(const SharedLinkConfigurationPtr& config)
{
    config->setSuppressAutoReconnect(true);
    // Poll even after a failed assertion; a released startup gate may still have a connect queued.
    // Scan the registry because duplicate/orphaned links are no longer reachable through config->link().
    const bool stopped = QTest::qWaitFor(
        [this, &config] {
            const auto links = _udpLinksForConfig(config);
            for (const SharedLinkInterfacePtr& link : links) {
                if (link->isConnected()) {
                    link->disconnect();
                }
            }
            return links.isEmpty() && !config->link();
        },
        TestTimeout::mediumMs());
    const auto& configs = linkManager()->_rgLinkConfigs;
    if (std::ranges::find(configs, config) != configs.end()) {
        linkManager()->removeConfiguration(config.get());
    }
    QVERIFY2(stopped, "UDP cleanup left a registered link");
    const auto* const udpConfig = qobject_cast<const UDPConfiguration*>(config.get());
    QVERIFY(udpConfig);
    QVERIFY2(_udpPortIsFree(udpConfig->localPort()), "UDP cleanup left the port occupied");
}

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

void LinkManagerTest::_expectUdpBindFailureLogs()
{
    expectLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression(QStringLiteral("AddressInUseError")));
    expectLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression(QStringLiteral("Failed to bind UDP socket")));
    expectLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression(QStringLiteral("Communication error")));
}

void LinkManagerTest::_verifyUdpBindFailureLogs()
{
    for (int i = 0; i < 3; i++) {
        verifyExpectedLogMessage();
    }
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

void LinkManagerTest::_testUdpUnresolvedHostFailsConnect()
{
    UDPConfiguration* const udpConfig = new UDPConfiguration(QStringLiteral("UnresolvedUdp"));
    udpConfig->addHost(QStringLiteral("drone.invalid"), 14550);
    SharedLinkConfigurationPtr config = linkManager()->addConfiguration(udpConfig);

    expectLogMessage("Comms.UDPLink", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Could not resolve host: drone.invalid")));
    expectAppMessage(QRegularExpression(QStringLiteral("Could not resolve host: drone.invalid")));
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testUdpUnresolvedHostAllowedStaysConnected()
{
    UDPConfiguration* const udpConfig = new UDPConfiguration(QStringLiteral("UnresolvedUdpAllowed"));
    udpConfig->setRequireResolvedHosts(false);
    udpConfig->addHost(QStringLiteral("drone.invalid"), 14550);
    SharedLinkConfigurationPtr config = linkManager()->addConfiguration(udpConfig);

    expectLogMessage("Comms.UDPLink", QtWarningMsg, QRegularExpression(QStringLiteral("Could not resolve host")));
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    verifyExpectedLogMessage();

    linkManager()->removeConfiguration(config.get());
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
}

void LinkManagerTest::_testUdpBindFailureDisconnects()
{
    QUdpSocket blocker;
    QVERIFY(blocker.bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress));

    UDPConfiguration* const udpConfig = new UDPConfiguration(QStringLiteral("BindFailUdp"));
    udpConfig->setLocalPort(blocker.localPort());
    SharedLinkConfigurationPtr config = linkManager()->addConfiguration(udpConfig);

    expectAppMessage(QRegularExpression(QStringLiteral("Link BindFailUdp:")));
    _expectUdpBindFailureLogs();
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    _verifyUdpBindFailureLogs();
    verifyExpectedLogMessage();

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testConfigurationNameValidation()
{
    const auto first = linkManager()->addConfiguration(new UDPConfiguration(QStringLiteral("Validation A")));
    const auto duplicate = linkManager()->addConfiguration(new UDPConfiguration(QStringLiteral("Validation A")));
    const auto other = linkManager()->addConfiguration(new UDPConfiguration(QStringLiteral("Validation B")));
    const auto cleanup = qScopeGuard([&] {
        linkManager()->removeConfiguration(first.get());
        linkManager()->removeConfiguration(duplicate.get());
        linkManager()->removeConfiguration(other.get());
    });

    const auto count = linkManager()->_rgLinkConfigs.size();
    std::unique_ptr<LinkConfiguration> created(new UDPConfiguration(QStringLiteral("Validation A")));
    QVERIFY(!linkManager()->endCreateConfiguration(created.get()));
    QCOMPARE(created->name(), QStringLiteral("Validation A"));
    QCOMPARE(linkManager()->_rgLinkConfigs.size(), count);
    QVERIFY(std::none_of(linkManager()->_rgLinkConfigs.cbegin(), linkManager()->_rgLinkConfigs.cend(),
                         [&](const auto& config) { return config.get() == created.get(); }));
    created->setName(QString());
    QVERIFY(!linkManager()->endCreateConfiguration(created.get()));
    created->setName(QStringLiteral("validation a"));
    QVERIFY(linkManager()->endCreateConfiguration(created.get()));
    const auto accepted = linkManager()->_rgLinkConfigs.last();
    created.release();
    linkManager()->removeConfiguration(accepted.get());

    std::unique_ptr<LinkConfiguration> edit(linkManager()->startConfigurationEditing(first.get()));
    edit->setName(QStringLiteral("Validation B"));
    QVERIFY(!linkManager()->endConfigurationEditing(first.get(), edit.get()));
    QCOMPARE(first->name(), QStringLiteral("Validation A"));
    QCOMPARE(edit->name(), QStringLiteral("Validation B"));
    QCOMPARE(linkManager()->_rgLinkConfigs.size(), count);
    QVERIFY(std::none_of(linkManager()->_rgLinkConfigs.cbegin(), linkManager()->_rgLinkConfigs.cend(),
                         [&](const auto& config) { return config.get() == edit.get(); }));
    edit->setName(QStringLiteral("Validation A"));
    edit->setHighLatency(true);
    QVERIFY(linkManager()->endConfigurationEditing(first.get(), edit.get()));
    QVERIFY(first->isHighLatency());
    QCOMPARE(linkManager()->_rgLinkConfigs.size(), count);
    edit.release();
}

void LinkManagerTest::_testUdpDisconnectEditReconnectOnNewPort()
{
    const quint16 portA = _unusedUdpPort();
    QVERIFY(portA);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("EditReconnectUdp"), portA);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    QVERIFY(config->linkActive());
    QVERIFY(!_udpPortIsFree(portA));

    // Allocate B while A is still bound so the ephemeral allocator cannot give us A again.
    const quint16 portB = _unusedUdpPort();
    QVERIFY(portB);
    QVERIFY(portA != portB);
    linkManager()->disconnectLinkConfiguration(config.get());
    QTRY_VERIFY_WITH_TIMEOUT(!config->link() && !config->linkActive(), TestTimeout::mediumMs());
    QVERIFY(_udpPortIsFree(portA));

    std::unique_ptr<LinkConfiguration> editedConfig(linkManager()->startConfigurationEditing(config.get()));
    auto* const editedUdpConfig = qobject_cast<UDPConfiguration*>(editedConfig.get());
    QVERIFY(editedUdpConfig);
    editedUdpConfig->setLocalPort(portB);
    linkManager()->endConfigurationEditing(config.get(), editedConfig.release());
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    QVERIFY(config->linkActive());
    QVERIFY(_udpPortIsFree(portA));
    QVERIFY(!_udpPortIsFree(portB));
}

void LinkManagerTest::_testUdpDisconnectWhilePendingStartup()
{
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("PendingDisconnectUdp"), port);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    LinkManagerUdpStartupGate startupGate;
    QVERIFY(linkManager()->createConnectedLink(config));
    const SharedLinkInterfacePtr link = linkManager()->sharedLinkInterfacePointerForLink(config->link());
    QVERIFY(link);
    QTRY_VERIFY_WITH_TIMEOUT(startupGate.entered(), TestTimeout::mediumMs());
    QVERIFY(!link->isConnected());

    linkManager()->disconnectLinkConfiguration(config.get());
    QVERIFY(startupGate.releaseAndDrain());
    QTRY_VERIFY_WITH_TIMEOUT(startupGate.drained(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(_udpLinksForConfig(config).isEmpty(), TestTimeout::mediumMs());
    QVERIFY(!config->link());
    QVERIFY(!config->linkActive());
    QVERIFY(_udpPortIsFree(port));
}

void LinkManagerTest::_testUdpRemoveConfigurationWhilePendingDoesNotOrphan()
{
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("PendingRemoveUdp"), port);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    LinkManagerUdpStartupGate startupGate;
    QVERIFY(linkManager()->createConnectedLink(config));
    const SharedLinkInterfacePtr link = linkManager()->sharedLinkInterfacePointerForLink(config->link());
    QVERIFY(link);
    QTRY_VERIFY_WITH_TIMEOUT(startupGate.entered(), TestTimeout::mediumMs());
    QVERIFY(!link->isConnected());

    linkManager()->removeConfiguration(config.get());
    QVERIFY(!linkManager()->containsConfiguration(config->name()));
    QVERIFY(startupGate.releaseAndDrain());
    QTRY_VERIFY_WITH_TIMEOUT(startupGate.drained(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(_udpLinksForConfig(config).isEmpty(), TestTimeout::mediumMs());
    QVERIFY(!config->link());
    QVERIFY(!config->linkActive());
    QVERIFY(_udpPortIsFree(port));
}

void LinkManagerTest::_testCreateConnectedLinkTwiceCreatesOneLink_data()
{
    QTest::addColumn<bool>("pending");
    QTest::newRow("connected") << false;
    QTest::newRow("pending") << true;
}

void LinkManagerTest::_testCreateConnectedLinkTwiceCreatesOneLink()
{
    QFETCH(bool, pending);
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("DuplicateConnectUdp"), port);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    LinkManagerUdpStartupGate startupGate(pending);
    QVERIFY(linkManager()->createConnectedLink(config));
    if (pending) {
        QTRY_VERIFY_WITH_TIMEOUT(startupGate.entered(), TestTimeout::mediumMs());
        QVERIFY(config->link());
        QVERIFY(!config->link()->isConnected());
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    }

    // Either a successful no-op or a rejected duplicate is acceptable; the first link must survive.
    (void) linkManager()->createConnectedLink(config);
    const auto registeredLinks = _udpLinksForConfig(config);
    const qsizetype registeredCount = registeredLinks.size();
    if (pending) {
        QTRY_VERIFY_WITH_TIMEOUT(startupGate.entered(), TestTimeout::mediumMs());
        QVERIFY(config->link());
        QVERIFY(!config->link()->isConnected());
        QVERIFY(startupGate.releaseAndDrain());
        QTRY_VERIFY_WITH_TIMEOUT(startupGate.drained(), TestTimeout::mediumMs());
    }
    QTRY_VERIFY_WITH_TIMEOUT(std::ranges::all_of(registeredLinks, [](const auto& link) { return link->isConnected(); }),
                             TestTimeout::mediumMs());

    linkManager()->disconnectLinkConfiguration(config.get());
    // Exercise disconnect even on the known-bad duplicate path before reporting the count failure.
    const bool disconnected = QTest::qWaitFor(
        [this, &config] { return _udpLinksForConfig(config).isEmpty() && !config->link() && !config->linkActive(); },
        TestTimeout::mediumMs());
    const bool portFree = _udpPortIsFree(port);
    QCOMPARE(registeredCount, qsizetype{1});
    QVERIFY(disconnected);
    QVERIFY(portFree);
}

void LinkManagerTest::_testUdpWriteQueuedAfterDisconnectIsSilent()
{
    Fact* const sendHeartbeat = SettingsManager::instance()->mavlinkSettings()->sendGCSHeartbeat();
    const QVariant oldSendHeartbeat = sendHeartbeat->rawValue();
    const auto restoreHeartbeat =
        qScopeGuard([sendHeartbeat, oldSendHeartbeat] { sendHeartbeat->setRawValue(oldSendHeartbeat); });
    sendHeartbeat->setRawValue(false);

    QUdpSocket peer;
    QVERIFY(peer.bind(QHostAddress::LocalHost, 0, QAbstractSocket::DontShareAddress));
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("QueuedWriteDisconnectUdp"), port);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    auto* const udpConfig = qobject_cast<UDPConfiguration*>(config.get());
    QVERIFY(udpConfig);
    udpConfig->addHost(QStringLiteral("127.0.0.1"), peer.localPort());
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    const SharedLinkInterfacePtr link = linkManager()->sharedLinkInterfacePointerForLink(config->link());
    QVERIFY(link);
    QSignalSpy errorSpy(link.get(), &LinkInterface::communicationError);
    QSignalSpy sentSpy(link.get(), &LinkInterface::bytesSent);
    QVERIFY(errorSpy.isValid());
    QVERIFY(sentSpy.isValid());

    const QByteArray payload("udp-queued-write");
    link->writeBytesThreadSafe(payload.constData(), static_cast<int>(payload.size()));
    QTRY_VERIFY_WITH_TIMEOUT(peer.hasPendingDatagrams(), TestTimeout::mediumMs());
    QCOMPARE(peer.receiveDatagram().data(), payload);
    QTRY_COMPARE_WITH_TIMEOUT(sentSpy.count(), 1, TestTimeout::mediumMs());

    auto* const workerThread = link->findChild<QThread*>();
    QVERIFY(workerThread);
    LinkManagerUdpStartupGate workerGate(false);
    QVERIFY(workerGate.holdWorker(workerThread));
    QTRY_VERIFY_WITH_TIMEOUT(workerGate.entered(), TestTimeout::mediumMs());

    // Keep the connected state visible to the producer until D and W are both queued.
    linkManager()->disconnectLinkConfiguration(config.get());
    QVERIFY(link->isConnected());
    link->writeBytesThreadSafe(payload.constData(), static_cast<int>(payload.size()));
    QVERIFY(workerGate.releaseAndDrain());
    QTRY_VERIFY_WITH_TIMEOUT(workerGate.drained(), TestTimeout::mediumMs());

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(sentSpy.count(), 1);
    QVERIFY(!link->isConnected());
    QTRY_VERIFY_WITH_TIMEOUT(_udpLinksForConfig(config).isEmpty() && !config->link() && !config->linkActive(),
                             TestTimeout::mediumMs());
    linkManager()->removeConfiguration(config.get());
    QVERIFY(!linkManager()->containsConfiguration(config->name()));
    QVERIFY(_udpPortIsFree(port));
}

void LinkManagerTest::_testUdpDisconnectWithTrafficAndLearnedTarget()
{
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("TrafficDisconnectUdp"), port);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    QSignalSpy receivedSpy(config->link(), &LinkInterface::bytesReceived);
    QVERIFY(receivedSpy.isValid());
    QUdpSocket peer;
    QVERIFY(peer.bind(QHostAddress::LocalHost, 0, QAbstractSocket::DontShareAddress));
    const QByteArray payload("udp-session-target");
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(peer.writeDatagram(payload, QHostAddress::LocalHost, port), qint64(payload.size()));
    }
    const auto receivedBytes = [&receivedSpy] {
        qsizetype size = 0;
        for (const auto& arguments : receivedSpy) {
            size += arguments.at(1).toByteArray().size();
        }
        return size;
    };
    QTRY_COMPARE_WITH_TIMEOUT(receivedBytes(), payload.size() * 3, TestTimeout::mediumMs());

    // No configured targets: a reply can reach peer only through the learned session target.
    const QByteArray reply("udp-learned-target-reply");
    config->link()->writeBytesThreadSafe(reply.constData(), static_cast<int>(reply.size()));
    bool replyReceived = false;
    const auto receivedReply = [&peer, &reply, &replyReceived] {
        while (peer.hasPendingDatagrams()) {
            if (peer.receiveDatagram().data() == reply) {
                replyReceived = true;
            }
        }
        return replyReceived;
    };
    QTRY_VERIFY_WITH_TIMEOUT(receivedReply(), TestTimeout::mediumMs());

    linkManager()->disconnectLinkConfiguration(config.get());
    QTRY_VERIFY_WITH_TIMEOUT(!config->linkActive() && !config->link(), TestTimeout::mediumMs());
    QVERIFY(_udpLinksForConfig(config).isEmpty());
    QVERIFY(_udpPortIsFree(port));
}

void LinkManagerTest::_testUdpAutoConnectStartedLinkDisconnectSuppresses()
{
    const quint16 port = _unusedUdpPort();
    QVERIFY(port);
    SharedLinkConfigurationPtr config = _addUdpConfig(QStringLiteral("StartedAutoConnectUdp"), port);
    const auto cleanupConfig = qScopeGuard([this, config] { _cleanupUdpConfig(config); });
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    QVERIFY(linkManager()->createConnectedLink(config));
    QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
    QVERIFY(config->linkActive());

    linkManager()->disconnectLinkConfiguration(config.get());
    QTRY_VERIFY_WITH_TIMEOUT(!config->linkActive() && !config->link(), TestTimeout::mediumMs());
    QVERIFY(config->suppressAutoReconnect());
    for (int tick = 0; tick < 3; ++tick) {
        _reconnect();
        QVERIFY(!config->link());
        QVERIFY(!config->linkActive());
        QVERIFY(_udpLinksForConfig(config).isEmpty());
    }
    QVERIFY(_udpPortIsFree(port));
}

void LinkManagerTest::_testUdpAutoConnectBindFailureReusesConfig()
{
    linkManager()->init();
    QUdpSocket blocker;
    QVERIFY(blocker.bind(QHostAddress::AnyIPv4, 0, QAbstractSocket::DontShareAddress));

    AutoConnectSettings* const autoConnectSettings = SettingsManager::instance()->autoConnectSettings();
    const QVariant oldAutoConnectUDP = autoConnectSettings->autoConnectUDP()->rawValue();
    const QVariant oldListenPort = autoConnectSettings->udpListenPort()->rawValue();
    const QVariant oldTargetHost = autoConnectSettings->udpTargetHostIP()->rawValue();
    const auto restoreSettings = qScopeGuard([&] {
        autoConnectSettings->autoConnectUDP()->setRawValue(oldAutoConnectUDP);
        autoConnectSettings->udpListenPort()->setRawValue(oldListenPort);
        autoConnectSettings->udpTargetHostIP()->setRawValue(oldTargetHost);
    });
    autoConnectSettings->autoConnectUDP()->setRawValue(true);
    autoConnectSettings->udpListenPort()->setRawValue(blocker.localPort());
    autoConnectSettings->udpTargetHostIP()->setRawValue(QString());

    const auto defaultConfigs = [this] { return _configsNamed(LinkManager::_defaultUDPLinkName); };
    QVERIFY(defaultConfigs().isEmpty());

    _expectUdpBindFailureLogs();
    linkManager()->_addUDPAutoConnectLink();
    QCOMPARE(defaultConfigs().size(), 1);
    const SharedLinkConfigurationPtr config = defaultConfigs().constFirst();
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    _verifyUdpBindFailureLogs();

    // First retry reuses the same config once the backoff allows it
    QTRY_VERIFY_WITH_TIMEOUT(config->reconnectReady(), TestTimeout::mediumMs());
    _expectUdpBindFailureLogs();
    linkManager()->_addUDPAutoConnectLink();
    QCOMPARE(defaultConfigs().size(), 1);
    QVERIFY(config->link());
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    _verifyUdpBindFailureLogs();

    // Next retry waits for the backoff
    linkManager()->_addUDPAutoConnectLink();
    QVERIFY(config->link() == nullptr);
    QCOMPARE(defaultConfigs().size(), 1);

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testForwardingLinkUnresolvedHostRetries()
{
    linkManager()->init();
    MavlinkSettings* const mavlinkSettings = SettingsManager::instance()->mavlinkSettings();
    const QVariant oldForward = mavlinkSettings->forwardMavlink()->rawValue();
    const QVariant oldHost = mavlinkSettings->forwardMavlinkHostName()->rawValue();
    const auto restoreSettings = qScopeGuard([&] {
        mavlinkSettings->forwardMavlink()->setRawValue(oldForward);
        mavlinkSettings->forwardMavlinkHostName()->setRawValue(oldHost);
    });
    mavlinkSettings->forwardMavlink()->setRawValue(true);
    expectAppMessage(QRegularExpression(QStringLiteral("Restart application")));
    mavlinkSettings->forwardMavlinkHostName()->setRawValue(QStringLiteral("drone.invalid:14445"));
    verifyExpectedLogMessage();

    const auto forwardingConfigs = [this] { return _configsNamed(LinkManager::_mavlinkForwardingLinkName); };
    QVERIFY(forwardingConfigs().isEmpty());

    const QRegularExpression unresolved(QStringLiteral("Could not resolve host: drone.invalid"));
    expectLogMessage("Comms.UDPLink", QtWarningMsg, unresolved);
    expectAppMessage(unresolved);
    linkManager()->_addMAVLinkForwardingLink();
    QCOMPARE(forwardingConfigs().size(), 1);
    const SharedLinkConfigurationPtr config = forwardingConfigs().constFirst();
    QVERIFY(config->isForwarding());
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();

    // Retry reuses the config, resolves again, and does not pop up again
    QTRY_VERIFY_WITH_TIMEOUT(config->reconnectReady(), TestTimeout::mediumMs());
    expectLogMessage("Comms.UDPLink", QtWarningMsg, unresolved);
    linkManager()->_addMAVLinkForwardingLink();
    QCOMPARE(forwardingConfigs().size(), 1);
    QVERIFY(config->link());
    QVERIFY(config->isForwarding());
    // Checked before the lookup completes so slow DNS can't outlast the backoff
    QVERIFY(!config->reconnectReady());
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    verifyExpectedLogMessage();

    // A new failure streak (backoff reset, as after a stable connection) pops up again
    config->resetReconnectBackoff();
    expectLogMessage("Comms.UDPLink", QtWarningMsg, unresolved);
    expectAppMessage(unresolved);
    linkManager()->_addMAVLinkForwardingLink();
    QTRY_VERIFY_WITH_TIMEOUT(config->link() == nullptr, TestTimeout::mediumMs());
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();

    linkManager()->removeConfiguration(config.get());
}

void LinkManagerTest::_testSupportForwardingFailureAllowsRetry()
{
    MavlinkSettings* const mavlinkSettings = SettingsManager::instance()->mavlinkSettings();
    const QVariant oldHost = mavlinkSettings->forwardMavlinkAPMSupportHostName()->rawValue();
    const auto restoreSettings =
        qScopeGuard([&] { mavlinkSettings->forwardMavlinkAPMSupportHostName()->setRawValue(oldHost); });
    mavlinkSettings->forwardMavlinkAPMSupportHostName()->setRawValue(QStringLiteral("drone.invalid:14445"));

    const auto supportConfigs = [this] { return _configsNamed(LinkManager::_mavlinkForwardingSupportLinkName); };
    QVERIFY(supportConfigs().isEmpty());

    const QRegularExpression unresolved(QStringLiteral("Could not resolve host: drone.invalid"));
    for (int attempt = 0; attempt < 2; attempt++) {
        expectLogMessage("Comms.UDPLink", QtWarningMsg, unresolved);
        expectAppMessage(unresolved);
        linkManager()->createMavlinkForwardingSupportLink();
        QTRY_VERIFY_WITH_TIMEOUT(!linkManager()->mavlinkSupportForwardingEnabled(), TestTimeout::mediumMs());
        QCOMPARE(supportConfigs().size(), 1);
        QVERIFY(supportConfigs().constFirst()->link() == nullptr);
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }

    linkManager()->removeConfiguration(supportConfigs().constFirst().get());
}

void LinkManagerTest::_testDynamicUdpLinkIgnoresSameNamedUserLink()
{
    linkManager()->init();
    MavlinkSettings* const mavlinkSettings = SettingsManager::instance()->mavlinkSettings();
    const QVariant oldForward = mavlinkSettings->forwardMavlink()->rawValue();
    const auto restoreSettings = qScopeGuard([&] { mavlinkSettings->forwardMavlink()->setRawValue(oldForward); });
    mavlinkSettings->forwardMavlink()->setRawValue(true);

    UDPConfiguration* const userUdpConfig =
        new UDPConfiguration(QLatin1String(LinkManager::_mavlinkForwardingLinkName));
    userUdpConfig->setDynamic(false);
    const SharedLinkConfigurationPtr userConfig = linkManager()->addConfiguration(userUdpConfig);

    linkManager()->_addMAVLinkForwardingLink();

    QVERIFY(!userConfig->isDynamic());
    QVERIFY(!userConfig->isForwarding());
    QVERIFY(userConfig->link() == nullptr);
    const QList<SharedLinkConfigurationPtr> configs = _configsNamed(LinkManager::_mavlinkForwardingLinkName);
    QCOMPARE(configs.size(), 2);

    // Let the UDP link connect before removal to keep this test focused on name handling.
    const auto dynamicIt =
        std::ranges::find_if(configs, [](const SharedLinkConfigurationPtr& c) { return c->isDynamic(); });
    QVERIFY(dynamicIt != configs.cend());
    const SharedLinkConfigurationPtr dynamicConfig = *dynamicIt;
    QTRY_VERIFY_WITH_TIMEOUT(dynamicConfig->link() && dynamicConfig->link()->isConnected(), TestTimeout::mediumMs());

    for (const SharedLinkConfigurationPtr& config : configs) {
        linkManager()->removeConfiguration(config.get());
    }
    QTRY_VERIFY_WITH_TIMEOUT(dynamicConfig->link() == nullptr, TestTimeout::mediumMs());
}

QList<SharedLinkConfigurationPtr> LinkManagerTest::_configsNamed(const char* name)
{
    QList<SharedLinkConfigurationPtr> configs;
    for (const SharedLinkConfigurationPtr& config : linkManager()->_rgLinkConfigs) {
        if (config->name() == QLatin1String(name)) {
            configs.append(config);
        }
    }
    return configs;
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
    linkManager()->_serialAutoConnect->_waitingPorts._deadlinesUs[location] = 0;
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
    const auto owner = SerialPortManager::instance()->reservationInfo(location);
    QVERIFY(owner);
    QCOMPARE(owner->kind, SerialPortManager::OwnerKind::MAVLink);
    QCOMPARE(owner->label, config->name());
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

namespace {
class LinkManagerDiagnosticConfiguration : public UDPConfiguration
{
public:
    explicit LinkManagerDiagnosticConfiguration(LinkType linkType)
        : UDPConfiguration(QStringLiteral("Diagnostic"))
        , _type(linkType)
    {}

    LinkType type() const override { return _type; }

private:
    LinkType _type;
};

class LinkManagerDiagnosticLink : public LinkInterface
{
public:
    explicit LinkManagerDiagnosticLink(SharedLinkConfigurationPtr& config)
        : LinkInterface(config)
    {}

    bool isConnected() const override { return _connected; }

    void reportConnected()
    {
        _connected = true;
        emit connected();
    }

    bool holdDisconnect = false;

    void disconnect() override
    {
        if (!holdDisconnect) {
            _connected = false;
            emit disconnected();
        }
    }

private:
    bool _connect() override { return false; }

    void _writeBytes(const QByteArray&) override {}

    bool _connected = false;
};
}  // namespace

void LinkManagerTest::_testRetryGateDiagnostics()
{
    LinkManager manager;
    auto config = manager.addConfiguration(new LinkManagerDiagnosticConfiguration(LinkConfiguration::TypeLast));
    const auto cleanup = qScopeGuard([&] { manager._removeConfiguration(config.get()); });
    auto link = std::make_shared<LinkManagerDiagnosticLink>(config);
    config->setLink(link);
    config->setSuppressAutoReconnect(true);
    ScopedConnectionLogCapture capture("Comms.LinkManager", "link_manager ");
    const auto checkGate = [&](const QString& token, qsizetype count) {
        for (int tick = 0; tick < 4; ++tick) {
            if (token == QStringLiteral("retry_pending")) {
                config->noteReconnectAttempt();
            }
            manager._reconnectAutoConnectLinks();
        }
        QCOMPARE(capture.messages().size(), count);
        QCOMPARE(capture.messages().last(),
                 QStringLiteral("link_manager event=retry_skip config=\"Diagnostic\" port=\"unknown\" reason=%1")
                     .arg(token));
    };
    checkGate(QStringLiteral("auto_connect_off"), 1);
    config->setAutoConnect(true);
    checkGate(QStringLiteral("has_link"), 2);
    config->setLink(nullptr);
    checkGate(QStringLiteral("suppressed"), 3);
    config->setSuppressAutoReconnect(false);
    checkGate(QStringLiteral("never_started"), 4);
    config->setAutoConnectStarted(true);
    config->noteReconnectAttempt();
    checkGate(QStringLiteral("retry_pending"), 5);
    config->resetReconnectBackoff();
    manager._reconnectAutoConnectLinks();
    QCOMPARE(capture.messages().size(), 7);
    QCOMPARE(capture.messages()[5],
             QStringLiteral("link_manager event=retry_clear config=\"Diagnostic\" port=\"unknown\" cause=ready"));
    QCOMPARE(capture.messages()[6], QStringLiteral("link_manager event=create_failed config=\"Diagnostic\" "
                                                   "port=\"unknown\" reason=dispatch_failed"));
    QCOMPARE(config->reconnectAttempts(), 1);
    QVERIFY(!config->link());
    manager._reconnectAutoConnectLinks();
    QCOMPARE(capture.messages().size(), 8);
    const QString pending =
        QStringLiteral("link_manager event=retry_skip config=\"Diagnostic\" port=\"unknown\" reason=retry_pending");
    QCOMPARE(capture.messages().last(), pending);
    config->setDynamic(true);
    manager._reconnectAutoConnectLinks();
    QVERIFY(manager._retryGateStates.isEmpty());
    QCOMPARE(capture.messages().size(), 8);
    config->setDynamic(false);
    manager._reconnectAutoConnectLinks();
    QCOMPARE(capture.messages().size(), 9);
    QCOMPARE(capture.messages().last(), pending);
    manager._removeConfiguration(config.get());
    QVERIFY(manager._retryGateStates.isEmpty());
    // A replacement configuration with the same name starts a fresh diagnostic lifetime.
    config = manager.addConfiguration(new LinkManagerDiagnosticConfiguration(LinkConfiguration::TypeLast));
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    config->noteReconnectAttempt();
    manager._reconnectAutoConnectLinks();
    QCOMPARE(capture.messages().size(), 10);
    QCOMPARE(capture.messages().last(), pending);
}

void LinkManagerTest::_testSuspensionDiagnostics()
{
    LinkManager manager;
    ScopedConnectionLogCapture capture("Comms.LinkManager", "link_manager ");
    manager.setConnectionsAllowed();
    QVERIFY(capture.messages().isEmpty());
    manager.setConnectionsSuspended(QStringLiteral("Firmware \"update\""));
    for (int tick = 0; tick < 4; ++tick) {
        manager.setConnectionsSuspended(QStringLiteral("Firmware \"update\""));
        manager._updateAutoConnectLinks();
    }
    QCOMPARE(capture.messages(),
             QStringList{QStringLiteral("link_manager event=suspended reason=\"Firmware \\\"update\\\"\"")});
    QVERIFY(manager.connectionsSuspended());
    manager.setConnectionsSuspended(QStringLiteral("Shutdown"));
    QCOMPARE(capture.messages().size(), 2);
    QCOMPARE(capture.messages().last(), QStringLiteral("link_manager event=suspended reason=\"Shutdown\""));
    manager.setConnectionsAllowed();
    manager.setConnectionsAllowed();
    QVERIFY(!manager.connectionsSuspended());
    QCOMPARE(capture.messages().size(), 3);
    QCOMPARE(capture.messages().last(), QStringLiteral("link_manager event=resumed reason=\"Shutdown\""));
}

void LinkManagerTest::_testCreationDiagnostics()
{
    LinkManager manager;
    SharedLinkConfigurationPtr config =
        std::make_shared<LinkManagerDiagnosticConfiguration>(LinkConfiguration::TypeLast);
    auto link = std::make_shared<LinkManagerDiagnosticLink>(config);
    config->setLink(link);
    ScopedConnectionLogCapture capture("Comms.LinkManager", "link_manager ");
    config->setSuppressAutoReconnect(true);
    QVERIFY(manager.createConnectedLink(config));
    QVERIFY(!config->suppressAutoReconnect());
    QCOMPARE(config->link(), link.get());
    QCOMPARE(capture.messages().size(), 1);
    QCOMPARE(capture.messages().last(),
             QStringLiteral("link_manager event=create_failed config=\"Diagnostic\" port=\"unknown\" reason=has_link"));
    config->setLink(nullptr);
    QVERIFY(!manager.createConnectedLink(config));
    QCOMPARE(capture.messages().size(), 2);
    QCOMPARE(capture.messages().last(), QStringLiteral("link_manager event=create_failed config=\"Diagnostic\" "
                                                       "port=\"unknown\" reason=dispatch_failed"));
#ifndef QGC_NO_SERIAL_LINK
    config = std::make_shared<LinkManagerDiagnosticConfiguration>(LinkConfiguration::TypeSerial);
    QVERIFY(!manager.createConnectedLink(config));
    QCOMPARE(capture.messages().size(), 3);
    QCOMPARE(capture.messages().last(), QStringLiteral("link_manager event=create_failed config=\"Diagnostic\" "
                                                       "port=\"unknown\" reason=invalid_serial_configuration"));
    const QString port = QStringLiteral("/test/diagnostic-claim");
    auto reservation = SerialPortManager::instance()->reservePort(port, SerialPortManager::OwnerKind::GPSReceiver,
                                                                  QStringLiteral("GNSS receiver"));
    QVERIFY(reservation);
    auto serial = std::make_shared<SerialConfiguration>(QStringLiteral("Telemetry"));
    serial->setPortName(port);
    config = serial;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QVERIFY(!manager.createConnectedLink(config));
    }
    QCOMPARE(capture.messages().size(), 5);
    QCOMPARE(capture.messages().last(), QStringLiteral("link_manager event=create_failed config=\"Telemetry\" "
                                                       "port=\"/test/diagnostic-claim\" reason=reservation_failed"));
    QVERIFY(!config->link());
    QCOMPARE(SerialPortManager::instance()->reservationInfo(port)->label, QStringLiteral("GNSS receiver"));
    reservation.reset();
#endif
    auto* const globalManager = linkManager();
    const auto channels = globalManager->_mavlinkChannelsUsedBitMask;
    const auto restoreChannels = qScopeGuard([&] { globalManager->_mavlinkChannelsUsedBitMask = channels; });
    globalManager->_mavlinkChannelsUsedBitMask = std::numeric_limits<uint32_t>::max();
    config = std::make_shared<UDPConfiguration>(QStringLiteral("No channels"));
    expectLogMessage("Comms.LinkManager", QtWarningMsg, QRegularExpression(QStringLiteral("all channels reserved")));
    expectLogMessage("Comms.LinkInterface", QtWarningMsg, QRegularExpression(QStringLiteral("^failed$")));
    expectLogMessage("Comms.LinkManager", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Link failed to setup mavlink channels")));
    const qsizetype before = capture.messages().size();
    QVERIFY(!manager.createConnectedLink(config));
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();
    verifyExpectedLogMessage();
    QCOMPARE(capture.messages().size(), before + 1);
    QCOMPARE(capture.messages().last(), QStringLiteral("link_manager event=create_failed config=\"No channels\" "
                                                       "port=\"unknown\" reason=no_mavlink_channel"));
    QVERIFY(!config->link());
}

#ifndef QGC_NO_SERIAL_LINK
void LinkManagerTest::_testSerialLossDiagnostics_data()
{
    QTest::addColumn<QString>("policy");
    QTest::addColumn<bool>("intentional");
    QTest::newRow("manual-loss") << QStringLiteral("auto_connect_off") << false;
    QTest::newRow("retry-loss") << QStringLiteral("retry_enabled") << false;
    QTest::newRow("discovery-loss") << QStringLiteral("dynamic") << false;
    QTest::newRow("never-started-loss") << QStringLiteral("never_started") << false;
    QTest::newRow("suppressed-loss") << QStringLiteral("suppressed") << false;
    QTest::newRow("explicit-disconnect") << QStringLiteral("retry_enabled") << true;
    QTest::newRow("explicit-link-disconnect") << QStringLiteral("disconnect-link") << true;
    QTest::newRow("disconnect-during-open") << QStringLiteral("pending-disconnect") << true;
    QTest::newRow("shutdown") << QStringLiteral("shutdown") << true;
    QTest::newRow("remove-config") << QStringLiteral("remove") << true;
    QTest::newRow("unverified-open") << QStringLiteral("unverified") << false;
}

void LinkManagerTest::_testSerialLossDiagnostics()
{
    QFETCH(QString, policy);
    QFETCH(bool, intentional);
    LinkManager manager;
    auto* const serial = new SerialConfiguration(QStringLiteral("Loss"));
    serial->setPortName(QStringLiteral("/test/loss"));
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(policy != QStringLiteral("auto_connect_off"));
    config->setAutoConnectStarted(policy != QStringLiteral("never_started") && policy != QStringLiteral("dynamic"));
    config->setDynamic(policy == QStringLiteral("dynamic"));
    config->setSuppressAutoReconnect(policy == QStringLiteral("suppressed"));
    auto link = std::make_shared<LinkManagerDiagnosticLink>(config);
    config->setLink(link);
    manager._rgLinks.append(link);
    manager._serialConnectionStates.insert(link.get(), {});
    (void) connect(link.get(), &LinkInterface::connected, &manager, &LinkManager::_linkConnected);
    (void) connect(link.get(), &LinkInterface::disconnected, &manager, &LinkManager::_linkDisconnected);
    const auto cleanup = qScopeGuard([&] {
        manager.disconnectAll();
        if (manager._rgLinkConfigs.contains(config)) {
            manager._removeConfiguration(config.get());
        }
    });
    ScopedConnectionLogCapture capture("Comms.LinkManager", "link_manager ");
    if (policy != QStringLiteral("unverified") && policy != QStringLiteral("pending-disconnect")) {
        link->reportConnected();
    }
    if (policy == QStringLiteral("pending-disconnect")) {
        link->holdDisconnect = true;
        manager.disconnectLinkConfiguration(config.get());
        link->reportConnected();
        link->holdDisconnect = false;
        link->disconnect();
    } else if (policy == QStringLiteral("shutdown")) {
        manager.disconnectAll();
    } else if (policy == QStringLiteral("remove")) {
        manager.removeConfiguration(config.get());
    } else if (policy == QStringLiteral("disconnect-link")) {
        manager.disconnectLink(link.get());
    } else if (intentional) {
        manager.disconnectLinkConfiguration(config.get());
    } else {
        link->disconnect();
    }
    const bool reportsLoss = !intentional && policy != QStringLiteral("unverified");
    QCOMPARE(capture.messages().size(), reportsLoss ? 1 : 0);
    if (reportsLoss) {
        QCOMPARE(capture.messages().first(),
                 QStringLiteral("link_manager event=serial_loss config=\"Loss\" port=\"/test/loss\" recovery=%1")
                     .arg(policy == QStringLiteral("dynamic") ? QStringLiteral("retry_enabled") : policy));
    }
    QVERIFY(!config->link());
    QVERIFY(manager.links().isEmpty());
    QVERIFY(manager._serialConnectionStates.isEmpty());
    link->disconnect();
    QCOMPARE(capture.messages().size(), reportsLoss ? 1 : 0);
}
#endif

#ifndef QGC_NO_SERIAL_LINK
void LinkManagerTest::_testSavedSerialOwnerClearsDiscoverySkip()
{
    LinkManager manager;
    manager.init();
    const SerialPortManager::Port port{QStringLiteral("/test/saved-live"), QStringLiteral("saved-live"),
                                       QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    auto claim = SerialPortManager::instance()->reservePort(port.systemLocation, SerialPortManager::OwnerKind::MAVLink,
                                                            QStringLiteral("Saved live"));
    QVERIFY(claim);
    auto* const serial = new SerialConfiguration(QStringLiteral("Saved live"));
    serial->setPortName(port.systemLocation);
    auto config = manager.addConfiguration(serial);
    auto link = std::make_shared<LinkManagerDiagnosticLink>(config);
    config->setLink(link);
    manager._rgLinks.append(link);
    const auto cleanup = qScopeGuard([&] {
        manager._rgLinks.clear();
        manager._removeConfiguration(config.get());
    });
    ScopedConnectionLogCapture capture("Comms.Serial.SerialAutoConnect", "serial_auto_connect ");
    manager._serialAutoConnect->update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 1);
    QVERIFY(capture.messages().last().contains(QStringLiteral("reason=port_reserved")));
    link->reportConnected();
    for (int tick = 0; tick < 5; ++tick) {
        manager._serialAutoConnect->update({port}, {.pixhawk = true});
    }
    QCOMPARE(capture.messages().size(), 2);
    QCOMPARE(capture.messages().last(),
             QStringLiteral("serial_auto_connect event=clear port=\"/test/saved-live\" cause=connected"));
    QVERIFY(manager._serialAutoConnect->_configs.isEmpty());
    QVERIFY(manager._serialAutoConnect->_waitingPorts.isEmpty());
}
#endif
