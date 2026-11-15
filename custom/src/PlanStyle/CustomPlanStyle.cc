#include "CustomPlanStyle.h"

#include "COPController.h"

CustomPlanStyle::CustomPlanStyle(QObject* parent)
    : QGCPlanStyle(parent)
{}

QColor CustomPlanStyle::keepOutFillColor() const
{
    QColor fill(QStringLiteral("#E53935"));
    fill.setAlphaF(0.35f);
    return fill;
}

QColor CustomPlanStyle::vehicleColor(quint32 sysid) const
{
    return _controller ? _controller->colorForSysid(sysid) : QColor();
}

void CustomPlanStyle::attachRegistry(COPController* controller)
{
    // QPointer is already cleared when destroyed() runs, but the old connections still need releasing.
    if (_controller == controller && (controller || _registryConnections.isEmpty())) {
        return;
    }
    for (const auto& connection : _registryConnections) {
        disconnect(connection);
    }
    _registryConnections.clear();
    _controller = controller;
    if (_controller) {
        _registryConnections.append(connect(_controller, &COPController::entriesChanged, this, [this]() {
            _watchEntries();
            _advanceRevision();
        }));
        _registryConnections.append(
            connect(_controller, &QObject::destroyed, this, [this]() { attachRegistry(nullptr); }));
    }
    _watchEntries();
    _advanceRevision();
}

void CustomPlanStyle::_watchEntries()
{
    for (const auto& connection : _entryConnections) {
        disconnect(connection);
    }
    _entryConnections.clear();
    if (!_controller) {
        return;
    }
    auto* entries = _controller->vehicles();
    if (!entries) {
        return;
    }
    for (int i = 0; i < entries->count(); ++i) {
        auto* entry = entries->value<COPVehicle*>(i);
        if (entry) {
            _entryConnections.append(
                connect(entry, &COPVehicle::colorChanged, this, &CustomPlanStyle::_advanceRevision));
        }
    }
}
