#pragma once

#include <QtCore/QTemporaryDir>

#include "UnitTest.h"
#include "VehicleTestManualConnect.h"

class QQmlApplicationEngine;

class COPVideoSessionTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _staleReadiness_data();
    void _staleReadiness();
    void _receiverOwnsReconnectAndConfiguration();
    void _endpointOwnershipAndShutdown();
    void _overviewHandoff();
};

class COPVideoPreviewTest : public VehicleTestManualConnect
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _disconnectAndReconnect_data();
    void _disconnectAndReconnect();
    void _videoOverrideFollowsActiveVehicle();

private:
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
    QQmlApplicationEngine* _engine = nullptr;
};
