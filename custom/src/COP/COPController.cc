#include "COPController.h"

#include <QtCore/QSettings>
#include <QtCore/QTimer>

#include "AudioOutput.h"
#include "MultiVehicleManager.h"
#include "QGCCorePlugin.h"
#include "VehicleLinkManager.h"
#include "VideoManager.h"

COPVehicle::COPVehicle(int sysid, VehicleRoleController* roles, QObject* parent)
    : QObject(parent)
    , _sysid(sysid)
    , _roles(roles)
    , _videoUri(QSettings().value(QStringLiteral("COP/Video/%1").arg(sysid)).toString())
{}

void COPVehicle::setVideoUri(const QString& uri)
{
    const QString value = uri.trimmed();
    if (_videoUri == value) {
        return;
    }
    _videoUri = value;
    QSettings().setValue(QStringLiteral("COP/Video/%1").arg(_sysid), value);
    emit videoUriChanged();
}

QString COPVehicle::label() const
{
    const QString role = _roles ? _roles->roleForSysid(_sysid) : QString();
    const QString name = _roles ? _roles->nameForSysid(_sysid) : QString();
    const QString base = role.isEmpty() ? tr("Vehicle %1").arg(_sysid) : role;
    return name.isEmpty() ? base : tr("%1 · %2").arg(base, name);
}

bool COPVehicle::connected() const
{
    return _vehicle && !_vehicle->vehicleLinkManager()->communicationLost();
}

void COPVehicle::_snapshot()
{
    if (!_vehicle) {
        return;
    }
    _coordinate = _vehicle->coordinate();
    _flightMode = _vehicle->flightMode();
    _lastSeen = QDateTime::currentDateTimeUtc();
    emit stateChanged();
}

void COPVehicle::setVehicle(Vehicle* vehicle)
{
    if (_vehicle == vehicle) {
        return;
    }
    if (_vehicle) {
        disconnect(_vehicle, nullptr, this, nullptr);
        disconnect(_vehicle->vehicleLinkManager(), nullptr, this, nullptr);
    }
    _vehicle = vehicle;
    if (vehicle) {
        connect(vehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this,
                &COPVehicle::stateChanged);
        connect(vehicle, &QObject::destroyed, this, [this]() { emit stateChanged(); });
        connect(vehicle, &Vehicle::coordinateChanged, this, &COPVehicle::_snapshot);
        connect(vehicle, &Vehicle::flightModeChanged, this, &COPVehicle::_snapshot);
        _snapshot();
    }
    emit stateChanged();
}

COPController::COPController(QObject* parent)
    : QObject(parent)
{}

COPVehicle* COPController::_find(int sysid) const
{
    for (int i = 0; i < _vehicles.count(); ++i) {
        auto* entry = _vehicles.value<COPVehicle*>(i);
        if (entry && entry->sysid() == sysid) {
            return entry;
        }
    }
    return nullptr;
}

COPVehicle* COPController::selected() const
{
    return _find(_selectedSysid);
}

COPVehicle* COPController::_remember(int sysid)
{
    if (sysid < 1 || sysid > 255) {
        return nullptr;
    }
    if (auto* entry = _find(sysid)) {
        return entry;
    }
    auto* entry = new COPVehicle(sysid, _roles, this);
    _vehicles.append(entry);
    QVariantList ids;
    for (int i = 0; i < _vehicles.count(); ++i) {
        ids.append(_vehicles.value<COPVehicle*>(i)->sysid());
    }
    QSettings().setValue(QStringLiteral("COP/VehicleOrder"), ids);
    return entry;
}

