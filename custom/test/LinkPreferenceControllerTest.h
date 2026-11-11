#pragma once

#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>

#include "CommsTest.h"

class LinkPreferenceController;
class VehicleRoleController;

class LinkPreferenceControllerTest : public CommsTest
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _settleFailoverFailbackAndSecondaryChoice_data();
    void _settleFailoverFailbackAndSecondaryChoice();
    void _lostAndOtherVehicleLinksAreNeverSelected();
    void _physicalDisconnectWaitsForBackoff();
    void _transferBusyAtDeadline_data();
    void _transferBusyAtDeadline();
    void _healthyPreferredDepartureReturnsAfterWait();
    void _repickCurrentPrimary_data();
    void _repickCurrentPrimary();
    void _signingPendingAtDeadline_data();
    void _signingPendingAtDeadline();
    void _reusedSysidStartsWithFreshHistory();
    void _productionClockReturnsAfterHold();
    void _armedConnectionCanReturn();
    void _returnStatusAndNotifications();
    void _returnStatusText_data();
    void _returnStatusText();
    void _statusObserverCanPauseReturn();

private:
    SharedLinkInterfacePtr _startLink(const QString& name, quint32 sysid, bool startArmed = false);
    Vehicle* _startPair(quint32 sysid = 4294967295U, bool productionClock = false);
    /// The vehicle keeps loading parameters after its initial connect; the controller waits for that, so must a test.
    bool _waitUntilIdle(Vehicle* vehicle);

    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
    QVariant _failbackHold;
    VehicleRoleController* _roles = nullptr;
    LinkPreferenceController* _preference = nullptr;
    QList<SharedLinkConfigurationPtr> _configs;
    SharedLinkInterfacePtr _backup;
    SharedLinkInterfacePtr _preferred;
};
