#pragma once

#include <functional>

#include "CommsTest.h"
#include "LinkConfiguration.h"
#ifndef QGC_NO_SERIAL_LINK
#include "SerialPortManager.h"
struct SerialOpenOutcome;
#endif

class LinkManager;

/// Tests for LinkManager::_reconnectAutoConnectLinks() — the auto-connect link
/// re-establishment driven off the auto-connect timer (PR #14547).
class LinkManagerTest : public CommsTest
{
    Q_OBJECT

public:
#ifndef QGC_NO_SERIAL_LINK
    // Shared observation fixtures for the custom serial-notice UI tests.
    static SharedLinkInterfacePtr _serialNoticeLink(LinkManager& manager, SharedLinkConfigurationPtr& config,
                                                    bool allocateChannel = false);
    static void reportSerialConnected(LinkInterface* link);
    static void observeSerialPorts(LinkManager& manager, const QList<SerialPortManager::Port>& ports);
    static void reportSerialOpenFailure(LinkManager& manager, LinkConfiguration* config,
                                        const SerialOpenOutcome& outcome);
    static std::function<quint64()> exchangeSerialNoticeClock(LinkManager& manager, std::function<quint64()> clock);
#endif

private slots:
    void _testRetryGateDiagnostics();
    void _testSuspensionDiagnostics();
    void _testCreationDiagnostics();
    void _testUdpAutoConnectDefaults();
#ifndef QGC_NO_SERIAL_LINK
    void _testSerialNoticeLoss_data();
    void _testSerialNoticeLoss();
    void _testSerialNoticeWorkerReasons_data();
    void _testSerialNoticeWorkerReasons();
    void _testSerialNoticeTransitions();
    void _testSerialNoticeEscalationAfterLoss();
    void _testSerialNoticeEscalationAfterCreationFailure();
    void _testSerialNoticeDiscovery_data();
    void _testSerialNoticeDiscovery();
    void _testSerialNoticeOwners_data();
    void _testSerialNoticeOwners();
    void _testSerialNoticeSuppression();
    void _testSerialNoticePeriodicObservation();
    void _testSerialNoticeSavedPrecedence();
    void _testSerialNoticeConfigurationLifetime();
    void _testSerialNoticeStaleDiscovery();
    void _testSerialNoticePolicyTransitions();
    void _testSerialNoticeHistoryCap();
    void _testSerialNoticeAutomaticOpen_data();
    void _testSerialNoticeAutomaticOpen();
    void _testSerialNoticeUiVocabulary();
    void _testSavedSerialOwnerClearsDiscoverySkip();
    void _testSerialLossDiagnostics_data();
    void _testSerialLossDiagnostics();
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
    void _testUdpWriteQueuedAfterDisconnectIsSilent();
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
