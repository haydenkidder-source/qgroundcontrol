#include "PlanMasterController.h"

#include <utility>

#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

#include "AppMessages.h"
#include "AppSettings.h"
#include "GeoFenceManager.h"
#include "JsonParsing.h"
#include "KMLPlanDomDocument.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "PlanCreator.h"
#include "QGCCorePlugin.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VisualMissionItem.h"

QGC_LOGGING_CATEGORY(PlanMasterControllerLog, "PlanManager.PlanMasterController")

QList<PlanMasterController*> PlanMasterController::_liveControllers;

PlanMasterController::PlanMasterController(QObject* parent)
    : QObject               (parent)
    , _multiVehicleMgr      (MultiVehicleManager::instance())
    , _controllerVehicle    (new Vehicle(Vehicle::MAV_AUTOPILOT_TRACK, Vehicle::MAV_TYPE_TRACK, this))
    , _managerVehicle       (_controllerVehicle)
    , _missionController    (this)
    , _geoFenceController   (this)
    , _rallyPointController (this)
{
    _commonInit();
}

#ifdef QGC_UNITTEST_BUILD
PlanMasterController::PlanMasterController(MAV_AUTOPILOT firmwareType, MAV_TYPE vehicleType, QObject* parent)
    : QObject               (parent)
    , _multiVehicleMgr      (MultiVehicleManager::instance())
    , _controllerVehicle    (new Vehicle(firmwareType, vehicleType))
    , _managerVehicle       (_controllerVehicle)
    , _missionController    (this)
    , _geoFenceController   (this)
    , _rallyPointController (this)
{
    _commonInit();
}
#endif

void PlanMasterController::_commonInit(void)
{
    _liveControllers.append(this);
    connect(&_missionController,    &MissionController::dirtyChanged,               this, &PlanMasterController::_updateOverallDirty);
    connect(&_geoFenceController,   &GeoFenceController::dirtyChanged,              this, &PlanMasterController::_updateOverallDirty);
    connect(&_rallyPointController, &RallyPointController::dirtyChanged,            this, &PlanMasterController::_updateOverallDirty);

    connect(&_missionController,    &MissionController::containsItemsChanged,       this, &PlanMasterController::containsItemsChanged);
    connect(&_geoFenceController,   &GeoFenceController::containsItemsChanged,      this, &PlanMasterController::containsItemsChanged);
    connect(&_rallyPointController, &RallyPointController::containsItemsChanged,    this, &PlanMasterController::containsItemsChanged);

    connect(this, &PlanMasterController::containsItemsChanged, this, &PlanMasterController::_updateShowCreateFromTemplate);

    connect(&_missionController,    &MissionController::syncInProgressChanged,      this, &PlanMasterController::syncInProgressChanged);
    connect(&_geoFenceController,   &GeoFenceController::syncInProgressChanged,     this, &PlanMasterController::syncInProgressChanged);
    connect(&_rallyPointController, &RallyPointController::syncInProgressChanged,   this, &PlanMasterController::syncInProgressChanged);

    connect(&_missionController, &MissionController::newItemsFromVehicle, this,
            &PlanMasterController::_loadMissionComplete);
    connect(&_geoFenceController, &GeoFenceController::loadComplete, this,
            &PlanMasterController::_loadGeoFenceComplete);
    connect(&_rallyPointController, &RallyPointController::loadComplete, this,
            &PlanMasterController::_loadRallyPointsComplete);

    // Offline vehicle can change firmware/vehicle type
    connect(_controllerVehicle,     &Vehicle::vehicleTypeChanged,                   this, &PlanMasterController::_updatePlanCreatorsList);
}


PlanMasterController::~PlanMasterController()
{
    _liveControllers.removeAll(this);
    _releaseExclusive();
    if (_removeExclusiveFence) {
        _removeExclusiveFence->releaseExclusive(&_removeAllFromVehicleContext);
    }
}

PlanMasterController::ExternalFenceResult PlanMasterController::applyVerifiedFence(Vehicle* vehicle,
                                                                                   const VerifiedFence& fence)
{
    return _applyVerifiedFence(vehicle, fence, EXTERNAL_FENCE_POLICY);
}

#ifdef QGC_UNITTEST_BUILD
PlanMasterController::ExternalFenceResult PlanMasterController::applyVerifiedFenceForTest(Vehicle* vehicle,
                                                                                          const VerifiedFence& fence,
                                                                                          ExternalFencePolicy policy)
{
    return _applyVerifiedFence(vehicle, fence, policy);
}
#endif

