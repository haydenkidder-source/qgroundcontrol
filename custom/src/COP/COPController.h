#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>

#include "QmlObjectListModel.h"
#include "Vehicle.h"
#include "VehicleRoleController.h"

class QQmlEngine;
class QJSEngine;

class COPVehicle : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by COPController")
    Q_PROPERTY(int sysid READ sysid CONSTANT)
    Q_PROPERTY(QString label READ label NOTIFY labelChanged)
    Q_PROPERTY(QString videoUri READ videoUri WRITE setVideoUri NOTIFY videoUriChanged)
    Q_PROPERTY(Vehicle* vehicle READ vehicle NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QGeoCoordinate coordinate READ coordinate NOTIFY stateChanged)
    Q_PROPERTY(QString flightMode READ flightMode NOTIFY stateChanged)
    Q_PROPERTY(QDateTime lastSeen READ lastSeen NOTIFY stateChanged)

public:
    COPVehicle(int sysid, VehicleRoleController* roles, QObject* parent);

    int sysid() const { return _sysid; }

    QString label() const;

    QString videoUri() const { return _videoUri; }

    void setVideoUri(const QString& uri);

    Vehicle* vehicle() const { return _vehicle; }

    bool connected() const;

    QGeoCoordinate coordinate() const { return _coordinate; }

    QString flightMode() const { return _flightMode; }

    QDateTime lastSeen() const { return _lastSeen; }

    void setVehicle(Vehicle* vehicle);

signals:
    void stateChanged();
    void labelChanged();
    void videoUriChanged();

private:
    void _snapshot();
    int _sysid;
    QPointer<VehicleRoleController> _roles;
    QPointer<Vehicle> _vehicle;
    QGeoCoordinate _coordinate;
    QString _flightMode;
    QString _videoUri;
    QDateTime _lastSeen;
};

class COPController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QmlObjectListModel* vehicles READ vehicles CONSTANT)
    Q_PROPERTY(int selectedSysid READ selectedSysid NOTIFY selectionChanged)
    Q_PROPERTY(int pendingSysid READ pendingSysid NOTIFY selectionChanged)
    Q_PROPERTY(COPVehicle* selected READ selected NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(bool unacknowledged READ unacknowledged NOTIFY messagesChanged)

public:
    explicit COPController(QObject* parent = nullptr);
    static COPController* instance();
    static COPController* create(QQmlEngine*, QJSEngine*);

    QmlObjectListModel* vehicles() { return &_vehicles; }

    int selectedSysid() const { return _selectedSysid; }

    int pendingSysid() const { return _pendingSysid; }

    COPVehicle* selected() const;

    QVariantList messages() const { return _messages; }

    bool unacknowledged() const { return _unacknowledged; }

    Q_INVOKABLE void initialize(VehicleRoleController* roles);
    Q_INVOKABLE void selectVehicle(int sysid);
    Q_INVOKABLE void assumeControl();
    Q_INVOKABLE void cancelControl();
    Q_INVOKABLE void notify(const QString& message);
    Q_INVOKABLE void acknowledge();

signals:
    void selectionChanged();
    void messagesChanged();

private:
    COPVehicle* _find(int sysid) const;
    COPVehicle* _remember(int sysid);
    void _vehicleAdded(Vehicle* vehicle);
    void _vehicleRemoved(Vehicle* vehicle);
    void _syncRoles();
    QmlObjectListModel _vehicles;
    QPointer<VehicleRoleController> _roles;
    QVariantList _messages;
    int _selectedSysid = 0;
    int _pendingSysid = 0;
    bool _initialized = false;
    bool _unacknowledged = false;
};
