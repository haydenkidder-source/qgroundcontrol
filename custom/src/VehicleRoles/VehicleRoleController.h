#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtQmlIntegration/QtQmlIntegration>

class QmlObjectListModel;

/// \brief A single sysid -> role/nickname assignment.
///
/// Mutated only through VehicleRoleController (mirrors StagedExclusionZone's
/// controller-owns-mutation style) so every change goes through one place that also persists it.
class VehicleRoleEntry : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by VehicleRoleController")

    Q_PROPERTY(int sysid READ sysid CONSTANT)
    Q_PROPERTY(QString role READ role NOTIFY roleChanged)
    Q_PROPERTY(QString name READ name NOTIFY nameChanged)
    Q_PROPERTY(int port READ port NOTIFY portChanged)

public:
    VehicleRoleEntry(int sysid, const QString& role, const QString& name, int port, QObject* parent = nullptr);

    int sysid() const { return _sysid; }

    QString role() const { return _role; }

    QString name() const { return _name; }

    /// The vehicle's assigned ground-station UDP port, for reference only (see
    /// custom/FIELD_RADIO_SETUP.md) - 0 means unassigned. Setting this does not create, modify or
    /// look up any actual comm link; the operator still configures the real UDP link separately
    /// under Settings > Comm Links.
    int port() const { return _port; }

    void setRole(const QString& role);
    void setName(const QString& name);
    void setPort(int port);

signals:
    void roleChanged(QString role);
    void nameChanged(QString name);
    void portChanged(int port);

private:
    int _sysid = 0;
    QString _role;
    QString _name;
    int _port = 0;
};

/// \brief Remembers which MAVLink system ID (sysid) corresponds to which of the program's
/// permanent vehicles, identified by ArduPilot vehicle type (role) plus an optional operator
/// nickname (name).
///
/// QGC already reads each vehicle's sysid from its heartbeat (Vehicle::id(), set on the vehicle
/// side by the ArduPilot SYSID_THISMAV parameter, the same value Mission Planner uses to
/// deconflict autopilots on a shared link) but has no persisted concept of which sysid means
/// which vehicle, and no way to show an operator-facing name anywhere. This is presentation-layer
/// bookkeeping only - it does not gate any safety-relevant behavior (rover takeover continues to
/// use Vehicle::rover()/MAV_TYPE, unchanged).
class VehicleRoleController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_MOC_INCLUDE("QmlObjectListModel.h")

    Q_PROPERTY(QmlObjectListModel* roleEntries READ roleEntries CONSTANT)
    Q_PROPERTY(QStringList availableRoles READ availableRoles CONSTANT)

public:
    explicit VehicleRoleController(QObject* parent = nullptr);

    QmlObjectListModel* roleEntries() const { return _roleEntries; }

    /// The fixed set of ArduPilot vehicle types entries can be grouped under. Not user-extensible:
    /// this is ArduPilot's own set of distinct vehicle firmwares/products, independent of which
    /// specific vehicles this program happens to fly.
    QStringList availableRoles() const
    {
        return {QStringLiteral("Copter"), QStringLiteral("Plane"),   QStringLiteral("Rover"),
                QStringLiteral("Sub"),    QStringLiteral("Tracker"), QStringLiteral("Blimp")};
    }

    /// Adds a new sysid/role/name/port assignment, or updates the existing entry for that sysid if
    /// one is already present. Ignored if sysid is outside the valid MAVLink range [1,255], role
    /// isn't one of availableRoles(), or port is outside [0,65535] (0 means unassigned).
    Q_INVOKABLE void addEntry(int sysid, const QString& role, const QString& name, int port);

    Q_INVOKABLE void removeEntry(int index);
    Q_INVOKABLE void setRole(int index, const QString& role);
    Q_INVOKABLE void setName(int index, const QString& name);
    Q_INVOKABLE void setPort(int index, int port);

    /// One-shot convenience: creates a real, saved, autoconnect-enabled UDP link configuration
    /// (named after the entry's role) using the entry's assigned port, and connects it now if
    /// possible. This does not require or check a port; the caller (QML) is expected to only
    /// offer this action when one is assigned. The created link is an ordinary link afterward -
    /// nothing here keeps it in sync with this entry if either is edited later. No-op if the
    /// entry has no port assigned.
    Q_INVOKABLE void createLinkForEntry(int index);

    /// Returns the assigned name for sysid, or an empty string if unassigned. Callers should fall
    /// back to a generic label.
    Q_INVOKABLE QString nameForSysid(int sysid) const;
    /// Returns the assigned role for sysid, or an empty string if unassigned.
    Q_INVOKABLE QString roleForSysid(int sysid) const;

    /// Returns the assigned port for sysid, or 0 (unassigned) if sysid has no entry.
    Q_INVOKABLE int portForSysid(int sysid) const;

private:
    void _load();
    void _save();
    int _indexForSysid(int sysid) const;

    QmlObjectListModel* _roleEntries = nullptr;
};
