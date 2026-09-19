#pragma once

#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>

#include "LinkConfiguration.h"
#include "MockLink.h"
#include "QmlUITestBase.h"

class COPController;
class COPVehicle;

class COPStressTest : public UnitTest
{
    Q_OBJECT
private slots:
    void init() override;
    void cleanup() override;
    void _videoOverrideSurvivesRestart();
    void _unacknowledgedBurstIsNotDiscarded();

private:
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
};

class COPStressUITest : public QmlUITestBase
{
    Q_OBJECT
private slots:
    void init() override;
    void cleanup() override;
    void _threeVehicleChurn();
    void _lastControlRequestWins();
    void _disconnectOtherPreservesControl();
    void _navigationAndLayout();

private:
    COPController* _controller() const;
    COPVehicle* _entryFor(int sysid) const;
    MockLink* _start(MAV_TYPE type, bool increment = true);
    bool _selectTab(int sysid);
    QList<SharedLinkConfigurationPtr> _configs;
    QList<QPointer<MockLink>> _links;
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
};
