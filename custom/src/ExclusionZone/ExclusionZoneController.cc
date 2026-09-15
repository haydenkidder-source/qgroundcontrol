#include "ExclusionZoneController.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtPositioning/QGeoCoordinate>

#include "AppSettings.h"
#include "GeoFenceManager.h"
#include "GeoJsonHelper.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "ShapeFileHelper.h"
#include "StagedExclusionZone.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(ExclusionZoneLog, "Custom.ExclusionZone")

ExclusionZoneController::ExclusionZoneController(QObject* parent)
    : QObject(parent), _stagedZones(new QmlObjectListModel(this))
{}

ExclusionZoneController::~ExclusionZoneController() {}

void ExclusionZoneController::setTargetVehicle(Vehicle* vehicle)
{
    if (_targetVehicle != vehicle) {
        _targetVehicle = vehicle;
        emit targetVehicleChanged(_targetVehicle);
    }
}

int ExclusionZoneController::approvedCount() const
{
    int count = 0;
    for (int i = 0; i < _stagedZones->count(); i++) {
        if (qobject_cast<StagedExclusionZone*>(_stagedZones->get(i))->approved()) {
            count++;
        }
    }
    return count;
}

bool ExclusionZoneController::importFromFile(const QString& file)
{
    QList<QList<QGeoCoordinate>> polygons;
    QString errorString;
    if (!ShapeFileHelper::loadPolygonsFromFile(file, polygons, errorString)) {
        qCWarning(ExclusionZoneLog) << "loadPolygonsFromFile failed:" << errorString;
        return false;
    }
    if (polygons.isEmpty()) {
        qCWarning(ExclusionZoneLog) << "No polygons found in file:" << file;
        return false;
    }

    for (const QList<QGeoCoordinate>& vertices : polygons) {
        auto* polygon = new QGCFencePolygon(false /* inclusion */, this);
        polygon->appendVertices(vertices);
        auto* zone = new StagedExclusionZone(polygon, this);
        connect(zone, &StagedExclusionZone::approvedChanged, this, &ExclusionZoneController::approvedCountChanged);
        _stagedZones->append(zone);
    }

    emit approvedCountChanged();
    return true;
}

void ExclusionZoneController::setApproved(int index, bool approved)
{
    if (auto* zone = qobject_cast<StagedExclusionZone*>(_stagedZones->get(index))) {
        zone->setApproved(approved);
    }
}

bool ExclusionZoneController::pushApproved()
{
    if (!_targetVehicle) {
        emit pushFinished(false, tr("No target vehicle selected."));
        return false;
    }

    QList<StagedExclusionZone*> approvedZones;
    for (int i = 0; i < _stagedZones->count(); i++) {
        auto* zone = qobject_cast<StagedExclusionZone*>(_stagedZones->get(i));
        if (zone && zone->approved()) {
            approvedZones.append(zone);
        }
    }
    if (approvedZones.isEmpty()) {
        emit pushFinished(false, tr("No approved zones to push."));
        return false;
    }

    GeoFenceManager* fenceMgr = _targetVehicle->geoFenceManager();
    if (!fenceMgr) {
        emit pushFinished(false, tr("Target vehicle has no geofence support."));
        return false;
    }
    if (fenceMgr->inProgress()) {
        emit pushFinished(false, tr("A geofence sync is already in progress on this vehicle."));
        return false;
    }

    disconnect(_fenceLoadCompleteConnection);
    disconnect(_fenceLoadErrorConnection);
    disconnect(_fenceErrorConnection);
    disconnect(_fenceSendCompleteConnection);

    // Load the vehicle's current fence first. sendToVehicle() replaces the entire fence with
    // whatever it's given, so the approved zones must be merged into the live fence rather than
    // sent by themselves, or the push would silently delete the vehicle's inclusion polygon, any
    // other zones, and its breach-return point.
    _fenceLoadErrorConnection = connect(fenceMgr, &GeoFenceManager::error, this, [this](int, const QString& msg) {
        disconnect(_fenceLoadCompleteConnection);
        disconnect(_fenceLoadErrorConnection);
        emit pushFinished(false, tr("Failed to load current fence: %1").arg(msg));
    });
    _fenceLoadCompleteConnection =
        connect(fenceMgr, &GeoFenceManager::loadComplete, this, [this, fenceMgr, approvedZones]() {
            disconnect(_fenceLoadCompleteConnection);
            disconnect(_fenceLoadErrorConnection);
            _mergeAndSend(fenceMgr, approvedZones);
        });
    fenceMgr->loadFromVehicle();

    return true;
}

