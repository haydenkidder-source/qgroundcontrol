#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>

#include "GPSCorrectionFrame.h"

class COPReferencePoint : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(double latitude READ latitude NOTIFY changed)
    Q_PROPERTY(double longitude READ longitude NOTIFY changed)
    Q_PROPERTY(double altitude READ altitude NOTIFY changed)
    Q_PROPERTY(bool valid READ valid NOTIFY changed)
    Q_PROPERTY(QString source READ source NOTIFY changed)

public:
    explicit COPReferencePoint(QObject* parent = nullptr);

    QGeoCoordinate coordinate() const { return _coordinate; }

    double latitude() const { return _coordinate.latitude(); }

    double longitude() const { return _coordinate.longitude(); }

    double altitude() const { return _coordinate.altitude(); }

    bool valid() const { return _coordinate.isValid(); }

    QString source() const { return _source; }

    static QGeoCoordinate decode(const QByteArray& frame);

signals:
    void changed();

private:
    void _accept(const GPSCorrectionFrame& frame);
    QGeoCoordinate _coordinate;
    QString _source;
    QString _instance;
    quint64 _session = 0;
    QTimer _expiry;
};
