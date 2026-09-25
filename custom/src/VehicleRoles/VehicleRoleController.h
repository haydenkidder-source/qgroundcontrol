#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtQmlIntegration/QtQmlIntegration>

class QmlObjectListModel;

/// \brief A single named link association within a VehicleRoleEntry (e.g. "RFD900x" pointing at
/// the LinkConfiguration named "Rover RFD900x").
///
/// Association is by LinkConfiguration::name() rather than by holding a LinkConfiguration
/// pointer directly: configurations are recreated during editing (see
/// LinkManager::endConfigurationEditing()), and this needs to survive that as well as round-trip
/// through JSON. linkConfigName() is empty until the operator attaches (or creates) a real link
/// configuration for this association.
///
/// Mutated only through VehicleRoleController (same controller-owns-mutation style as
/// VehicleRoleEntry) so every change goes through one place that also persists it.
class VehicleRoleLinkEntry : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by VehicleRoleController")

    Q_PROPERTY(QString label READ label NOTIFY labelChanged)
    Q_PROPERTY(QString linkConfigName READ linkConfigName NOTIFY linkConfigNameChanged)

public:
    VehicleRoleLinkEntry(const QString& label, const QString& linkConfigName, QObject* parent = nullptr);

    QString label() const { return _label; }

    QString linkConfigName() const { return _linkConfigName; }

    void setLabel(const QString& label);
    void setLinkConfigName(const QString& linkConfigName);

signals:
    void labelChanged(QString label);
    void linkConfigNameChanged(QString linkConfigName);

private:
    QString _label;
    QString _linkConfigName;
};

/// \brief A single sysid -> role/nickname assignment, plus its named link associations.
///
/// Mutated only through VehicleRoleController (mirrors StagedExclusionZone's
/// controller-owns-mutation style) so every change goes through one place that also persists it.
class VehicleRoleEntry : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by VehicleRoleController")
    Q_MOC_INCLUDE("QmlObjectListModel.h")

    Q_PROPERTY(int sysid READ sysid CONSTANT)
    Q_PROPERTY(QString role READ role NOTIFY roleChanged)
    Q_PROPERTY(QString name READ name NOTIFY nameChanged)
    Q_PROPERTY(int port READ port NOTIFY portChanged)
    Q_PROPERTY(QmlObjectListModel* links READ links CONSTANT)

public:
    VehicleRoleEntry(int sysid, const QString& role, const QString& name, int port, QObject* parent = nullptr);

    int sysid() const { return _sysid; }

    QString role() const { return _role; }

    QString name() const { return _name; }

    /// The vehicle's assigned ground-station UDP port, for reference only (see
    /// custom/FIELD_RADIO_SETUP.md) - 0 means unassigned. Setting this does not create, modify or
    /// look up any actual comm link; the operator still configures the real UDP link separately
    /// under Settings > Comm Links. Superseded by links() for new work (see VehicleRoleLinkEntry)
    /// but kept for existing callers that still read/write it.
    int port() const { return _port; }

    /// This entry's named link associations (e.g. "RFD900x", "Microhard 2450", "WFB-NG"), each
    /// optionally pointing at a real LinkConfiguration. See VehicleRoleLinkEntry.
    QmlObjectListModel* links() const { return _links; }

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
    QmlObjectListModel* _links = nullptr;
};

/// \brief Remembers which MAVLink system ID (sysid) corresponds to which of the program's
/// permanent vehicles (Rover, Stallion, Hex), plus an optional operator nickname.
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

    /// The program's fixed set of vehicle roles. Not user-extensible: the program has exactly
    /// three permanent vehicles.
    QStringList availableRoles() const
    {
        return {QStringLiteral("Rover"), QStringLiteral("Stallion"), QStringLiteral("Hex")};
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

    /// Returns the link associations for sysid, or nullptr if sysid has no entry.
    Q_INVOKABLE QmlObjectListModel* linksForSysid(int sysid) const;

    /// Adds a new named link association (e.g. "RFD900x") to the entry at index, optionally
    /// already pointing at an existing LinkConfiguration by name - pass an empty string to leave
    /// it unattached for now, the operator can attach or create one later with
    /// setLinkConfigName(). Ignored (returns -1) if index is out of range or label is empty.
    /// Returns the new link's index within that entry's links list.
    Q_INVOKABLE int addLink(int index, const QString& label, const QString& linkConfigName);

    Q_INVOKABLE void removeLink(int index, int linkIndex);
    Q_INVOKABLE void setLinkLabel(int index, int linkIndex, const QString& label);
    Q_INVOKABLE void setLinkConfigName(int index, int linkIndex, const QString& linkConfigName);

    /// True if a live LinkInterface exists right now for a LinkConfiguration with this name - i.e.
    /// the link is actually connected, not merely configured or pending auto-reconnect. An empty
    /// linkConfigName (an association not yet pointed at a real link) is never connected.
    Q_INVOKABLE bool isLinkConfigConnected(const QString& linkConfigName) const;

private:
    void _load();
    void _save();
    int _indexForSysid(int sysid) const;
    VehicleRoleLinkEntry* _linkAt(int index, int linkIndex) const;

    QmlObjectListModel* _roleEntries = nullptr;
};