PlanMasterController::ExternalFenceResult PlanMasterController::_applyVerifiedFence(Vehicle* vehicle,
                                                                                    const VerifiedFence& fence,
                                                                                    ExternalFencePolicy policy)
{
    ExternalFenceResult result;
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return result;
    }
    const auto samePosition = [](const QGeoCoordinate& first, const QGeoCoordinate& second) {
        const auto transmitted = [](double value) {
            const double scaled = value * 1e7;
            return static_cast<qint32>(scaled + (scaled < 0 ? -1e-6 : 1e-6));
        };
        return transmitted(first.latitude()) == transmitted(second.latitude()) &&
               transmitted(first.longitude()) == transmitted(second.longitude());
    };
    for (PlanMasterController* editor : std::as_const(_liveControllers)) {
        if (editor->_flyView || editor->_managerVehicle != vehicle) {
            continue;
        }
        ++result.editors;
        if (policy == ExternalFencePolicy::RequireReload) {
            result.reloadRequired = true;
            if (!editor->_staleFenceVehicles.contains(vehicle)) {
                editor->_staleFenceVehicles.append(vehicle);
            }
            continue;
        }
        GeoFenceController* const controller = &editor->_geoFenceController;
        QList<bool> usedPolygons(controller->polygons()->count(), false);
        QList<bool> usedCircles(controller->circles()->count(), false);
        int added = 0;
        for (const VerifiedFenceShape& shape : fence.shapes) {
            if (shape.vertices.isEmpty()) {
                continue;
            }
            bool found = false;
            if (shape.polygon) {
                for (int i = 0; i < usedPolygons.count() && !found; ++i) {
                    auto* polygon = controller->polygons()->value<QGCFencePolygon*>(i);
                    if (usedPolygons[i] || !polygon || polygon->inclusion() != shape.inclusion ||
                        polygon->count() != shape.vertices.count()) {
                        continue;
                    }
                    const qsizetype count = shape.vertices.count();
                    for (qsizetype offset = 0; offset < count && !found; ++offset) {
                        for (int direction : {1, -1}) {
                            bool matches = true;
                            for (qsizetype j = 0; j < count; ++j) {
                                if (!samePosition(shape.vertices[j], polygon->vertexCoordinate(static_cast<int>(
                                                                         (offset + direction * j + count) % count)))) {
                                    matches = false;
                                    break;
                                }
                            }
                            if (matches) {
                                found = true;
                                usedPolygons[i] = true;
                                break;
                            }
                        }
                    }
                }
                if (!found) {
                    auto* polygon = new QGCFencePolygon(shape.inclusion, controller);
                    polygon->appendVertices(shape.vertices);
                    controller->polygons()->append(polygon);
                    ++added;
                }
            } else {
                for (int i = 0; i < usedCircles.count() && !found; ++i) {
                    auto* circle = controller->circles()->value<QGCFenceCircle*>(i);
                    if (!usedCircles[i] && circle && circle->inclusion() == shape.inclusion &&
                        samePosition(circle->center(), shape.vertices.first()) &&
                        static_cast<float>(circle->radius()->rawValue().toDouble()) ==
                            static_cast<float>(shape.radius)) {
                        usedCircles[i] = true;
                        found = true;
                    }
                }
                if (!found) {
                    controller->circles()->append(
                        new QGCFenceCircle(shape.vertices.first(), shape.radius, shape.inclusion, controller));
                    ++added;
                }
            }
        }
        bool changed = added > 0;
        const QGeoCoordinate planReturn = controller->breachReturnPoint();
        if (fence.returnPoint.isValid()) {
            if (!planReturn.isValid()) {
                controller->setBreachReturnPoint(fence.returnPoint);
                controller->breachReturnAltitude()->setRawValue(fence.returnPoint.altitude());
                changed = true;
            } else if (!samePosition(planReturn, fence.returnPoint) ||
                       (!(qIsNaN(planReturn.altitude()) && qIsNaN(fence.returnPoint.altitude())) &&
                        static_cast<float>(planReturn.altitude()) !=
                            static_cast<float>(fence.returnPoint.altitude()))) {
                result.returnPointConflict = true;
            }
        }
        result.shapesAdded += added;
        if (changed) {
            ++result.changedEditors;
            editor->_planContentsChanged();
            controller->setDirty(true);
            editor->_setDirtyStates(true, true);
        }
    }
    return result;
}

bool PlanMasterController::_acquireExclusive()
{
    if (!_managerVehicle || !_managerVehicle->geoFenceManager()) {
        return false;
    }
    GeoFenceManager* const fence = _managerVehicle->geoFenceManager();
    if (fence->exclusiveBusy() || !fence->tryAcquireExclusive(this)) {
        QGC::showAppMessage(
            tr("Another plan or fence operation is in progress on this vehicle. "
               "Try again when it has finished."));
        return false;
    }
    _exclusiveFence = fence;
    return true;
}

void PlanMasterController::_releaseExclusive()
{
    if (_exclusiveFence) {
        _exclusiveFence->releaseExclusive(this);
        _exclusiveFence.clear();
    }
}

void PlanMasterController::start(void)
{
    _missionController.start    (_flyView);
    _geoFenceController.start   (_flyView);
    _rallyPointController.start (_flyView);

    _activeVehicleChanged(_multiVehicleMgr->activeVehicle());
    connect(_multiVehicleMgr, &MultiVehicleManager::activeVehicleChanged, this, &PlanMasterController::_activeVehicleChanged);

    _updatePlanCreatorsList();
}

void PlanMasterController::startStaticActiveVehicle(Vehicle* vehicle, bool deleteWhenSendCompleted)
{
    _flyView = true;
    _deleteWhenSendCompleted = deleteWhenSendCompleted;
    _missionController.start(_flyView);
    _geoFenceController.start(_flyView);
    _rallyPointController.start(_flyView);
    _activeVehicleChanged(vehicle);
}

