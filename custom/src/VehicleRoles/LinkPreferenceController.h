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

    QString linkReturnStatus(quint32 sysid) const { return _statusTexts.value(sysid); }

#ifdef QGC_UNITTEST_BUILD
    friend class LinkPreferenceControllerTest;

    void setTimingForTesting(const LinkPreferencePolicy::Timing& timing, qint64 now);
    void evaluateForTesting(qint64 now);
#endif

signals:
    void linkReturnStatusChanged();

private:
    struct State;
    void _vehicleAdded(Vehicle* vehicle);
    void _vehicleRemoved(Vehicle* vehicle);
    void _refresh();
    void _observe(State* state);
    void _evaluate(State* state);
    void _updateStatuses();
    QString _statusText(const LinkPreferencePolicy::Input& input, const LinkPreferencePolicy::Status& status) const;
    static const char* _gateName(LinkPreferencePolicy::Gate gate);
    LinkPreferencePolicy::Input _input(Vehicle* vehicle,
                                       LinkPreferencePolicy::TransferFlags* transferFlags = nullptr) const;
    qint64 _now() const;

    QPointer<VehicleRoleController> _roles;
    QHash<Vehicle*, State*> _states;
    QHash<quint32, QString> _statusTexts;
    QTimer _timer;
    QElapsedTimer _clock;
    LinkPreferencePolicy::Timing _timing;
#ifdef QGC_UNITTEST_BUILD
    qint64 _testNow = -1;
#endif
};
