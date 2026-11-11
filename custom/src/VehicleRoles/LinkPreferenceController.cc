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
#include "VehicleSigningController.h"

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
    LinkPreferencePolicy::Gate lastGate = LinkPreferencePolicy::Gate::Off;
    bool gateLogged = false;
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
    connect(links, &VehicleLinkManager::primaryLinkChanged, state, changed);
    connect(links, &VehicleLinkManager::communicationLostChanged, state, changed);
    connect(vehicle, &Vehicle::initialConnectComplete, state, changed);
    if (auto* parameters = vehicle->parameterManager()) {
        connect(parameters, &ParameterManager::parametersReadyChanged, state, changed);
        connect(parameters, &ParameterManager::pendingWritesChanged, state, changed);
        connect(parameters, &ParameterManager::loadProgressChanged, state, changed);
    }
    if (auto* signing = vehicle->signingController()) {
        // The signing facade can signal inside primaryLinkChanged before our primary observer runs.
        connect(signing, &VehicleSigningController::signingStatusChanged, state, changed, Qt::QueuedConnection);
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

LinkPreferencePolicy::Input LinkPreferenceController::_input(Vehicle* vehicle,
                                                             LinkPreferencePolicy::TransferFlags* transferFlags) const
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
    auto* signing = vehicle->signingController();
    // Public progress covers visible refreshes, but is not an aggregate parameter-read busy flag.
    const LinkPreferencePolicy::TransferFlags flags{.parametersReady = parameters->parametersReady(),
                                                    .pendingWrites = parameters->pendingWrites(),
                                                    .loadProgressActive = parameters->loadProgress() > 0,
                                                    .missionBusy = mission->inProgress(),
                                                    .fenceBusy = fence->inProgress(),
                                                    .rallyBusy = rally->inProgress(),
                                                    .signingPending = signing && signing->signingStatus().pending()};
    if (transferFlags) {
        *transferFlags = flags;
    }
    return {.preferredName = _roles->preferredLinkConfigNameForSysid(vehicle->id()),
            .linkNames = links->linkNames(),
            .linkStatuses = links->linkStatuses(),
            .primaryName = links->primaryLinkName(),
            .initialConnectDone = vehicle->isInitialConnectComplete(),
            .transferActive = LinkPreferencePolicy::transferActive(flags)};
}

void LinkPreferenceController::_observe(State* state)
{
    if (!state || !state->vehicle) {
        return;
    }
    state->policy.observe(_input(state->vehicle), _now());
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
        qint64 baseHoldMs = _roles ? _roles->failbackHoldSeconds() * 1000LL : 60000;
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
    _updateStatuses();
}

QString LinkPreferenceController::_statusText(const LinkPreferencePolicy::Input& input,
                                              const LinkPreferencePolicy::Status& status) const
{
    switch (status.gate) {
        case LinkPreferencePolicy::Gate::Off:
            return tr("Automatic return is off. Tick Preferred on one link to turn it on.");
        case LinkPreferencePolicy::Gate::NotAttached:
            return tr("%1 is not connected to this vehicle right now.").arg(input.preferredName);
        case LinkPreferencePolicy::Gate::OnPreferred:
            return tr("Using %1, the preferred link.").arg(input.preferredName);
        case LinkPreferencePolicy::Gate::PreferredDown:
            if (input.primaryName.isEmpty()) {
                return tr("%1 has no communication.").arg(input.preferredName);
            }
            return tr("%1 has no communication. Staying on %2.").arg(input.preferredName, input.primaryName);
        case LinkPreferencePolicy::Gate::Waiting:
            if (status.extendedHold) {
                return tr("Returning to %1 in %2 s if it stays steady (longer than usual because it "
                          "dropped again soon after the last return).")
                    .arg(input.preferredName)
                    .arg(qMax(qint64(1), (status.remainingMs + 999) / 1000));
            }
            return tr("Returning to %1 in %2 s if it stays steady.")
                .arg(input.preferredName)
                .arg(qMax(qint64(1), (status.remainingMs + 999) / 1000));
        case LinkPreferencePolicy::Gate::WaitingForConnect:
            return tr("Return paused until the vehicle finishes connecting.");
        case LinkPreferencePolicy::Gate::PausedForTransfer:
            return tr("Return paused while a parameter, mission, fence, rally or signing transfer is running.");
        case LinkPreferencePolicy::Gate::Ready:
        case LinkPreferencePolicy::Gate::Switching:
            return tr("Returning to %1...").arg(input.preferredName);
    }
    return {};
}