void PlanMasterController::_activeVehicleChanged(Vehicle* activeVehicle)
{
    if (_managerVehicle && _managerVehicle == activeVehicle) {
        // We are already setup for this vehicle
        return;
    }

    qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged" << activeVehicle;

    _releaseExclusive();

    if (_managerVehicle) {
        // Disconnect old vehicle. Be careful of wildcarding disconnect too much since _managerVehicle may equal _controllerVehicle
        disconnect(_managerVehicle,                         &Vehicle::initialPlanRequestCompleteChanged, this, nullptr);
        disconnect(_managerVehicle, &QObject::destroyed, this, nullptr);
        disconnect(_managerVehicle->missionManager(),       nullptr, this, nullptr);
        disconnect(_managerVehicle->geoFenceManager(),      nullptr, this, nullptr);
        disconnect(_managerVehicle->rallyPointManager(),    nullptr, this, nullptr);
    }

    const bool uploadInterrupted = (_sendSequence != SyncSequence::Idle);
    _loadSequence = SyncSequence::Idle;
    _sendSequence = SyncSequence::Idle;
    _loadVehicleId = 0;
    _sendVehicleId = 0;

    bool newOffline = false;
    if (activeVehicle == nullptr) {
        // Since there is no longer an active vehicle we use the offline controller vehicle as the manager vehicle
        _managerVehicle = _controllerVehicle;
        newOffline = true;
    } else {
        newOffline = false;
        _managerVehicle = activeVehicle;

        // Update controllerVehicle to the currently connected vehicle
        AppSettings* appSettings = SettingsManager::instance()->appSettings();
        appSettings->offlineEditingFirmwareClass()->setRawValue(QGCMAVLink::firmwareClass(_managerVehicle->firmwareType()));
        appSettings->offlineEditingVehicleClass()->setRawValue(QGCMAVLink::vehicleClass(_managerVehicle->vehicleType()));

        // We use these signals to sequence upload and download to the multiple controller/managers
        connect(_managerVehicle,                        &Vehicle::initialPlanRequestCompleteChanged, this, &PlanMasterController::_initialPlanRequestCompleteChanged);
        connect(_managerVehicle->missionManager(), &MissionManager::sendComplete, this,
                [this, vehicle = QPointer<Vehicle>(_managerVehicle)](bool error) {
                    if (vehicle && vehicle == _managerVehicle) {
                        _sendMissionComplete(error);
                    }
                });
        connect(_managerVehicle->geoFenceManager(), &GeoFenceManager::sendComplete, this,
                [this, vehicle = QPointer<Vehicle>(_managerVehicle)](bool error) {
                    if (vehicle && vehicle == _managerVehicle) {
                        _sendGeoFenceComplete(error);
                    }
                });
        connect(_managerVehicle->rallyPointManager(), &RallyPointManager::sendComplete, this,
                [this, vehicle = QPointer<Vehicle>(_managerVehicle)](bool error) {
                    if (vehicle && vehicle == _managerVehicle) {
                        _sendRallyPointsComplete(error);
                    }
                });
        connect(_managerVehicle->missionManager(), &MissionManager::error, this,
                &PlanMasterController::_planLoadFailed);
        connect(_managerVehicle->geoFenceManager(), &GeoFenceManager::error, this,
                &PlanMasterController::_planLoadFailed);
        connect(_managerVehicle->rallyPointManager(), &RallyPointManager::error, this,
                &PlanMasterController::_planLoadFailed);
        connect(_managerVehicle, &QObject::destroyed, this, [this] { _activeVehicleChanged(nullptr); });
    }

    _offline = newOffline;
    emit offlineChanged(offline());
    emit managerVehicleChanged(_managerVehicle);

    if (_flyView) {
        // We are in the Fly View
        if (newOffline) {
            // No active vehicle, clear mission
            qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Fly View - No active vehicle, clearing stale plan";
            removeAll();
        } else {
            // Fly view has changed to a new active vehicle, update to show correct mission
            qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Fly View - New active vehicle, loading new plan from manager vehicle";
            _showPlanFromManagerVehicle();
        }
    } else {
        // We are in the Plan view.
        if (containsItems()) {
            // We have a plan which is from a different vehicle than the new active vehicle. By definition this plan requires and upload.
            _setDirtyForUpload(true);

            // The plan view has a stale plan in it
            if (dirtyForSave()) {
                // Plan is dirty, the user must decide what to do in all cases
                qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Plan View - Previous dirty plan exists, no new active vehicle, sending promptForPlanUsageOnVehicleChange signal";
                emit promptForPlanUsageOnVehicleChange();
            } else {
                // Plan is not dirty
                if (newOffline) {
                    // The active vehicle went away with no new active vehicle
                    qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Plan View - Previous clean plan exists, no new active vehicle, clear stale plan";
                    removeAll();
                } else {
                    // We are transitioning from one active vehicle to another. Show the plan from the new vehicle.
                    qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Plan View - Previous clean plan exists, new active vehicle, loading from new manager vehicle";
                    _showPlanFromManagerVehicle();
                }
            }
        } else {
            // There is no previous Plan in the view
            _setDirtyStates(false, false);
            if (newOffline) {
                // Nothing special to do in this case
                qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Plan View - No previous plan, no longer connected to vehicle, nothing to do";
            } else {
                // Just show the plan from the new vehicle
                qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Plan View - No previous plan, new active vehicle, loading from new manager vehicle";
                _showPlanFromManagerVehicle();
            }
        }
    }

    if (uploadInterrupted) {
        QGC::showAppMessage(
            tr("Plan upload interrupted: the active vehicle changed or disconnected while the plan was "
               "being sent. The vehicle it was being sent to may hold only part of the plan (mission, "
               "geofence, rally points). Select that vehicle and upload again."));
        _setDirtyForUpload(true);
    }

    // Vehicle changed so we need to signal everything
    emit containsItemsChanged();
    emit syncInProgressChanged();
    emit dirtyForSaveChanged(dirtyForSave());
    emit dirtyForUploadChanged(dirtyForUpload());

    _updatePlanCreatorsList();
}

void PlanMasterController::loadFromVehicle(void)
{
    if (!_managerVehicle) {
        return;
    }
    SharedLinkInterfacePtr sharedLink = _managerVehicle->vehicleLinkManager()->primaryLink().lock();
    if (sharedLink) {
        if (sharedLink->linkConfiguration()->isHighLatency()) {
            QGC::showAppMessage(tr("Download not supported on high latency links."));
            return;
        }
    } else {
        // Vehicle is shutting down
        return;
    }

    if (_managerVehicle->geoFenceManager()->exclusiveBusy()) {
        QGC::showAppMessage(
            tr("Another plan or fence operation is in progress on this vehicle. "
               "Try again when it has finished."));
    } else if (offline()) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle called while offline";
    } else if (_flyView) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle called from Fly view";
    } else if (syncInProgress()) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle called while syncInProgress";
    } else if (_acquireExclusive()) {
        _loadVehicleId = _editingVehicleId();
        _loadSequence = SyncSequence::Mission;
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle calling _missionController.loadFromVehicle";
        _missionController.loadFromVehicle();
    }
}


