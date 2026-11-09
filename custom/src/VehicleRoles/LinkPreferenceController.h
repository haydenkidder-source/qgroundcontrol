#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include "LinkPreferencePolicy.h"

class Vehicle;
class VehicleRoleController;

/// Adds preference and failback to stock failover without owning links or vehicles.
class LinkPreferenceController : public QObject
{
    Q_OBJECT

public:
    explicit LinkPreferenceController(VehicleRoleController* roles);

#ifdef QGC_UNITTEST_BUILD
    void setTimingForTesting(const LinkPreferencePolicy::Timing& timing, qint64 now);
    void evaluateForTesting(qint64 now);
#endif

private:
    struct State;
    void _vehicleAdded(Vehicle* vehicle);
    void _vehicleRemoved(Vehicle* vehicle);
    void _refresh();
    void _observe(State* state, bool primarySignal = false);
    void _evaluate(State* state);
    LinkPreferencePolicy::Input _input(Vehicle* vehicle) const;
    qint64 _now() const;

    QPointer<VehicleRoleController> _roles;
    QHash<Vehicle*, State*> _states;
    QTimer _timer;
    QElapsedTimer _clock;
    LinkPreferencePolicy::Timing _timing;
#ifdef QGC_UNITTEST_BUILD
    qint64 _testNow = -1;
#endif
};