const char* LinkPreferenceController::_gateName(LinkPreferencePolicy::Gate gate)
{
    switch (gate) {
        case LinkPreferencePolicy::Gate::Off:
            return "Off";
        case LinkPreferencePolicy::Gate::NotAttached:
            return "NotAttached";
        case LinkPreferencePolicy::Gate::OnPreferred:
            return "OnPreferred";
        case LinkPreferencePolicy::Gate::PreferredDown:
            return "PreferredDown";
        case LinkPreferencePolicy::Gate::Waiting:
            return "Waiting";
        case LinkPreferencePolicy::Gate::WaitingForConnect:
            return "WaitingForConnect";
        case LinkPreferencePolicy::Gate::PausedForTransfer:
            return "PausedForTransfer";
        case LinkPreferencePolicy::Gate::Ready:
            return "Ready";
        case LinkPreferencePolicy::Gate::Switching:
            return "Switching";
    }
    return "Unknown";
}

void LinkPreferenceController::_updateStatuses()
{
    QHash<quint32, QString> texts;
    const qint64 now = _now();
    for (auto* state : _states) {
        if (state->vehicle) {
            LinkPreferencePolicy::TransferFlags flags;
            const auto input = _input(state->vehicle, &flags);
            const auto status = state->policy.status(input, now);
            if (!state->gateLogged || state->lastGate != status.gate) {
                state->gateLogged = true;
                state->lastGate = status.gate;
                qCDebug(LinkPreferenceLog)
                    << "sysid:" << state->vehicle->id() << "gate:" << _gateName(status.gate)
                    << "preferred:" << input.preferredName << "primary:" << input.primaryName
                    << "linkNames:" << input.linkNames << "linkStatuses:" << input.linkStatuses
                    << "initialConnectDone:" << input.initialConnectDone << "parametersReady:" << flags.parametersReady
                    << "pendingWrites:" << flags.pendingWrites << "loadProgressActive:" << flags.loadProgressActive
                    << "missionBusy:" << flags.missionBusy << "fenceBusy:" << flags.fenceBusy
                    << "rallyBusy:" << flags.rallyBusy << "signingPending:" << flags.signingPending
                    << state->policy.debugState() << "remainingMs:" << status.remainingMs;
            }
            texts.insert(state->vehicle->id(), _statusText(input, status));
        }
    }
    if (_statusTexts != texts) {
        _statusTexts = texts;
        emit linkReturnStatusChanged();
    }
}

void LinkPreferenceController::_evaluate(State* state)
{
    if (!state || !state->vehicle || !_roles || QCoreApplication::closingDown()) {
        return;
    }
    QPointer<State> guard(state);
    QPointer<Vehicle> vehicle = state->vehicle;
    const QString target = state->policy.evaluate(_input(vehicle), _now());
    _updateStatuses();
    if (!guard || !vehicle || !_roles || target.isEmpty()) {
        return;
    }
    const auto current = _input(vehicle);
    if (current.preferredName != target ||
        state->policy.status(current, _now()).gate != LinkPreferencePolicy::Gate::Switching) {
        state->policy.cancelPendingSwitch();
        _updateStatuses();
        return;
    }
    vehicle->vehicleLinkManager()->setPrimaryLinkByName(target);
    if (!guard || !vehicle || !_roles) {
        return;
    }
    if (vehicle->vehicleLinkManager()->primaryLinkName() != target) {
        state->policy.cancelPendingSwitch();
        _updateStatuses();
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
