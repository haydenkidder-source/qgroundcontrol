#pragma once

#include <QtCore/QTemporaryDir>

#include "UnitTest.h"
#include "VehicleTestManualConnect.h"

class COPControllerTest : public UnitTest
{
    Q_OBJECT
private slots:
    void init() override;
    void cleanup() override;
    void _rolesAndOrder();
    void _pendingControl_data();
    void _pendingControl();
    void _systemIdSettingsRoundTrip_data();
    void _systemIdSettingsRoundTrip();
    void _savedOrderValidation_data();
    void _savedOrderValidation();
    void _notificationQueue();
    void _referencePoint();
    void _referencePointFollowsSelectedStream();
    void _colorAssignedByDiscoveryOrder();

private:
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
};

class COPVehicleLifecycleTest : public VehicleTestManualConnect
{
    Q_OBJECT
private slots:
    void _disconnectAndReconnect_data();
    void _disconnectAndReconnect();
};