void PlanMasterController::_loadMissionComplete(void)
{
    if (_loadSequence == SyncSequence::CachedMission) {
        _loadSequence = SyncSequence::CachedGeoFence;
        _geoFenceController.showPlanFromManagerVehicle();
        return;
    }
    if (_loadSequence != SyncSequence::Mission) {
        return;
    }
    _loadSequence = SyncSequence::GeoFence;
    if (_geoFenceController.supported()) {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadMissionComplete calling _geoFenceController.loadFromVehicle";
        _geoFenceController.loadFromVehicle();
    } else {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadMissionComplete GeoFence not supported skipping";
        _suppressOverallDirtyUpdate = true;
        _geoFenceController.removeAll();
        _suppressOverallDirtyUpdate = false;
        _loadGeoFenceComplete();
    }
}

void PlanMasterController::_loadGeoFenceComplete(void)
{
    if (_loadSequence == SyncSequence::CachedGeoFence) {
        _loadSequence = SyncSequence::CachedRallyPoints;
        _rallyPointController.showPlanFromManagerVehicle();
        return;
    }
    if (_loadSequence != SyncSequence::GeoFence) {
        return;
    }
    _loadSequence = SyncSequence::RallyPoints;
    if (_rallyPointController.supported()) {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadGeoFenceComplete calling _rallyPointController.loadFromVehicle";
        _rallyPointController.loadFromVehicle();
    } else {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadGeoFenceComplete Rally Points not supported skipping";
        _suppressOverallDirtyUpdate = true;
        _rallyPointController.removeAll();
        _suppressOverallDirtyUpdate = false;
        _loadRallyPointsComplete();
    }
}

void PlanMasterController::_loadRallyPointsComplete(void)
{
    if (_loadSequence != SyncSequence::RallyPoints && _loadSequence != SyncSequence::CachedRallyPoints) {
        return;
    }
    if (_loadSequence == SyncSequence::RallyPoints && _loadVehicleId && _managerVehicle &&
        _loadVehicleId == _managerVehicle->id()) {
        _staleFenceVehicles.removeAll(_managerVehicle);
    }
    if (_loadSequence == SyncSequence::RallyPoints) {
        _releaseExclusive();
    }
    _loadSequence = _loadVehicleId ? SyncSequence::Idle : SyncSequence::FailedLoad;
    _setPlanVehicleId(_loadVehicleId);
    qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadRallyPointsComplete";
    // A plan just downloaded from the vehicle reflects exactly what is on the vehicle, so it is not dirty for save.
    // Each element clears its own dirty flag when it adopts its downloaded content, so an element that is dirty now was
    // edited after that and keeps the plan dirty. A failed download leaves the editor with only part of the vehicle's
    // plan, so it stays dirty for upload. Any previous file association no longer describes the editor contents.
    const bool editedAfterAdoption =
        _missionController.dirty() || _geoFenceController.dirty() || _rallyPointController.dirty();
    _clearCurrentPlanFile();
    _setDirtyStates(editedAfterAdoption, editedAfterAdoption || !_loadVehicleId);
}

void PlanMasterController::_sendMissionComplete(bool error)
{
    if (_sendSequence != SyncSequence::Mission) {
        return;
    }
    if (error) {
        _sendVehicleId = 0;
        _sendFailed = true;
    }
    _sendSequence = SyncSequence::GeoFence;
    if (_geoFenceController.supported()) {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle start GeoFence sendToVehicle";
        _geoFenceController.sendToVehicle();
    } else {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle GeoFence not supported skipping";
        _sendGeoFenceComplete();
    }
}

void PlanMasterController::_sendGeoFenceComplete(bool error)
{
    if (_sendSequence != SyncSequence::GeoFence) {
        return;
    }
    if (error) {
        _sendVehicleId = 0;
        _sendFailed = true;
    }
    _sendSequence = SyncSequence::RallyPoints;
    if (_rallyPointController.supported()) {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle start rally sendToVehicle";
        _rallyPointController.sendToVehicle();
    } else {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle Rally Points not support skipping";
        _sendRallyPointsComplete();
    }
}

void PlanMasterController::_sendRallyPointsComplete(bool error)
{
    if (_sendSequence != SyncSequence::RallyPoints) {
        return;
    }
    if (error) {
        _sendVehicleId = 0;
        _sendFailed = true;
    }
    _sendSequence = SyncSequence::Idle;
    _releaseExclusive();
    if (_sendVehicleId) {
        if (_loadSequence == SyncSequence::FailedLoad) {
            _loadSequence = SyncSequence::Idle;
        }
        _loadVehicleId = _sendVehicleId;
        _setPlanVehicleId(_sendVehicleId);
    }
    qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle Rally Point send complete";
    // Each element clears its own dirty flag when it takes its upload snapshot, so an element that is dirty now was
    // edited after that and what was sent does not include the edit.
    const bool editedAfterSnapshot =
        _missionController.dirty() || _geoFenceController.dirty() || _rallyPointController.dirty();
    if (editedAfterSnapshot) {
        _setDirtyForSave(true);
    }
    _setDirtyForUpload(_sendFailed || editedAfterSnapshot);
    if (_deleteWhenSendCompleted) {
        this->deleteLater();
    }
}