void COPController::initialize(VehicleRoleController* roles)
{
    if (_initialized || !roles) {
        return;
    }
    _initialized = true;
    _roles = roles;
    connect(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification, this, &COPController::notify);
    const auto ids = QSettings().value(QStringLiteral("COP/VehicleOrder")).toList();
    for (const auto& id : ids) {
        _remember(id.toInt());
    }
    _syncRoles();
    connect(roles->roleEntries(), &QAbstractItemModel::rowsInserted, this, &COPController::_syncRoles);
    connect(roles->roleEntries(), &QAbstractItemModel::rowsRemoved, this, &COPController::_syncRoles);
    auto* manager = MultiVehicleManager::instance();
    connect(manager, &MultiVehicleManager::vehicleAdded, this, &COPController::_vehicleAdded);
    connect(manager, &MultiVehicleManager::vehicleRemoved, this, &COPController::_vehicleRemoved);
    connect(manager, &MultiVehicleManager::activeVehicleChanged, this, [this, manager](Vehicle* vehicle) {
        if (_activationInFlight == vehicle) {
            _activationInFlight.clear();
            _queueActivation(_controlRequestToken);
        }
        const QPointer<Vehicle> restore = _activeBeforeRemoval;
        _activeBeforeRemoval.clear();
        if (restore && restore != vehicle && manager->vehicles()->contains(restore)) {
            _requestActivation(restore);
        }
    });
    for (int i = 0; i < manager->vehicles()->count(); ++i) {
        _vehicleAdded(manager->vehicles()->value<Vehicle*>(i));
    }
}

void COPController::_syncRoles()
{
    if (!_roles) {
        return;
    }
    for (int i = 0; i < _roles->roleEntries()->count(); ++i) {
        auto* role = _roles->roleEntries()->value<VehicleRoleEntry*>(i);
        if (!role) {
            continue;
        }
        auto* entry = _remember(role->sysid());
        if (entry) {
            connect(role, &VehicleRoleEntry::roleChanged, entry, &COPVehicle::labelChanged, Qt::UniqueConnection);
            connect(role, &VehicleRoleEntry::nameChanged, entry, &COPVehicle::labelChanged, Qt::UniqueConnection);
        }
    }
    for (int i = 0; i < _vehicles.count(); ++i) {
        emit _vehicles.value<COPVehicle*>(i)->labelChanged();
    }
}

void COPController::_vehicleAdded(Vehicle* vehicle)
{
    if (!vehicle) {
        return;
    }
    auto* entry = _remember(vehicle->id());
    if (!entry) {
        return;
    }
    if (entry->vehicle() == vehicle) {
        return;
    }
    entry->setVehicle(vehicle);
    connect(vehicle, &Vehicle::textMessageReceived, this,
            [this, entry](int, int, int severity, const QString& text, const QString&) {
                // Emergency/Alert require immediate action; subsystem health remains in Vehicle's message display.
                if (severity >= MAV_SEVERITY_EMERGENCY && severity <= MAV_SEVERITY_ALERT) {
                    notify(tr("%1: %2").arg(entry->label(), text));
                }
            });
    auto* links = vehicle->vehicleLinkManager();
    auto* relayTimer = new QTimer(vehicle);
    relayTimer->setSingleShot(true);
    relayTimer->setInterval(60000);
    connect(entry, &COPVehicle::stateChanged, relayTimer, [entry, relayTimer]() {
        if (!entry->vehicle()) {
            relayTimer->stop();
        }
    });
    connect(relayTimer, &QTimer::timeout, this, [this, entry]() {
        notify(tr("%1: No telemetry for over 60 seconds. Review aircraft status and consider a relay-related action.")
                   .arg(entry->label()));
    });
    connect(links, &VehicleLinkManager::communicationLostChanged, this, [this, entry, relayTimer](bool lost) {
        if (lost) {
            notify(tr("%1: Communication lost").arg(entry->label()));
        }
        Vehicle* current = entry->vehicle();
        if (!lost && current && _pendingSysid == entry->sysid()) {
            _requestActivation(current);
            _pendingSysid = 0;
            emit selectionChanged();
        }
        if (lost && current && current->rover()) {
            relayTimer->start();
        } else {
            relayTimer->stop();
        }
    });
    if (_pendingSysid == vehicle->id()) {
        _requestActivation(vehicle);
        _pendingSysid = 0;
    }
    emit selectionChanged();
}

