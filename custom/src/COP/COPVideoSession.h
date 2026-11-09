#pragma once

#include <functional>

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
    /// Permanently stops this session while application event delivery is still available.
    void shutdown();

private:
    friend class COPVideoSessionTest;

    void _queueUpdate();
    void _update();
    void _invalidate();
    void _releaseEndpoint();
    void _stop();
    void _setStatus(const QString& status);
    QPointer<Vehicle> _vehicle;
    QPointer<QQuickItem> _output;
    QPointer<VideoReceiver> _receiver;
    std::function<VideoReceiver*(QObject*)> _receiverFactory;
    std::function<void(QObject*, std::function<void(bool)>)> _requestBackendReady;
    QTimer _refresh;
    QString _status;
    QString _uri;
    QString _endpoint;
    QString _pendingUri;
    quint64 _generation = 0;
    bool _backendReady = false;
    bool _backendPending = false;
    bool _shutdown = false;
    bool _enabled = true;
    bool _decoding = false;

signals:
    void configurationChanged();
    void statusChanged();
};
