#pragma once

#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>

#include "LinkConfiguration.h"
#include "MockLink.h"
#include "QmlUITestBase.h"

class COPController;
class COPVehicle;

class PlanStyleUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _flyPlans_data();
    void _flyPlans();
    void _liveRegistryAndDynamicVisuals();

protected:
    bool usesPluginFlyViewOverlay() const override { return true; }

private:
    COPController* _controller() const;
    COPVehicle* _entryFor(quint32 sysid) const;
    MockLink* _start(MAV_TYPE type);
    bool _selectTab(quint32 sysid);
    QList<SharedLinkConfigurationPtr> _configs;
    QList<QPointer<MockLink>> _links;
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
};
