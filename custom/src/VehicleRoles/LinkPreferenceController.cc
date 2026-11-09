#include "LinkPreferenceController.h"

#include <QtCore/QCoreApplication>

#include "GeoFenceManager.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QGCCorePlugin.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VehicleRoleController.h"

QGC_LOGGING_CATEGORY(LinkPreferenceLog, "Custom.LinkPreference")

struct LinkPreferenceController::State : QObject
{
    State(Vehicle* owner, const LinkPreferencePolicy::Timing& timing, QObject* parent)
        : QObject(parent)
        , vehicle(owner)
        , policy(timing)
    {}

    QPointer<Vehicle> vehicle;
    LinkPreferencePolicy policy;
    bool switching = false;
    bool queued = false;
};

LinkPreferenceController::LinkPreferenceController(VehicleRoleController* roles)
    : QObject(roles)
    , _roles(roles)
{
    _clock.start();
    _timer.setInterval(1000);
    connect(&_timer, &QTimer::timeout, this, &LinkPreferenceController::_refresh);
    connect(roles, &VehicleRoleController::linkPreferencesChanged, this, &LinkPreferenceController::_refresh);
    connect(roles, &VehicleRoleController::failbackHoldSecondsChanged, this, &LinkPreferenceController::_refresh);
    auto* manager = MultiVehicleManager::instance();
    connect(manager, &MultiVehicleManager::vehicleAdded, this, &LinkPreferenceController::_vehicleAdded);
    connect(manager, &MultiVehicleManager::vehicleRemoved, this, &LinkPreferenceController::_vehicleRemoved);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
        _timer.stop();
        const auto vehicles = _states.keys();
        for (auto* vehicle : vehicles) {
            _vehicleRemoved(vehicle);
        }
    });
    for (int i = 0; i < manager->vehicles()->count(); ++i) {
        _vehicleAdded(manager->vehicles()->value<Vehicle*>(i));
    }
}

void LinkPreferenceController::_vehicleAdded(Vehicle* vehicle)
{
    if (!vehicle || _states.contains(vehicle)) {
        return;
    }
    auto* state = new State(vehicle, _timing, this);
    _states.insert(vehicle, state);
    auto* links = vehicle->vehicleLinkManager();
    const auto changed = [this, state]() {
        _observe(state);
        _refresh();
    };
    connect(links, &VehicleLinkManager::linkNamesChanged, state, changed);
    connect(links, &VehicleLinkManager::linkStatusesChanged, state, changed);
    connect(links, &VehicleLinkManager::primaryLinkChanged, state, [this, state]() {
        _observe(state, true);
        _refresh();
    });
    connect(links, &VehicleLinkManager::communicationLostChanged, state, changed);
    connect(vehicle, &Vehicle::initialConnectComplete, state, changed);
    if (auto* parameters = vehicle->parameterManager()) {
        connect(parameters, &ParameterManager::parametersReadyChanged, state, changed);
        connect(parameters, &ParameterManager::pendingWritesChanged, state, changed);
        connect(parameters, &ParameterManager::loadProgressChanged, state, changed);
    }
    for (PlanManager* plan :
         {static_cast<PlanManager*>(vehicle->missionManager()), static_cast<PlanManager*>(vehicle->geoFenceManager()),
          static_cast<PlanManager*>(vehicle->rallyPointManager())}) {
        if (plan) {
            connect(plan, &PlanManager::inProgressChanged, state, changed);
        }
    }
    connect(vehicle, &QObject::destroyed, this, [this, vehicle]() { _vehicleRemoved(vehicle); });
    _observe(state);
    _refresh();
}

void LinkPreferenceController::_vehicleRemoved(Vehicle* vehicle)
{
    delete _states.take(vehicle);
    _refresh();
}

qint64 LinkPreferenceController::_now() const
{
#ifdef QGC_UNITTEST_BUILD
    if (_testNow >= 0) {
        return _testNow;
    }
#endif
    return _clock.elapsed();
}

