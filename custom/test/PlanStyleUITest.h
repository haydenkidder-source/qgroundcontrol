#pragma once

#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>

#include "LinkConfiguration.h"
#include "MockLink.h"
#include "QmlUITestBase.h"

class COPController;
class COPVehicle;
class PlanMasterController;

class PlanStyleUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _flyPlans_data();
    void _flyPlans();
    void _liveRegistryAndDynamicVisuals();
    void _liveBoundaryVisuals();
    void _planEditor_data();
    void _planEditor();

protected:
    bool usesPluginFlyViewOverlay() const override { return true; }

private:
    COPController* _controller() const;
    COPVehicle* _entryFor(quint32 sysid) const;
    MockLink* _start(MAV_TYPE type);
    void _uploadPlan(Vehicle* vehicle, int count);
    void _checkPlan(QQuickItem* view, quint32 id, const QColor& color, int itemCount, int rallyCount);
    void _editPlan(QQuickItem* view);
    void _insertPatterns(PlanMasterController* master);
    void _editPatterns(QQuickItem* view);
    void _setPlanLayer(QQuickItem* view, int layer);
    bool _selectTab(quint32 sysid);
    QList<SharedLinkConfigurationPtr> _configs;
    QList<QPointer<MockLink>> _links;
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
};