void PlanMasterController::sendToVehicle(void)
{
    if (!_managerVehicle) {
        return;
    }
    if (_staleFenceVehicles.contains(_managerVehicle)) {
        QGC::showAppMessage(tr("The vehicle fence has changed. Reload the plan from the vehicle before uploading."));
        return;
    }
    SharedLinkInterfacePtr sharedLink = _managerVehicle->vehicleLinkManager()->primaryLink().lock();
    if (sharedLink) {
        if (sharedLink->linkConfiguration()->isHighLatency()) {
            QGC::showAppMessage(tr("Upload not supported on high latency links."));
            return;
        }
    } else {
        // Vehicle is shutting down
        return;
    }

    if (_managerVehicle->geoFenceManager()->exclusiveBusy()) {
        QGC::showAppMessage(
            tr("Another plan or fence operation is in progress on this vehicle. "
               "Try again when it has finished."));
    } else if (offline()) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle called while offline";
    } else if (syncInProgress()) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle called while syncInProgress";
    } else if (_acquireExclusive()) {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle start mission sendToVehicle";
        _sendVehicleId = _editingVehicleId();
        _sendSequence = SyncSequence::Mission;
        _sendFailed = false;
        _missionController.sendToVehicle();
    }
}

void PlanMasterController::loadFromFile(const QString& filename)
{
    QString errorString;
    QString errorMessage = tr("Error loading Plan file (%1). %2").arg(filename).arg("%1");

    if (filename.isEmpty()) {
        return;
    }

    QFileInfo fileInfo(filename);
    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorString = file.errorString() + QStringLiteral(" ") + filename;
        QGC::showAppMessage(errorMessage.arg(errorString));
        return;
    }

    const quint32 vehicleId = _editingVehicleId();
    _planContentsChanged();
    bool success = false;
    if (fileInfo.suffix() == AppSettings::waypointsFileExtension || fileInfo.suffix() == QStringLiteral("txt")) {
        success = _missionController.loadTextFile(file, errorString);
    } else {
        success = _loadPlanJson(file.readAll(), errorString);
    }
    if (!success) {
        QGC::showAppMessage(errorMessage.arg(errorString));
    }

    if (success){
        _setPlanVehicleId(vehicleId);
        _currentPlanFile = QString::asprintf("%s/%s.%s", fileInfo.path().toLocal8Bit().data(), fileInfo.completeBaseName().toLocal8Bit().data(), AppSettings::planFileExtension);
        emit currentPlanFileChanged();
        _setDirtyStates(false /* dirtyForSave */, true /* dirtyForUpload */);
    } else {
        _setPlanVehicleId(0);
        _clearCurrentPlanFile();
    }
}

bool PlanMasterController::_loadPlanJson(const QByteArray& bytes, QString& errorString)
{
    QJsonDocument jsonDoc;
    if (!JsonParsing::isJsonFile(bytes, jsonDoc, errorString)) {
        return false;
    }

    QJsonObject json = jsonDoc.object();
    //-- Allow plugins to pre process the load
    QGCCorePlugin::instance()->preLoadFromJson(this, json);

    int version;
    if (!JsonParsing::validateExternalQGCJsonFile(json, kPlanFileType, kPlanFileVersion, kPlanFileVersion, version, errorString)) {
        return false;
    }

    const QList<JsonParsing::KeyValidateInfo> rgKeyInfo = {
        { kJsonMissionObjectKey,        QJsonValue::Object, true },
        { kJsonGeoFenceObjectKey,       QJsonValue::Object, true },
        { kJsonRallyPointsObjectKey,    QJsonValue::Object, true },
    };
    if (!JsonParsing::validateKeys(json, rgKeyInfo, errorString)) {
        return false;
    }

    if (!_missionController.load(json[kJsonMissionObjectKey].toObject(), errorString) ||
            !_geoFenceController.load(json[kJsonGeoFenceObjectKey].toObject(), errorString) ||
            !_rallyPointController.load(json[kJsonRallyPointsObjectKey].toObject(), errorString)) {
        return false;
    }

    //-- Allow plugins to post process the load
    QGCCorePlugin::instance()->postLoadFromJson(this, json);
    return true;
}

QJsonDocument PlanMasterController::saveToJson()
{
    QJsonObject planJson;
    QGCCorePlugin::instance()->preSaveToJson(this, planJson);
    QJsonObject missionJson;
    QJsonObject fenceJson;
    QJsonObject rallyJson;
    JsonParsing::saveQGCJsonFileHeader(planJson, kPlanFileType, kPlanFileVersion);
    //-- Allow plugin to preemptly add its own keys to mission
    QGCCorePlugin::instance()->preSaveToMissionJson(this, missionJson);
    _missionController.save(missionJson);
    //-- Allow plugin to add its own keys to mission
    QGCCorePlugin::instance()->postSaveToMissionJson(this, missionJson);
    _geoFenceController.save(fenceJson);
    _rallyPointController.save(rallyJson);
    planJson[kJsonMissionObjectKey] = missionJson;
    planJson[kJsonGeoFenceObjectKey] = fenceJson;
    planJson[kJsonRallyPointsObjectKey] = rallyJson;
    QGCCorePlugin::instance()->postSaveToJson(this, planJson);
    return QJsonDocument(planJson);
}

bool
PlanMasterController::saveToCurrent()
{
    if (!_currentPlanFile.isEmpty()) {
        const bool saveSuccess = saveToFile(_currentPlanFile);
        return saveSuccess;
    }

    return false;
}