LinkPreferencePolicy::Input LinkPreferenceController::_input(Vehicle* vehicle) const
{
    if (!vehicle || !_roles) {
        return {};
    }
    auto* links = vehicle->vehicleLinkManager();
    auto* parameters = vehicle->parameterManager();
    auto* mission = vehicle->missionManager();
    auto* fence = vehicle->geoFenceManager();
    auto* rally = vehicle->rallyPointManager();
    if (!links || !parameters || !mission || !fence || !rally) {
        return {};
    }
    // Public progress covers visible refreshes, but is not an aggregate parameter-read busy flag.
    return {.preferredName = _roles->preferredLinkConfigNameForSysid(vehicle->id()),
            .linkNames = links->linkNames(),
            .linkStatuses = links->linkStatuses(),
            .primaryName = links->primaryLinkName(),
            .initialConnectDone = vehicle->isInitialConnectComplete(),
            .transferActive = !parameters->parametersReady() || parameters->pendingWrites() ||
                              parameters->loadProgress() > 0 || mission->inProgress() || fence->inProgress() ||
                              rally->inProgress()};
}

void LinkPreferenceController::_observe(State* state, bool primarySignal)
{
    if (!state || !state->vehicle) {
        return;
    }
    state->policy.observe(_input(state->vehicle), _now(), state->switching, primarySignal);
}

void LinkPreferenceController::_refresh()
{
    bool attached = false;
    for (auto* state : _states) {
        if (!state->vehicle) {
            continue;
        }
        const auto input = _input(state->vehicle);
        attached = attached || (!input.preferredName.isEmpty() && input.linkNames.contains(input.preferredName));
        qint64 baseHoldMs = _roles ? _roles->failbackHoldSeconds() * 1000LL : 15000;
#ifdef QGC_UNITTEST_BUILD
        if (_testNow >= 0) {
            baseHoldMs = _timing.baseHoldMs;
        }
#endif
        state->policy.setBaseHoldMs(baseHoldMs);
        _observe(state);
        if (!state->queued) {
            state->queued = true;
            // Stock emits status changes before completing its own failover or link removal.
            QTimer::singleShot(0, state, [this, state]() {
                state->queued = false;
                _evaluate(state);
            });
        }
    }
    if (attached) {
        if (!_timer.isActive()) {
            _timer.start();
        }
    } else {
        _timer.stop();
    }
}

void LinkPreferenceController::_evaluate(State* state)
{
    if (!state || !state->vehicle || !_roles || QCoreApplication::closingDown()) {
        return;
    }
    const QString target = state->policy.evaluate(_input(state->vehicle), _now());
    if (target.isEmpty()) {
        return;
    }
    const auto current = _input(state->vehicle);
    if (current.preferredName != target || !LinkPreferencePolicy::preferredHealthy(current) ||
        !current.initialConnectDone || current.transferActive) {
        state->policy.cancelPendingSwitch();
        return;
    }
    QPointer<State> guard(state);
    QPointer<Vehicle> vehicle = state->vehicle;
    if (!vehicle) {
        state->policy.cancelPendingSwitch();
        return;
    }
    state->switching = true;
    vehicle->vehicleLinkManager()->setPrimaryLinkByName(target);
    if (!guard || !vehicle || !_roles) {
        return;
    }
    state->switching = false;
    if (vehicle->vehicleLinkManager()->primaryLinkName() != target) {
        state->policy.cancelPendingSwitch();
        return;
    }
    QString label = _roles->nameForSysid(vehicle->id());
    if (label.isEmpty()) {
        label = _roles->roleForSysid(vehicle->id());
    }
    if (label.isEmpty()) {
        label = tr("Vehicle %1").arg(vehicle->id());
    }
    qCDebug(LinkPreferenceLog) << "Preferred link selected for" << label << target;
    emit QGCCorePlugin::instance() -> operatorNotification(tr("%1: communication back on %2").arg(label, target));
}

#ifdef QGC_UNITTEST_BUILD
void LinkPreferenceController::setTimingForTesting(const LinkPreferencePolicy::Timing& timing, qint64 now)
{
    _timing = timing;
    _testNow = now;
    for (auto* state : _states) {
        state->policy = LinkPreferencePolicy(timing);
    }
    _refresh();
}

void LinkPreferenceController::evaluateForTesting(qint64 now)
{
    _testNow = now;
    const auto states = _states.values();
    for (auto* state : states) {
        _evaluate(state);
    }
}
#endif
