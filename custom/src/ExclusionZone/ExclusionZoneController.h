#pragma once

#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtQmlIntegration/QtQmlIntegration>

class GeoFenceManager;
class QmlObjectListModel;
class StagedExclusionZone;
class Vehicle;

/// \brief Stages perception-derived exclusion polygons (imported from KML/SHP) for operator
/// review/approval, then merges approved zones into a specific vehicle's existing geofence.
///
/// Approved zones are merged with the target vehicle's current fence - freshly loaded from the
/// vehicle, not a stale cache - before being sent: GeoFenceManager::sendToVehicle() replaces the
/// entire fence with whatever it's given, so pushing the approved zones by themselves would
/// silently delete the vehicle's inclusion polygon, any other zones, and its breach-return point.
/// The merged fence is written to a timestamped audit-trail JSON file (same schema
/// GeoFenceController::save()/load() use) and re-read from that file before sending, so the
/// on-disk record is proven to match what was actually sent rather than just what was in memory.
/// The push itself goes directly through the target Vehicle's own GeoFenceManager rather than a
/// throwaway PlanMasterController/GeoFenceController: driving the latter's send would either leak
/// the throwaway controller or risk overwriting the target vehicle's live mission (its send
/// cascade is Mission -> GeoFence -> RallyPoints).
class ExclusionZoneController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_MOC_INCLUDE("QmlObjectListModel.h")
    Q_MOC_INCLUDE("Vehicle.h")

    Q_PROPERTY(QmlObjectListModel* stagedZones READ stagedZones CONSTANT)
    Q_PROPERTY(Vehicle* targetVehicle READ targetVehicle WRITE setTargetVehicle NOTIFY targetVehicleChanged)
    Q_PROPERTY(int approvedCount READ approvedCount NOTIFY approvedCountChanged)

public:
    explicit ExclusionZoneController(QObject* parent = nullptr);
    ~ExclusionZoneController();

    QmlObjectListModel* stagedZones() const { return _stagedZones; }

    Vehicle* targetVehicle() const { return _targetVehicle; }

    void setTargetVehicle(Vehicle* vehicle);

    int approvedCount() const;

    /// Imports polygons from a KML/SHP file, staging one exclusion zone per polygon found.
    /// @return true if at least one polygon was imported.
    Q_INVOKABLE bool importFromFile(const QString& file);

    /// Marks a staged zone's approval state by its index in stagedZones.
    Q_INVOKABLE void setApproved(int index, bool approved);

    /// Loads targetVehicle's current fence, merges in the approved zones, writes/re-reads the
    /// audit-trail file, and pushes the merged result to targetVehicle's geofence. Requires
    /// targetVehicle to be set and approvedCount > 0.
    /// @return true if the push was started (completion is reported via pushFinished).
    Q_INVOKABLE bool pushApproved();

signals:
    void targetVehicleChanged(Vehicle* targetVehicle);
    void approvedCountChanged();
    /// Fired once the push attempt concludes, successfully or not.
    void pushFinished(bool success, QString message);

private:
    void _mergeAndSend(GeoFenceManager* fenceMgr, const QList<StagedExclusionZone*>& approvedZones);

    QmlObjectListModel* _stagedZones = nullptr;
    Vehicle* _targetVehicle = nullptr;
    QString _lastFenceError;

    // Re-bound on every pushApproved() call so at most one push's completion handlers are ever
    // live on a GeoFenceManager, regardless of how many times pushApproved() has been called or
    // how many different target vehicles it's been pointed at.
    QMetaObject::Connection _fenceLoadCompleteConnection;
    QMetaObject::Connection _fenceLoadErrorConnection;
    QMetaObject::Connection _fenceErrorConnection;
    QMetaObject::Connection _fenceSendCompleteConnection;
};
