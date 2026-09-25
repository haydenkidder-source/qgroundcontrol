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
    void _pendingControl();
    void _notificationQueue();
    void _referencePoint();
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
    void _disconnectAndReconnect();
};
