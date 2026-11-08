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
#include "MultiVehicleManager.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "ShapeFileHelper.h"
#include "StagedExclusionZone.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

QGC_LOGGING_CATEGORY(ExclusionZoneLog, "Custom.ExclusionZone")

ExclusionZoneController::ExclusionZoneController(QObject* parent)
    : QObject(parent)
    , _stagedZones(new QmlObjectListModel(this))
{
    _transactionTimer.setSingleShot(true);
    // Bound even a manager which silently declines an operation; each phase gets two minutes.
    _transactionTimer.setInterval(120000);
    connect(&_transactionTimer, &QTimer::timeout, this,
            [this]() { _failTransaction(tr("The fence operation timed out.")); });
    connect(MultiVehicleManager::instance(), &MultiVehicleManager::vehicleRemoved, this, [this](Vehicle* vehicle) {
        if (_pushInProgress && _transactionVehicle == vehicle) {
            _failTransaction(tr("The vehicle disconnected during the push."));
        }
        if (_targetVehicle == vehicle) {
            setTargetVehicle(nullptr);
        }
    });
}

ExclusionZoneController::~ExclusionZoneController() {}

Vehicle* ExclusionZoneController::targetVehicle() const
{
    return _targetVehicle.data();
}

void ExclusionZoneController::_clearTransaction()
{
    disconnect(_fenceLoadCompleteConnection);
    disconnect(_fenceLoadErrorConnection);
    disconnect(_fenceErrorConnection);
    disconnect(_fenceSendCompleteConnection);
    disconnect(_transactionDestroyedConnection);
    disconnect(_transactionLinkConnection);
    disconnect(_transactionCommunicationConnection);
    disconnect(_fenceRemoveConnection);
    _transactionTimer.stop();
    ++_transactionId;  // Invalidate a queued verification from an earlier push.
    _transactionFenceManager.clear();
    _expectedFence.clear();
    _approvedZones.clear();
    _approvedPolygons = QJsonArray();
    _pushPhase = PushPhase::Idle;
    _recoveryRequiresReadback = false;
    _transactionVehicle.clear();
    _pushInProgress = false;
}

void ExclusionZoneController::_failTransaction(const QString& reason)
{
    if (!_pushInProgress) {
        return;
    }
    QString message = reason;
    if (_recoveryRequiresReadback || (_transactionFenceManager && _transactionFenceManager->uploadRequiresReadback() &&
                                      _pushPhase != PushPhase::Loading)) {
        message += tr(" The fence upload needed a full restart, so its stored contents require readback verification.");
    }
    message += _pushPhase == PushPhase::Loading
                   ? tr(" No zones were sent. Check the connection and try again.")
                   : tr(" The zones may be on the vehicle, but installation could not be verified. "
                        "Read the fence back in Plan view before relying on it.");
    if (_pushPhase != PushPhase::Loading && _approvedZonesChanged()) {
        message += tr(" Approved staged zones were removed during the push.");
    }
    _clearTransaction();
    emit pushFinished(false, message);
}

