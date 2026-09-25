#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtGui/QColor>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>

#include "QmlObjectListModel.h"
#include "Vehicle.h"
#include "VehicleRoleController.h"

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
    Q_PROPERTY(QColor color READ color NOTIFY colorChanged)
    Q_PROPERTY(
        bool planOverlayVisible READ planOverlayVisible WRITE setPlanOverlayVisible NOTIFY planOverlayVisibleChanged)
    Q_PROPERTY(QVariantList missionCoordinates READ missionCoordinates NOTIFY planDataChanged)
    Q_PROPERTY(QVariantList fencePolygons READ fencePolygons NOTIFY planDataChanged)
    Q_PROPERTY(QVariantList fenceCircles READ fenceCircles NOTIFY planDataChanged)
    Q_PROPERTY(QVariantList rallyPoints READ rallyPoints NOTIFY planDataChanged)

public:
    COPVehicle(int sysid, VehicleRoleController* roles, QObject* parent);

    int sysid() const { return _sysid; }

    QString label() const;

    QString videoUri() const { return _videoUri; }

    void setVideoUri(const QString& uri);

    Vehicle* vehicle() const { return _vehicle; }

    bool connected() const;

    QGeoCoordinate coordinate() const { return _coordinate; }

    /// Whether this vehicle's mission/geofence/rally-point overlay should be drawn on the COP map.
    /// Purely a display toggle - not persisted, and does not affect the vehicle in any way.
    bool planOverlayVisible() const { return _planOverlayVisible; }

    void setPlanOverlayVisible(bool visible);

    /// This vehicle's navigation waypoints (mission items with a valid lat/lon), in mission order,
    /// for a read-only overlay polyline. Snapshotted from Vehicle::missionManager() - the same live
    /// data PlanView downloads, not a separate download - whenever it (re)loads; see _refreshPlanData().
    QVariantList missionCoordinates() const { return _missionCoordinates; }

    /// Each entry: {"path": list<coordinate>, "inclusion": bool}.
    QVariantList fencePolygons() const { return _fencePolygons; }

    /// Each entry: {"center": coordinate, "radius": real (meters), "inclusion": bool}.
    QVariantList fenceCircles() const { return _fenceCircles; }

    QVariantList rallyPoints() const { return _rallyPoints; }

    QString flightMode() const { return _flightMode; }

    QDateTime lastSeen() const { return _lastSeen; }

    /// This vehicle's identifying color for COP map markers and overlays (mission/geofence/rally),
    /// so an operator can visually tell vehicles apart at a glance. Assigned once, by the order
    /// vehicles were first remembered (see COPController::_remember()) - stable for the life of
    /// the entry, cycling through a fixed palette if there are more vehicles than colors.
    QColor color() const;

    void setVehicle(Vehicle* vehicle);

    /// Set by COPController when this entry is first remembered; not user-editable.
    void setColorIndex(int index);

signals:
    void stateChanged();
    void labelChanged();
    void videoUriChanged();
    void colorChanged();
    void planOverlayVisibleChanged();
    void planDataChanged();

private:
    void _snapshot();
    /// Re-reads mission/geofence/rally data from the vehicle's managers into the cached lists
    /// below and emits planDataChanged(). Called once per actual reload rather than on every
    /// property read, since geofence circles in particular are only readable by copying each
    /// QGCFenceCircle (its radius accessor isn't const - see fenceCircles() in the .cc).
    void _refreshPlanData();
    int _sysid;
    QPointer<VehicleRoleController> _roles;
    QPointer<Vehicle> _vehicle;
    QGeoCoordinate _coordinate;
    QString _flightMode;
    QString _videoUri;
    QDateTime _lastSeen;
    bool _planOverlayVisible = false;
    int _colorIndex = 0;
    QVariantList _missionCoordinates;
    QVariantList _fencePolygons;
    QVariantList _fenceCircles;
    QVariantList _rallyPoints;
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
    Q_PROPERTY(quint64 droppedMessages READ droppedMessages NOTIFY messagesChanged)
    Q_PROPERTY(bool unacknowledged READ unacknowledged NOTIFY messagesChanged)

public:
    explicit COPController(QObject* parent = nullptr);

    QmlObjectListModel* vehicles() { return &_vehicles; }

    int selectedSysid() const { return _selectedSysid; }

    int pendingSysid() const { return _pendingSysid; }

    COPVehicle* selected() const;

    QVariantList messages() const { return _messages; }

    quint64 droppedMessages() const { return _droppedMessages; }

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
    void _requestActivation(Vehicle* vehicle);
    void _queueActivation(quint64 token);
    QmlObjectListModel _vehicles;
    QPointer<VehicleRoleController> _roles;
    QPointer<Vehicle> _activeBeforeRemoval;
    QPointer<Vehicle> _requestedVehicle;
    QPointer<Vehicle> _activationInFlight;
    quint64 _controlRequestToken = 0;
    QVariantList _messages;
    int _unacknowledgedCount = 0;
    quint64 _droppedMessages = 0;
    int _selectedSysid = 0;
    int _pendingSysid = 0;
    bool _initialized = false;
    bool _unacknowledged = false;
};