bool PlanMasterController::saveToFile(const QString& filename)
{
    if (filename.isEmpty()) {
        return false;
    }

    QString planFilename = filename;
    if (!QFileInfo(filename).fileName().contains(".")) {
        planFilename += QString(".%1").arg(fileExtension());
    }

    QFile file(planFilename);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QGC::showAppMessage(tr("Plan save error %1 : %2").arg(filename).arg(file.errorString()));
        return false;
    } else {
        const QByteArray saveBytes = saveToJson().toJson();
        const qint64 bytesWritten = file.write(saveBytes);
        if (bytesWritten != saveBytes.size()) {
            QGC::showAppMessage(tr("Plan save error %1 : %2").arg(filename).arg(file.errorString()));
            return false;
        }
        if(_currentPlanFile != planFilename) {
            _currentPlanFile = planFilename;
            emit currentPlanFileChanged();
        }
        _setDirtyForSave(false);
    }

    return true;
}

void PlanMasterController::saveToKml(const QString& filename)
{
    if (filename.isEmpty()) {
        return;
    }

    QString kmlFilename = filename;
    if (!QFileInfo(filename).fileName().contains(".")) {
        kmlFilename += QString(".%1").arg(kmlFileExtension());
    }

    QFile file(kmlFilename);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QGC::showAppMessage(tr("KML save error %1 : %2").arg(filename).arg(file.errorString()));
    } else {
        KMLPlanDomDocument planKML;
        _missionController.addMissionToKML(planKML);
        QTextStream stream(&file);
        stream << planKML.toString();
        file.close();
    }
}

void PlanMasterController::removeAll(void)
{
    _planContentsChanged();
    _setPlanVehicleId(0);
    _suppressOverallDirtyUpdate = true;
    _missionController.removeAll();
    _geoFenceController.removeAll();
    _rallyPointController.removeAll();
    _suppressOverallDirtyUpdate = false;

    _setDirtyStates(false, false);
    if (_offline) {
        _clearCurrentPlanFile();
    }
    setUserSelectedManualCreation(false);
}

void PlanMasterController::removeAllFromVehicle(void)
{
    if (!_managerVehicle || !_managerVehicle->geoFenceManager()) {
        return;
    }
    GeoFenceManager* const fence = _managerVehicle->geoFenceManager();
    if (fence->exclusiveBusy()) {
        QGC::showAppMessage(
            tr("Another plan or fence operation is in progress on this vehicle. "
               "Try again when it has finished."));
    } else if (offline()) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::removeAllFromVehicle called while offline";
    } else if (syncInProgress()) {
        qCCritical(PlanMasterControllerLog) << "PlanMasterController::removeAllFromVehicle called while syncInProgress";
    } else {
        if (_removeAllFromVehiclePendingCount > 0) {
            _finishRemoveAllFromVehicle(true);
        }
        if (!fence->tryAcquireExclusive(&_removeAllFromVehicleContext)) {
            QGC::showAppMessage(
                tr("Another plan or fence operation is in progress on this vehicle. "
                   "Try again when it has finished."));
            return;
        }
        _removeExclusiveFence = fence;
        const bool geoFenceRequested = _geoFenceController.supported();
        const bool rallyPointsRequested = _rallyPointController.supported();
        _trackRemoveAllFromVehicle(geoFenceRequested, rallyPointsRequested);
        _missionController.removeAllFromVehicle();
        if (geoFenceRequested) {
            _geoFenceController.removeAllFromVehicle();
        }
        if (rallyPointsRequested) {
            _rallyPointController.removeAllFromVehicle();
        }
        _setDirtyForUpload(false);
        _clearCurrentPlanFile();
    }
    setUserSelectedManualCreation(false);
}

void PlanMasterController::_trackRemoveAllFromVehicle(bool geoFenceRequested, bool rallyPointsRequested)
{
    if (_removeAllFromVehiclePendingCount > 0) {
        // Still unconfirmed on a previously active vehicle; it can no longer be reported once superseded
        _finishRemoveAllFromVehicle(true /* error */);
    }

    // Bound to this vehicle rather than whichever vehicle is active when the acks arrive: MultiVehicleManager switches
    // the active vehicle whenever any other vehicle drops out, which must not turn a confirmed removal into a failure.
    Vehicle* const vehicle = _managerVehicle;
    if (!vehicle) {
        return;
    }
    const auto stepComplete = [this](bool error) { _removeAllFromVehicleStepComplete(error); };

    _removeAllFromVehicleId = vehicle->id();
    _removeAllFromVehicleError = false;
    _removeAllFromVehiclePendingCount = 1;  // Mission removal is always requested
    _removeAllFromVehicleConnections.append(connect(vehicle->missionManager(), &MissionManager::removeAllComplete,
                                                    &_removeAllFromVehicleContext, stepComplete));
    if (geoFenceRequested) {
        _removeAllFromVehiclePendingCount++;
        _removeAllFromVehicleConnections.append(connect(vehicle->geoFenceManager(), &GeoFenceManager::removeAllComplete,
                                                        &_removeAllFromVehicleContext, stepComplete));
    }
    if (rallyPointsRequested) {
        _removeAllFromVehiclePendingCount++;
        _removeAllFromVehicleConnections.append(connect(vehicle->rallyPointManager(),
                                                        &RallyPointManager::removeAllComplete,
                                                        &_removeAllFromVehicleContext, stepComplete));
    }
    _removeAllFromVehicleConnections.append(connect(_multiVehicleMgr, &MultiVehicleManager::vehicleRemoved,
                                                    &_removeAllFromVehicleContext,
                                                    [this, vehicle = QPointer<Vehicle>(vehicle)](Vehicle* removed) {
                                                        if (removed == vehicle) {
                                                            // Gone before confirming every removal
                                                            _finishRemoveAllFromVehicle(true /* error */);
                                                        }
                                                    }));
}

void PlanMasterController::_removeAllFromVehicleStepComplete(bool error)
{
    if (_removeAllFromVehiclePendingCount <= 0) {
        // Stray completion with no in-flight removeAllFromVehicle request
        return;
    }

    _removeAllFromVehicleError = _removeAllFromVehicleError || error;
    if (--_removeAllFromVehiclePendingCount == 0) {
        _finishRemoveAllFromVehicle(_removeAllFromVehicleError);
    }
}