void ExclusionZoneController::setTargetVehicle(Vehicle* vehicle)
{
    if (_targetVehicle != vehicle) {
        if (_pushInProgress) {
            _failTransaction(vehicle ? tr("The target vehicle changed during the push.")
                                     : tr("The vehicle disconnected during the push."));
        }
        disconnect(_targetDestroyedConnection);
        _targetVehicle = vehicle;
        if (vehicle) {
            _targetDestroyedConnection = connect(vehicle, &QObject::destroyed, this, [this]() {
                _targetVehicle.clear();
                emit targetVehicleChanged(nullptr);
            });
        }
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
    if (_pushInProgress) {
        emit pushFinished(false, tr("Zones are already being sent or verified. Wait for that to finish."));
        return false;
    }

    if (!_targetVehicle) {
        emit pushFinished(false, tr("Pick a vehicle to send the zones to."));
        return false;
    }

    QList<QPointer<StagedExclusionZone>> approvedZones;
    QJsonArray approvedPolygons;
    for (int i = 0; i < _stagedZones->count(); i++) {
        auto* zone = qobject_cast<StagedExclusionZone*>(_stagedZones->get(i));
        if (zone && zone->approved()) {
            approvedZones.append(QPointer<StagedExclusionZone>(zone));
            QJsonObject polygonJson;
            zone->polygon()->saveToJson(polygonJson);
            approvedPolygons.append(polygonJson);
        }
    }
    if (approvedZones.isEmpty()) {
        emit pushFinished(false, tr("No zones are approved yet. Approve at least one, then send."));
        return false;
    }

    GeoFenceManager* fenceMgr = _targetVehicle->geoFenceManager();
    if (!fenceMgr || !fenceMgr->supported()) {
        emit pushFinished(false, tr("This vehicle can't accept geofences, so the zones can't be sent to it."));
        return false;
    }
    if (_targetVehicle->isOfflineEditingVehicle() || _targetVehicle->vehicleLinkManager()->communicationLost() ||
        _targetVehicle->vehicleLinkManager()->primaryLink().expired()) {
        emit pushFinished(false, tr("The vehicle is disconnected. Reconnect before sending the zones."));
        return false;
    }
    if (fenceMgr->inProgress()) {
        emit pushFinished(false, tr("This vehicle is busy updating its fence. Try again in a moment."));
        return false;
    }

    _pushInProgress = true;
    _pushPhase = PushPhase::Loading;
    _transactionVehicle = _targetVehicle;
    _transactionFenceManager = fenceMgr;
    _approvedZones = approvedZones;
    _approvedPolygons = approvedPolygons;
    _transactionTimer.start();
    _transactionDestroyedConnection = connect(_transactionVehicle, &QObject::destroyed, this, [this]() {
        _failTransaction(tr("The vehicle disconnected during the push."));
    });
    auto* linkManager = _transactionVehicle->vehicleLinkManager();
    _transactionLinkConnection = connect(linkManager, &VehicleLinkManager::allLinksRemoved, this, [this]() {
        _failTransaction(tr("The vehicle disconnected during the push."));
    });
    _transactionCommunicationConnection =
        connect(linkManager, &VehicleLinkManager::communicationLostChanged, this, [this](bool lost) {
            if (lost) {
                _failTransaction(tr("Communication with the vehicle was lost during the push."));
            }
        });
    _fenceRemoveConnection = connect(fenceMgr, &GeoFenceManager::removeAllComplete, this, [this]() {
        _failTransaction(tr("Another fence operation interrupted the push."));
    });
    // Load the vehicle's current fence first. sendToVehicle() replaces the entire fence with
    // whatever it's given, so the approved zones must be merged into the live fence rather than
    // sent by themselves, or the push would silently delete the vehicle's inclusion polygon, any
    // other zones, and its breach-return point.
    const quint64 transactionId = _transactionId;
    _fenceLoadErrorConnection =
        connect(fenceMgr, &GeoFenceManager::error, this, [this, transactionId](int, const QString& msg) {
            if (transactionId == _transactionId) {
                _failTransaction(tr("Couldn't read the vehicle's current fence. (%1)").arg(msg));
            }
        });
    _fenceLoadCompleteConnection =
        connect(fenceMgr, &GeoFenceManager::loadComplete, this, [this, fenceMgr, transactionId]() {
            if (!_pushInProgress || transactionId != _transactionId || _pushPhase != PushPhase::Loading) {
                return;
            }
            disconnect(_fenceLoadCompleteConnection);
            disconnect(_fenceLoadErrorConnection);
            if (!_transactionVehicle || _targetVehicle != _transactionVehicle ||
                _transactionVehicle->geoFenceManager() != fenceMgr || fenceMgr->inProgress()) {
                _failTransaction(tr("The fence operation was interrupted before sending."));
                return;
            }
            _mergeAndSend(fenceMgr);
        });
    fenceMgr->loadFromVehicle();

    return true;
}

bool ExclusionZoneController::_approvedZonesChanged() const
{
    for (const auto& zone : _approvedZones) {
        if (!zone || !_stagedZones->contains(zone.data())) {
            return true;
        }
    }
    return false;
}

void ExclusionZoneController::_mergeAndSend(GeoFenceManager* fenceMgr)
{
    if (_approvedZonesChanged()) {
        _failTransaction(tr("The staged zones changed before the merge."));
        return;
    }
    // 1. Merge: the vehicle's full current fence (just loaded above) plus the newly approved
    // exclusion polygons. Anything not included here is silently deleted from the vehicle by
    // sendToVehicle(), so the existing polygons/circles/breach-return must all be carried over.
    QJsonArray polygonArray;
    for (QGCFencePolygon polygon : fenceMgr->polygons()) {
        QJsonObject polygonJson;
        polygon.saveToJson(polygonJson);
        polygonArray.append(polygonJson);
    }
    for (const QJsonValue& polygon : _approvedPolygons) {
        polygonArray.append(polygon);
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
    // GeoFenceManager retains its cached return point when a download has none.
    QGeoCoordinate breachReturn;
    for (const MissionItem* item : fenceMgr->missionItems()) {
        if (item->command() == MAV_CMD_NAV_FENCE_RETURN_POINT) {
            breachReturn = QGeoCoordinate(item->param5(), item->param6(), item->param7());
        }
    }
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
        _clearTransaction();
        emit pushFinished(false,
                          tr("The audit record could not be written to %1, so no zones were sent.").arg(filePath));
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
        _clearTransaction();
        emit pushFinished(false,
                          tr("The audit record could not be read back from %1, so no zones were sent.").arg(filePath));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument auditDoc = QJsonDocument::fromJson(auditFileReadBack.readAll(), &parseError);
    auditFileReadBack.close();
    if (parseError.error != QJsonParseError::NoError || !auditDoc.isObject()) {
        qCWarning(ExclusionZoneLog) << "Audit file read-back failed to parse:" << parseError.errorString();
        _clearTransaction();
        emit pushFinished(
            false,
            tr("The audit record could not be checked (%1), so no zones were sent.").arg(parseError.errorString()));
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
            _clearTransaction();
            emit pushFinished(
                false, tr("The audit record did not match the zones (%1), so no zones were sent.").arg(loadError));
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
            _clearTransaction();
            emit pushFinished(
                false, tr("The audit record did not match the zones (%1), so no zones were sent.").arg(loadError));
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
            _clearTransaction();
            emit pushFinished(false,
                              tr("The audit record did not match the breach-return point (%1), so no zones were sent.")
                                  .arg(breachError));
            return;
        }
    }

    // Snapshot the audited objects in the same ordered representation sendToVehicle serializes.
    for (int i = 0; i < sendPolygons->count(); ++i) {
        const auto* polygon = sendPolygons->value<QGCFencePolygon*>(i);
        const int command = polygon->inclusion() ? MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
                                                 : MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
        for (const QVariant& vertex : polygon->path()) {
            _expectedFence.append(
                {command, MAV_FRAME_GLOBAL, double(polygon->count()), vertex.value<QGeoCoordinate>()});
        }
    }
    for (int i = 0; i < sendCircles->count(); ++i) {
        auto* circle = sendCircles->value<QGCFenceCircle*>(i);
        _expectedFence.append(
            {circle->inclusion() ? MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION : MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION,
             MAV_FRAME_GLOBAL, static_cast<float>(circle->radius()->rawValue().toDouble()), circle->center()});
    }
    if (sendBreachReturn.isValid()) {
        // Preserve unspecified altitude (NaN); finite altitude is transmitted as float32.
        QGeoCoordinate expectedReturn = sendBreachReturn;
        expectedReturn.setAltitude(static_cast<float>(sendBreachReturn.altitude()));
        _expectedFence.append({MAV_CMD_NAV_FENCE_RETURN_POINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, expectedReturn});
    }

    _pushPhase = PushPhase::Sending;
    _transactionTimer.start();
    const quint64 transactionId = _transactionId;
    _fenceErrorConnection =
        connect(fenceMgr, &GeoFenceManager::error, this, [this, transactionId](int, const QString& msg) {
            if (transactionId == _transactionId) {
                _failTransaction(tr("The fence transfer or verification failed. (%1)").arg(msg));
            }
        });
    _fenceSendCompleteConnection =
        connect(fenceMgr, &GeoFenceManager::sendComplete, this, [this, fenceMgr, transactionId](bool error) {
            if (!_pushInProgress || transactionId != _transactionId) {
                return;
            }
            if (_pushPhase != PushPhase::Sending) {
                _failTransaction(tr("Another fence upload interrupted verification."));
                return;
            }
            _recoveryRequiresReadback = fenceMgr->uploadRequiresReadback();
            if (error) {
                _failTransaction(tr("The vehicle did not accept the fence."));
                return;
            }
            _pushPhase = PushPhase::VerificationPending;
            // Leave PlanManager's terminal signal stack before starting a new transaction.
            QTimer::singleShot(0, this, [this, transactionId]() { _startVerification(transactionId); });
        });

    fenceMgr->sendToVehicle(sendBreachReturn, *sendPolygons, *sendCircles);

    sendPolygons->deleteLater();
    sendCircles->deleteLater();
}

void ExclusionZoneController::_startVerification(quint64 transactionId)
{
    if (!_pushInProgress || transactionId != _transactionId) {
        return;
    }
    GeoFenceManager* fenceMgr = _transactionFenceManager.data();
    if (!_transactionVehicle || _targetVehicle != _transactionVehicle || !fenceMgr ||
        _transactionVehicle->geoFenceManager() != fenceMgr) {
        _failTransaction(tr("The target vehicle changed before fence verification."));
        return;
    }
    if (fenceMgr->inProgress()) {
        _failTransaction(tr("The vehicle's fence manager is busy, so verification could not start."));
        return;
    }
    _pushPhase = PushPhase::Verifying;
    _transactionTimer.start();
    _fenceLoadCompleteConnection =
        connect(fenceMgr, &GeoFenceManager::loadComplete, this, [this, fenceMgr, transactionId]() {
            if (!_pushInProgress || transactionId != _transactionId) {
                return;
            }
            if (!_transactionVehicle || _targetVehicle != _transactionVehicle ||
                _transactionVehicle->geoFenceManager() != fenceMgr || fenceMgr->inProgress()) {
                _failTransaction(tr("The fence verification was interrupted."));
                return;
            }
            if (!_fenceMatches(fenceMgr)) {
                _failTransaction(tr("The fence read back from the vehicle does not match the fence sent."));
                return;
            }
            const auto approvedZones = _approvedZones;
            _clearTransaction();
            int completed = 0;
            for (const auto& zone : approvedZones) {
                if (zone && _stagedZones->contains(zone.data())) {
                    _stagedZones->removeOne(zone.data());
                    if (zone) {
                        zone->deleteLater();
                        ++completed;
                    }
                }
            }
            if (completed > 0) {
                emit approvedCountChanged();
            }
            QString message;
            if (completed != approvedZones.count()) {
                message = tr("The vehicle fence was verified. Completed %1 surviving staged zones; "
                             "%2 approved staged zones were removed during the push.")
                              .arg(completed)
                              .arg(approvedZones.count() - completed);
            }
            emit pushFinished(completed > 0, message);
        });
    fenceMgr->loadFromVehicle();
}

bool ExclusionZoneController::_fenceMatches(GeoFenceManager* fenceMgr) const
{
    struct FenceShape
    {
        int command;
        double parameter;
        QList<QGeoCoordinate> vertices;
    };

    const auto parse = [](const QList<FenceItem>& items, QList<FenceShape>& shapes) {
        bool hasReturn = false;
        for (qsizetype i = 0; i < items.count();) {
            const FenceItem& first = items[i];
            if (!qIsFinite(first.parameter)) {
                return false;
            }
            const bool polygon = first.command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION ||
                                 first.command == MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION;
            const bool circle = first.command == MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION ||
                                first.command == MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION;
            const bool returnPoint = first.command == MAV_CMD_NAV_FENCE_RETURN_POINT;
            qsizetype count = 1;
            if (polygon) {
                // Bound before converting, and require a complete, contiguous group with one count/type.
                if (first.parameter < 3 || first.parameter > items.count() - i) {
                    return false;
                }
                count = static_cast<qsizetype>(first.parameter);
                if (first.parameter != count) {
                    return false;
                }
            } else if (returnPoint) {
                if (hasReturn || (!qIsFinite(first.coordinate.altitude()) && !qIsNaN(first.coordinate.altitude()))) {
                    return false;
                }
                hasReturn = true;
            } else if (!circle) {
                return false;
            }
            FenceShape shape{first.command, first.parameter, {}};
            for (qsizetype j = 0; j < count; ++j) {
                const FenceItem& item = items[i + j];
                if (item.command != first.command || item.parameter != first.parameter ||
                    item.frame != (returnPoint ? MAV_FRAME_GLOBAL_RELATIVE_ALT : MAV_FRAME_GLOBAL) ||
                    !item.coordinate.isValid() || !qIsFinite(item.coordinate.latitude()) ||
                    !qIsFinite(item.coordinate.longitude())) {
                    return false;
                }
                shape.vertices.append(item.coordinate);
            }
            shapes.append(shape);
            i += count;
        }
        return true;
    };

    QList<FenceItem> actualItems;
    for (const MissionItem* item : fenceMgr->missionItems()) {
        if (!item) {
            return false;
        }
        actualItems.append({item->command(), item->frame(), item->param1(),
                            QGeoCoordinate(item->param5(), item->param6(), item->param7())});
    }
    QList<FenceShape> expected;
    QList<FenceShape> actual;
    if (!parse(_expectedFence, expected) || !parse(actualItems, actual) || expected.count() != actual.count()) {
        return false;
    }
    const auto samePosition = [](const QGeoCoordinate& a, const QGeoCoordinate& b) {
        // One int32 1e7 coordinate step plus floating-point slack (~1.2 cm latitude).
        constexpr double COORDINATE_TOLERANCE_DEGREES = 1.1e-7;
        return qAbs(a.latitude() - b.latitude()) <= COORDINATE_TOLERANCE_DEGREES &&
               qAbs(a.longitude() - b.longitude()) <= COORDINATE_TOLERANCE_DEGREES;
    };
    const auto sameShape = [&samePosition](const FenceShape& a, const FenceShape& b) {
        if (a.command != b.command || a.parameter != b.parameter || a.vertices.count() != b.vertices.count()) {
            return false;
        }
        if (a.command == MAV_CMD_NAV_FENCE_RETURN_POINT) {
            const double altitudeA = a.vertices.first().altitude();
            const double altitudeB = b.vertices.first().altitude();
            return samePosition(a.vertices.first(), b.vertices.first()) &&
                   ((qIsNaN(altitudeA) && qIsNaN(altitudeB)) || altitudeA == altitudeB);
        }
        const qsizetype count = a.vertices.count();
        for (qsizetype offset = 0; offset < count; ++offset) {
            for (int direction : {1, -1}) {
                bool matches = true;
                for (qsizetype j = 0; j < count; ++j) {
                    if (!samePosition(a.vertices[j], b.vertices[(offset + direction * j + count) % count])) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return true;
                }
            }
        }
        return false;
    };

    // Tolerance can give one shape several candidates. Find a full one-to-one matching rather than
    // making a greedy choice that could reject an equivalent multiset solely because of its order.
    QList<qsizetype> matchedExpected(actual.count(), -1);
    const auto match = [&](auto&& self, qsizetype index, QList<bool>& visited) -> bool {
        for (qsizetype j = 0; j < actual.count(); ++j) {
            if (visited[j] || !sameShape(expected[index], actual[j])) {
                continue;
            }
            visited[j] = true;
            if (matchedExpected[j] == -1 || self(self, matchedExpected[j], visited)) {
                matchedExpected[j] = index;
                return true;
            }
        }
        return false;
    };
    for (qsizetype i = 0; i < expected.count(); ++i) {
        QList<bool> visited(actual.count(), false);
        if (!match(match, i, visited)) {
            return false;
        }
    }
    return true;
}
