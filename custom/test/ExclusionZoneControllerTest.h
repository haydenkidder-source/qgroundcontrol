#pragma once

#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>

#include "VehicleTestManualConnect.h"

class ExclusionZoneController;

class ExclusionZoneControllerTest : public VehicleTestManualConnect
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _pushVerification_data();
    void _pushVerification();
    void _stagedZoneLifetime_data();
    void _stagedZoneLifetime();

private:
    void _seedFence(bool unspecifiedAltitude);
    void _stageZone(ExclusionZoneController& controller);
    void _connectOtherVehicle();
    void _expectFenceError(int errorCode, const QString& message);

    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
    QPointer<MockLink> _otherLink;
};
