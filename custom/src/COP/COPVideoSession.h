#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtQmlIntegration/QtQmlIntegration>
#include <QtQuick/QQuickItem>

#include "Vehicle.h"
#include "VideoReceiver.h"

class COPVideoSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Vehicle* vehicle READ vehicle WRITE setVehicle NOTIFY configurationChanged)
    Q_PROPERTY(QQuickItem* output READ output WRITE setOutput NOTIFY configurationChanged)
    Q_PROPERTY(QString uri READ uri WRITE setUri NOTIFY configurationChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY configurationChanged)
    Q_PROPERTY(bool decoding READ decoding NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit COPVideoSession(QObject* parent = nullptr);
    ~COPVideoSession() override;

    Vehicle* vehicle() const { return _vehicle; }

    QQuickItem* output() const { return _output; }

    QString uri() const { return _uri; }

    void setUri(const QString& uri);

    bool enabled() const { return _enabled; }

    bool decoding() const { return _decoding; }

    QString status() const { return _status; }

    void setVehicle(Vehicle* vehicle);
    void setOutput(QQuickItem* output);
    void setEnabled(bool enabled);

private:
    void _update();
    void _stop();
    void _setStatus(const QString& status);
    QPointer<Vehicle> _vehicle;
    QPointer<QQuickItem> _output;
    VideoReceiver* _receiver = nullptr;
    QTimer _refresh;
    QString _status;
    QString _uri;
    QString _endpoint;
    bool _backendReady = false;
    bool _backendPending = false;
    bool _enabled = true;
    bool _decoding = false;

signals:
    void configurationChanged();
    void statusChanged();
};
