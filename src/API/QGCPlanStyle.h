#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtGui/QColor>
#include <QtQmlIntegration/QtQmlIntegration>

/// Optional presentation policy for plan map items; the base supplies no styling.
class QGCPlanStyle : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the core plugin")
    /// Changes whenever a presentation value or vehicle color lookup can change.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    /// Mission stroke width in pixels.
    Q_PROPERTY(qreal missionLineWidth READ missionLineWidth NOTIFY revisionChanged)
    /// Boundary stroke width in pixels.
    Q_PROPERTY(qreal boundaryLineWidth READ boundaryLineWidth NOTIFY revisionChanged)
    /// Keep-in dash and gap lengths in multiples of boundaryLineWidth.
    Q_PROPERTY(QVariantList keepInDashPattern READ keepInDashPattern NOTIFY revisionChanged)
    /// Keep-out dash and gap lengths in multiples of boundaryLineWidth.
    Q_PROPERTY(QVariantList keepOutDashPattern READ keepOutDashPattern NOTIFY revisionChanged)
    /// Keep-out fill including opacity.
    Q_PROPERTY(QColor keepOutFillColor READ keepOutFillColor NOTIFY revisionChanged)
    /// Rally marker shape name, or empty when unspecified.
    Q_PROPERTY(QString rallyShape READ rallyShape NOTIFY revisionChanged)
    /// Marker border color including opacity.
    Q_PROPERTY(QColor markerBorderColor READ markerBorderColor NOTIFY revisionChanged)
    /// Marker border width in pixels.
    Q_PROPERTY(qreal markerBorderWidth READ markerBorderWidth NOTIFY revisionChanged)
    /// Marker text color including opacity.
    Q_PROPERTY(QColor markerTextColor READ markerTextColor NOTIFY revisionChanged)

public:
    explicit QGCPlanStyle(QObject* parent = nullptr);

    [[nodiscard]] int revision() const { return _revision; }

    [[nodiscard]] virtual qreal missionLineWidth() const { return 0; }

    [[nodiscard]] virtual qreal boundaryLineWidth() const { return 0; }

    [[nodiscard]] virtual QVariantList keepInDashPattern() const { return {}; }

    [[nodiscard]] virtual QVariantList keepOutDashPattern() const { return {}; }

    [[nodiscard]] virtual QColor keepOutFillColor() const { return {}; }

    [[nodiscard]] virtual QString rallyShape() const { return {}; }

    [[nodiscard]] virtual QColor markerBorderColor() const { return {}; }

    [[nodiscard]] virtual qreal markerBorderWidth() const { return 0; }

    [[nodiscard]] virtual QColor markerTextColor() const { return {}; }

    /// Invalid when the vehicle is unknown or there is no color registry; lookup never creates an entry.
    [[nodiscard]] Q_INVOKABLE virtual QColor vehicleColor(quint32 sysid) const;

signals:
    void revisionChanged();

protected:
    void _advanceRevision();

private:
    int _revision = 0;
};
