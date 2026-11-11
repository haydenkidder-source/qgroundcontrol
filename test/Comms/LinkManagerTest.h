#pragma once

#include "CommsTest.h"
#include "LinkConfiguration.h"

/// Tests for LinkManager::_reconnectAutoConnectLinks() — the auto-connect link
/// re-establishment driven off the auto-connect timer (PR #14547).
class LinkManagerTest : public CommsTest
{
    Q_OBJECT

private slots:
    void _testUdpAutoConnectDefaults();
#ifndef QGC_NO_SERIAL_LINK
    void _testOccupiedSerialAutoConnectRecovers();
    void _testReservedSerialPortNotOpened();
    void _testSerialReservationFollowsLink();
#endif
    void _testReconnectsDroppedAutoConnectLink();
    void _testSuppressedLinkNotReconnected();
    void _testDynamicLinkNotReconnected();
    void _testNonAutoConnectLinkNotReconnected();
    void _testNeverStartedLinkNotConnected();
    void _testLinkActiveStableAcrossReconnect();
    void _testStaleBytesNotAttributedToRecycledLinkAddress();
    void _testUdpUnresolvedHostFailsConnect();
    void _testUdpUnresolvedHostAllowedStaysConnected();
    void _testUdpBindFailureDisconnects();
    void _testUdpDisconnectEditReconnectOnNewPort();
    void _testUdpDisconnectWhilePendingStartup();
    void _testUdpRemoveConfigurationWhilePendingDoesNotOrphan();
    void _testCreateConnectedLinkTwiceCreatesOneLink_data();
    void _testCreateConnectedLinkTwiceCreatesOneLink();
    void _testUdpDisconnectWithTrafficAndLearnedTarget();
    void _testUdpAutoConnectStartedLinkDisconnectSuppresses();
    void _testUdpAutoConnectBindFailureReusesConfig();
    void _testForwardingLinkUnresolvedHostRetries();
    void _testSupportForwardingFailureAllowsRetry();
    void _testDynamicUdpLinkIgnoresSameNamedUserLink();

private:
    SharedLinkConfigurationPtr _addMockConfig(const QString &name, bool dynamic, bool autoConnect);
    void _reconnect();
    void _expectUdpBindFailureLogs();
    void _verifyUdpBindFailureLogs();
    static quint16 _unusedUdpPort();
    static bool _udpPortIsFree(quint16 port);
    SharedLinkConfigurationPtr _addUdpConfig(const QString& name, quint16 port);
    QList<SharedLinkInterfacePtr> _udpLinksForConfig(const SharedLinkConfigurationPtr& config);
    void _cleanupUdpConfig(const SharedLinkConfigurationPtr& config);
    QList<SharedLinkConfigurationPtr> _configsNamed(const char* name);
};