void ExclusionZoneController::_mergeAndSend(GeoFenceManager* fenceMgr, const QList<StagedExclusionZone*>& approvedZones)
{
    // 1. Merge: the vehicle's full current fence (just loaded above) plus the newly approved
    // exclusion polygons. Anything not included here is silently deleted from the vehicle by
    // sendToVehicle(), so the existing polygons/circles/breach-return must all be carried over.
    QJsonArray polygonArray;
    for (QGCFencePolygon polygon : fenceMgr->polygons()) {
        QJsonObject polygonJson;
        polygon.saveToJson(polygonJson);
        polygonArray.append(polygonJson);
    }
    for (StagedExclusionZone* zone : approvedZones) {
        QJsonObject polygonJson;
        zone->polygon()->saveToJson(polygonJson);
        polygonArray.append(polygonJson);
    }
    QJsonArray circleArray;
    for (QGCFenceCircle circle : fenceMgr->circles()) {
        QJsonObject circleJson;
        circle.saveToJson(circleJson);
        circleArray.append(circleJson);
    }
    QJsonObject fenceJson;
    fenceJson[QStringLiteral("version")] = 2;
    fenceJson[QStringLiteral("polygons")] = polygonArray;
    fenceJson[QStringLiteral("circles")] = circleArray;
    const QGeoCoordinate breachReturn = fenceMgr->breachReturnPoint();
    if (breachReturn.isValid()) {
        QJsonValue breachJson;
        GeoJsonHelper::saveGeoCoordinate(breachReturn, true /* writeAltitude */, breachJson);
        fenceJson[QStringLiteral("breachReturn")] = breachJson;
    }

    // 2. Write the audit-trail file.
    const QString dirPath =
        SettingsManager::instance()->appSettings()->missionSavePath() + QStringLiteral("/ExclusionZoneApprovals");
    QDir().mkpath(dirPath);
    const QString filePath =
        dirPath + QStringLiteral("/exclusion-zone-approval-%1.json")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
    QFile auditFile(filePath);
    if (!auditFile.open(QIODevice::WriteOnly)) {
        qCWarning(ExclusionZoneLog) << "Failed to open audit file for write:" << filePath;
        emit pushFinished(false, tr("Failed to write audit file: %1").arg(filePath));
        return;
    }
    auditFile.write(QJsonDocument(fenceJson).toJson());
    auditFile.close();
    qCDebug(ExclusionZoneLog) << "Wrote exclusion-zone audit file:" << filePath;

    // 3. Round-trip: read the audit file back from disk (not the in-memory JSON built above) and
    // re-parse it into fresh polygons/circles, proving what's on disk is what actually gets sent.
    QFile auditFileReadBack(filePath);
    if (!auditFileReadBack.open(QIODevice::ReadOnly)) {
        qCWarning(ExclusionZoneLog) << "Failed to read back audit file:" << filePath;
        emit pushFinished(false, tr("Failed to read back audit file: %1").arg(filePath));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument auditDoc = QJsonDocument::fromJson(auditFileReadBack.readAll(), &parseError);
    auditFileReadBack.close();
    if (parseError.error != QJsonParseError::NoError || !auditDoc.isObject()) {
        qCWarning(ExclusionZoneLog) << "Audit file read-back failed to parse:" << parseError.errorString();
        emit pushFinished(false, tr("Audit file read-back failed to parse: %1").arg(parseError.errorString()));
        return;
    }
    const QJsonObject auditJson = auditDoc.object();

    auto* sendPolygons = new QmlObjectListModel(this);
    auto* sendCircles = new QmlObjectListModel(this);
    QString loadError;
    for (const QJsonValue& value : auditJson[QStringLiteral("polygons")].toArray()) {
        auto* polygon = new QGCFencePolygon(false, sendPolygons);
        if (!polygon->loadFromJson(value.toObject(), true, loadError)) {
            qCWarning(ExclusionZoneLog) << "Audit round-trip failed:" << loadError;
            sendPolygons->deleteLater();
            sendCircles->deleteLater();
            emit pushFinished(false, tr("Audit round-trip failed: %1").arg(loadError));
            return;
        }
        sendPolygons->append(polygon);
    }
    for (const QJsonValue& value : auditJson[QStringLiteral("circles")].toArray()) {
        auto* circle = new QGCFenceCircle(sendCircles);
        if (!circle->loadFromJson(value.toObject(), loadError)) {
            qCWarning(ExclusionZoneLog) << "Audit round-trip failed:" << loadError;
            sendPolygons->deleteLater();
            sendCircles->deleteLater();
            emit pushFinished(false, tr("Audit round-trip failed: %1").arg(loadError));
            return;
        }
        sendCircles->append(circle);
    }
    QGeoCoordinate sendBreachReturn;
    if (auditJson.contains(QStringLiteral("breachReturn"))) {
        QString breachError;
        if (!GeoJsonHelper::loadGeoCoordinate(auditJson[QStringLiteral("breachReturn")], true /* altitudeRequired */,
                                              sendBreachReturn, breachError)) {
            qCWarning(ExclusionZoneLog) << "Audit round-trip breach-return failed:" << breachError;
            sendPolygons->deleteLater();
            sendCircles->deleteLater();
            emit pushFinished(false, tr("Audit round-trip breach-return failed: %1").arg(breachError));
            return;
        }
    }

    // 4. Send directly through the vehicle's own permanent GeoFenceManager - not through a
    // throwaway PlanMasterController/GeoFenceController. sendToVehicle() copies every
    // polygon/circle by value synchronously before the async MAVLink exchange starts, so
    // sendPolygons/sendCircles don't need to outlive this call.
    _fenceErrorConnection =
        connect(fenceMgr, &GeoFenceManager::error, this, [this](int, const QString& msg) { _lastFenceError = msg; });
    _fenceSendCompleteConnection =
        connect(fenceMgr, &GeoFenceManager::sendComplete, this, [this, approvedZones](bool error) {
            if (!error) {
                for (StagedExclusionZone* zone : approvedZones) {
                    _stagedZones->removeOne(zone);
                    zone->deleteLater();
                }
                emit approvedCountChanged();
            }
            emit pushFinished(!error, error ? _lastFenceError : QString());
        });

    fenceMgr->sendToVehicle(sendBreachReturn, *sendPolygons, *sendCircles);

    sendPolygons->deleteLater();
    sendCircles->deleteLater();
}
