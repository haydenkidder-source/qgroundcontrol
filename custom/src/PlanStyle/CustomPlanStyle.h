#pragma once

#include <QtCore/QList>
#include <QtCore/QPointer>

#include "QGCPlanStyle.h"

class COPController;

class CustomPlanStyle : public QGCPlanStyle
{
    Q_OBJECT

public:
    explicit CustomPlanStyle(QObject* parent = nullptr);

    qreal missionLineWidth() const final { return 4; }

    qreal boundaryLineWidth() const final { return 3; }

    QVariantList keepInDashPattern() const final { return {6.0, 3.0}; }

    QVariantList keepOutDashPattern() const final { return {2.0, 2.0}; }

    QColor keepOutFillColor() const final;

    QString rallyShape() const final { return QStringLiteral("diamond"); }

    QColor markerBorderColor() const final { return QColor(Qt::white); }

    qreal markerBorderWidth() const final { return 2; }

    QColor markerTextColor() const final { return QColor(Qt::black); }

    QColor vehicleColor(quint32 sysid) const final;

    /// Attach the engine's existing registry; null detaches without changing its entries.
    void attachRegistry(COPController* controller);

private:
    void _watchEntries();

    QPointer<COPController> _controller;
    QList<QMetaObject::Connection> _registryConnections;
    QList<QMetaObject::Connection> _entryConnections;
};