void PlanMasterController::_finishRemoveAllFromVehicle(bool error)
{
    for (const QMetaObject::Connection& connection : std::as_const(_removeAllFromVehicleConnections)) {
        disconnect(connection);
    }
    _removeAllFromVehicleConnections.clear();
    _removeAllFromVehiclePendingCount = 0;
    if (_removeExclusiveFence) {
        _removeExclusiveFence->releaseExclusive(&_removeAllFromVehicleContext);
        _removeExclusiveFence.clear();
    }
    emit removeAllFromVehicleCompleted(error, _removeAllFromVehicleId);
}

void PlanMasterController::_setPlanVehicleId(quint32 vehicleId)
{
    if (_planVehicleId != vehicleId) {
        _planVehicleId = vehicleId;
        emit planVehicleIdChanged();
    }
}

Vehicle* PlanMasterController::managerVehicle(void)
{
    return _managerVehicle.data();
}

quint32 PlanMasterController::_editingVehicleId() const
{
    return _managerVehicle && !_managerVehicle->isOfflineEditingVehicle() ? _managerVehicle->id() : 0;
}

void PlanMasterController::_planElementWillLoadFromVehicle(quint32 vehicleId)
{
    if (_planVehicleId != vehicleId || !_loadVehicleId) {
        _setPlanVehicleId(0);
    }
}

void PlanMasterController::_planContentsChanged()
{
    // Edits after the mission section was adopted can leave the remaining download mixed.
    if (_loadSequence == SyncSequence::GeoFence || _loadSequence == SyncSequence::RallyPoints ||
        _loadSequence == SyncSequence::CachedGeoFence || _loadSequence == SyncSequence::CachedRallyPoints) {
        _loadVehicleId = 0;
    }
    _sendVehicleId = 0;
}

void PlanMasterController::_planElementWillAdd()
{
    _planContentsChanged();
    if (isEmpty()) {
        _setPlanVehicleId(_editingVehicleId());
    }
}

void PlanMasterController::_planElementCleared()
{
    if (!_suppressOverallDirtyUpdate) {
        _planContentsChanged();
        _setPlanVehicleId(0);
    }
}

void PlanMasterController::_planElementRemoved()
{
    if (!_suppressOverallDirtyUpdate) {
        _planContentsChanged();
        if (isEmpty()) {
            _setPlanVehicleId(0);
        }
    }
}

void PlanMasterController::_planLoadFailed()
{
    if (!_managerVehicle ||
        (sender() && sender() != _managerVehicle->missionManager() && sender() != _managerVehicle->geoFenceManager() &&
         sender() != _managerVehicle->rallyPointManager())) {
        return;
    }
    _loadVehicleId = 0;
    if (_loadSequence == SyncSequence::Idle) {
        _loadSequence = SyncSequence::FailedLoad;
    } else if (_loadSequence != SyncSequence::FailedLoad) {
        _setPlanVehicleId(0);
    }
}

bool PlanMasterController::containsItems(void) const
{
    return _missionController.containsItems() || _geoFenceController.containsItems() || _rallyPointController.containsItems();
}

void PlanMasterController::_updateShowCreateFromTemplate(void)
{
    // When the plan becomes empty, always return to template-selection mode regardless
    // of how the items were removed.
    if (!containsItems() && _userSelectedManualCreation) {
        _userSelectedManualCreation = false;
        emit userSelectedManualCreationChanged();
    }
    const bool show = showCreateFromTemplate();
    if (show != _showCreateFromTemplate) {
        _showCreateFromTemplate = show;
        emit showCreateFromTemplateChanged();
    }
}

QString PlanMasterController::fileExtension(void) const
{
    return AppSettings::planFileExtension;
}

QString PlanMasterController::currentPlanFileName(void) const
{
    return _currentPlanFile.isEmpty() ? QString() : QFileInfo(_currentPlanFile).completeBaseName();
}

void PlanMasterController::_clearCurrentPlanFile()
{
    if (!_currentPlanFile.isEmpty()) {
        _currentPlanFile.clear();
        emit currentPlanFileChanged();
    }
}

QString PlanMasterController::kmlFileExtension(void) const
{
    return AppSettings::kmlFileExtension;
}

QStringList PlanMasterController::loadNameFilters(void) const
{
    QStringList filters;

    filters << tr("Supported types (*.%1 *.%2 *.%3)").arg(AppSettings::planFileExtension).arg(AppSettings::waypointsFileExtension).arg("txt") <<
               tr("All Files (*)");
    return filters;
}


QStringList PlanMasterController::saveNameFilters(void) const
{
    QStringList filters;

    filters << tr("Plan Files (*.%1)").arg(fileExtension()) << tr("All Files (*)");
    return filters;
}

void PlanMasterController::sendPlanToVehicle(Vehicle* vehicle, const QString& filename)
{
    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return;
    }
    // Use a transient PlanMasterController to accomplish this
    PlanMasterController* controller = new PlanMasterController();
    controller->startStaticActiveVehicle(vehicle, true /* deleteWhenSendCompleted */);
    controller->loadFromFile(filename);
    controller->sendToVehicle();
}

void PlanMasterController::_showPlanFromManagerVehicle(void)
{
    if (!_managerVehicle || offline()) {
        return;
    }
    if (_loadSequence != SyncSequence::InitialPlanLoad && _loadSequence != SyncSequence::FailedLoad) {
        _loadVehicleId = _editingVehicleId();
    }
    if (!_managerVehicle->initialPlanRequestComplete()) {
        _loadSequence = SyncSequence::InitialPlanLoad;
        return;
    }

    _loadSequence = SyncSequence::CachedMission;
    _missionController.showPlanFromManagerVehicle();
}

