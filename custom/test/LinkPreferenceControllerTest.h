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
    void _settleFailoverFailbackAndManualChoice_data();
    void _settleFailoverFailbackAndManualChoice();
    void _lostAndOtherVehicleLinksAreNeverSelected();
    void _physicalDisconnectWaitsForBackoff();
    void _transferBusyAtDeadline_data();
    void _transferBusyAtDeadline();
    void _repickCurrentPrimary();
    void _reusedSysidStartsWithFreshHistory();

private:
    SharedLinkInterfacePtr _startLink(const QString& name, quint32 sysid);
    Vehicle* _startPair(quint32 sysid = 4294967295U);
    /// The vehicle keeps loading parameters after its initial connect; the controller waits for that, so must a test.
    bool _waitUntilIdle(Vehicle* vehicle);

    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
    VehicleRoleController* _roles = nullptr;
    LinkPreferenceController* _preference = nullptr;
    QList<SharedLinkConfigurationPtr> _configs;
    SharedLinkInterfacePtr _backup;
    SharedLinkInterfacePtr _preferred;
};