void COPController::_vehicleRemoved(Vehicle* vehicle)
{
    if (!vehicle) {
        return;
    }
    auto* manager = MultiVehicleManager::instance();
    Vehicle* active = manager->activeVehicle();
    if (active && active != vehicle && manager->vehicles()->contains(active) && !_requestedVehicle &&
        !_activationInFlight) {
        _activeBeforeRemoval = active;
    } else {
        _activeBeforeRemoval.clear();
    }
    if (auto* entry = _find(vehicle->id()); entry && entry->vehicle() == vehicle) {
        entry->setVehicle(nullptr);
        disconnect(vehicle, nullptr, this, nullptr);
        disconnect(vehicle->vehicleLinkManager(), nullptr, this, nullptr);
        notify(tr("%1 disconnected; showing last received state.").arg(entry->label()));
    }
    connect(vehicle, &QObject::destroyed, this, [this]() {
        _activeBeforeRemoval.clear();
        _queueActivation(_controlRequestToken);
    });
    emit selectionChanged();
}

void COPController::selectVehicle(int sysid)
{
    if (sysid != 0 && !_find(sysid)) {
        return;
    }
    ++_controlRequestToken;
    _requestedVehicle.clear();
    _activeBeforeRemoval.clear();
    _pendingSysid = 0;
    _selectedSysid = sysid;
    if (sysid == 0) {
        VideoManager::instance()->stopVideo();
    }
    emit selectionChanged();
    if (sysid != 0) {
        VideoManager::instance()->startVideo();
    }
}

void COPController::assumeControl()
{
    auto* entry = selected();
    if (!entry) {
        return;
    }
    ++_controlRequestToken;
    _requestedVehicle.clear();
    _activeBeforeRemoval.clear();
    Vehicle* vehicle = entry->connected() ? entry->vehicle() : nullptr;
    _pendingSysid = vehicle ? 0 : entry->sysid();
    if (vehicle) {
        _requestActivation(vehicle);
    }
    emit selectionChanged();
}

void COPController::_requestActivation(Vehicle* vehicle)
{
    _requestedVehicle = vehicle;
    _queueActivation(++_controlRequestToken);
}

void COPController::_queueActivation(quint64 token)
{
    QTimer::singleShot(0, this, [this, token]() {
        if (token != _controlRequestToken || _activationInFlight || !_requestedVehicle) {
            return;
        }
        auto* manager = MultiVehicleManager::instance();
        Vehicle* vehicle = _requestedVehicle;
        _requestedVehicle.clear();
        if (!vehicle || !manager->vehicles()->contains(vehicle) || vehicle->vehicleLinkManager()->communicationLost() ||
            manager->activeVehicle() == vehicle) {
            return;
        }
        // Wait for the manager's deferred switch before submitting the next request.
        _activationInFlight = vehicle;
        manager->setActiveVehicle(vehicle);
    });
}

void COPController::cancelControl()
{
    ++_controlRequestToken;
    _requestedVehicle.clear();
    _pendingSysid = 0;
    emit selectionChanged();
}

void COPController::notify(const QString& message)
{
    if (message.trimmed().isEmpty()) {
        return;
    }
    _messages.prepend(
        QVariantMap{{QStringLiteral("text"), message}, {QStringLiteral("time"), QDateTime::currentDateTime()}});
    ++_unacknowledgedCount;
    while (_messages.size() > qMax(200, qMin(_unacknowledgedCount, 1000))) {
        _messages.removeLast();
    }
    if (_unacknowledgedCount > 1000) {
        _unacknowledgedCount = 1000;
        ++_droppedMessages;
    }
    _unacknowledged = true;
    emit messagesChanged();
    AudioOutput::instance()->say(tr("New operator notification"));
}

void COPController::acknowledge()
{
    _unacknowledged = false;
    _unacknowledgedCount = 0;
    _droppedMessages = 0;
    while (_messages.size() > 200) {
        _messages.removeLast();
    }
    emit messagesChanged();
}
