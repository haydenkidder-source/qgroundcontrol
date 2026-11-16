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

    [[nodiscard]] qreal missionLineWidth() const final { return 4; }

    [[nodiscard]] qreal boundaryLineWidth() const final { return 3; }

    [[nodiscard]] QVariantList keepInDashPattern() const final { return {6.0, 3.0}; }

    [[nodiscard]] QVariantList keepOutDashPattern() const final { return {2.0, 2.0}; }

    [[nodiscard]] QColor keepOutFillColor() const final;

    [[nodiscard]] QString rallyShape() const final { return QStringLiteral("diamond"); }

    [[nodiscard]] QColor markerBorderColor() const final { return QColor(Qt::white); }

    [[nodiscard]] qreal markerBorderWidth() const final { return 2; }

    [[nodiscard]] QColor markerTextColor() const final { return QColor(Qt::black); }

    [[nodiscard]] QColor vehicleColor(quint32 sysid) const final;

    /// Attach the engine's existing registry; null detaches without changing its entries.
    void attachRegistry(COPController* controller);

private:
    void _watchEntries();

    QPointer<COPController> _controller;
    QList<QMetaObject::Connection> _registryConnections;
    QList<QMetaObject::Connection> _entryConnections;
};