void PlanMasterController::_initialPlanRequestCompleteChanged(bool initialPlanRequestComplete)
{
    if (!initialPlanRequestComplete || _loadSequence != SyncSequence::InitialPlanLoad) {
        return;
    }
    qCDebug(PlanMasterControllerLog) << "_initialPlanRequestCompleteChanged: initial download complete";
    // Import the manager data into the editor (non-empty sub-controllers reject the automatic
    // manager updates during download) and scrub dirty/file association.
    _showPlanFromManagerVehicle();
}

bool PlanMasterController::syncInProgress(void) const
{
    return _missionController.syncInProgress() ||
            _geoFenceController.syncInProgress() ||
            _rallyPointController.syncInProgress();
}

bool PlanMasterController::isEmpty(void) const
{
    return _missionController.isEmpty() &&
            _geoFenceController.isEmpty() &&
            _rallyPointController.isEmpty();
}

void PlanMasterController::_updateOverallDirty(void)
{
    if (syncInProgress() || _suppressOverallDirtyUpdate) {
        return;
    }

    const bool saveDirty = _missionController.dirty() || _geoFenceController.dirty() || _rallyPointController.dirty();
    if (saveDirty) {
        _setDirtyForSave(true);
    }
}

void PlanMasterController::_setDirtyForSave(bool dirtyForSave)
{
    if (!dirtyForSave) {
        _clearElementDirtyStates();
    }
    if (_dirtyForSave != dirtyForSave) {
        _dirtyForSave = dirtyForSave;
        emit dirtyForSaveChanged(_dirtyForSave);

        if (_dirtyForSave) {
            _setDirtyForUpload(true);
        }
    }
}

void PlanMasterController::_setDirtyForUpload(bool dirtyForUpload)
{
    if (_dirtyForUpload != dirtyForUpload) {
        _dirtyForUpload = dirtyForUpload;
        emit dirtyForUploadChanged(_dirtyForUpload);
    }
}

void PlanMasterController::_setDirtyStates(bool dirtyForSave, bool dirtyForUpload)
{
    if (!dirtyForSave) {
        _clearElementDirtyStates();
    }

    const bool saveChanged = (_dirtyForSave != dirtyForSave);
    const bool uploadChanged = (_dirtyForUpload != dirtyForUpload);

    _dirtyForSave = dirtyForSave;
    _dirtyForUpload = dirtyForUpload;

    if (saveChanged) {
        emit dirtyForSaveChanged(_dirtyForSave);
    }
    if (uploadChanged) {
        emit dirtyForUploadChanged(_dirtyForUpload);
    }
}

// Element controllers only signal on a dirty transition, so they must be clean whenever the plan is clean for save
void PlanMasterController::_clearElementDirtyStates()
{
    _suppressOverallDirtyUpdate = true;
    _missionController.setDirty(false);
    _geoFenceController.setDirty(false);
    _rallyPointController.setDirty(false);
    _suppressOverallDirtyUpdate = false;
}

void PlanMasterController::_updatePlanCreatorsList(void)
{
    if (_flyView) {
        return;
    }

    if (!_managerVehicle) {
        return;
    }
    const auto vehicleClass = _managerVehicle->vehicleClass();

    // Only rebuild if the vehicle class actually changed
    if (_planCreators && _planCreatorsVehicleClass == vehicleClass) {
        return;
    }

    if (!_planCreators) {
        _planCreators = new QmlObjectListModel(this);
    } else {
        _planCreators->clearAndDeleteContents();
    }

    _planCreatorsVehicleClass = vehicleClass;

    // Allow custom builds to provide their own list of plan creators
    const QList<PlanCreator*> creators = QGCCorePlugin::instance()->planCreators(this);

    // Filter by vehicle class and add to the model
    for (PlanCreator* creator : creators) {
        if (creator->supportsVehicleClass(vehicleClass)) {
            _planCreators->append(creator);
        } else {
            delete creator;
        }
    }

    emit planCreatorsChanged(_planCreators);
}

int PlanMasterController::readyForSaveState(void) const
{
    const int missionState = _missionController.readyForSaveState();
    if (missionState != VisualMissionItem::ReadyForSave) {
        return missionState;
    }

    if (_rallyPointController.terrainQueryPending()) {
        return VisualMissionItem::NotReadyForSaveTerrain;
    }

    return VisualMissionItem::ReadyForSave;
}

void PlanMasterController::showPlanFromManagerVehicle(void)
{
    if (offline()) {
        // There is no new vehicle so clear any previous plan
        qCDebug(PlanMasterControllerLog) << "showPlanFromManagerVehicle: Plan View - No new vehicle, clear any previous plan";
        removeAll();
    } else {
        // We have a new active vehicle, show the plan from that
        qCDebug(PlanMasterControllerLog) << "showPlanFromManagerVehicle: Plan View - New vehicle available, show plan from new manager vehicle";
        _showPlanFromManagerVehicle();
    }
}

void PlanMasterController::setUserSelectedManualCreation(bool userSelectedManualCreation)
{
    if (_userSelectedManualCreation != userSelectedManualCreation) {
        _userSelectedManualCreation = userSelectedManualCreation;
        emit userSelectedManualCreationChanged();
        // Update showCreateFromTemplate directly — do not go through _updateShowCreateFromTemplate,
        // which would immediately auto-clear the flag if the plan happens to be empty right now.
        const bool show = showCreateFromTemplate();
        if (show != _showCreateFromTemplate) {
            _showCreateFromTemplate = show;
            emit showCreateFromTemplateChanged();
        }
    }
}
