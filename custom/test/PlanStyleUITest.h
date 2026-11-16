#pragma once

#include <functional>

#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>
#include <QtPositioning/QGeoCoordinate>

#include "LinkConfiguration.h"
#include "MockLink.h"
#include "QmlUITestBase.h"

class COPController;
class COPVehicle;
class Fact;
class PlanMasterController;

class PlanStyleUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _flyPlans_data();
    void _flyPlans();
    void _perimeterVisuals_data();
    void _perimeterVisuals();
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
    void _clickStableItem(const QString& description, const std::function<QQuickItem*()>& resolve);
    void _clickStableButton(const QString& name);
    bool _selectTab(quint32 sysid);
    QList<SharedLinkConfigurationPtr> _configs;
    QList<QPointer<MockLink>> _links;
    QTemporaryDir _directory;
    QVariantMap _settingsSnapshot;
    QList<QPair<QPointer<Fact>, QVariant>> _factSnapshot;
    QGeoCoordinate _savedMapPosition;
    double _savedMapZoom = 0;
    bool _savedAdvancedUI = false;
};
