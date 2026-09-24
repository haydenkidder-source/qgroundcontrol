#include "VehicleRoleController.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include "AppSettings.h"
#include "LinkManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "UDPLink.h"

QGC_LOGGING_CATEGORY(VehicleRoleLog, "Custom.VehicleRoles")

namespace {
const char* kRolesFileName = "VehicleRoles.json";
}

VehicleRoleEntry::VehicleRoleEntry(int sysid, const QString& role, const QString& name, int port, QObject* parent)
    : QObject(parent)
    , _sysid(sysid)
    , _role(role)
    , _name(name)
    , _port(port)
{}

void VehicleRoleEntry::setRole(const QString& role)
{
    if (_role != role) {
        _role = role;
        emit roleChanged(_role);
    }
}

void VehicleRoleEntry::setName(const QString& name)
{
    if (_name != name) {
        _name = name;
        emit nameChanged(_name);
    }
}

void VehicleRoleEntry::setPort(int port)
{
    if (_port != port) {
        _port = port;
        emit portChanged(_port);
    }
}

VehicleRoleController::VehicleRoleController(QObject* parent)
    : QObject(parent)
    , _roleEntries(new QmlObjectListModel(this))
{
    _load();
}

int VehicleRoleController::_indexForSysid(int sysid) const
{
    for (int i = 0; i < _roleEntries->count(); i++) {
        if (qobject_cast<VehicleRoleEntry*>(_roleEntries->get(i))->sysid() == sysid) {
            return i;
        }
    }
    return -1;
}

void VehicleRoleController::addEntry(int sysid, const QString& role, const QString& name, int port)
{
    if (sysid < 1 || sysid > 255 || !availableRoles().contains(role) || port < 0 || port > 65535) {
        qCWarning(VehicleRoleLog) << "Ignoring invalid entry - sysid:" << sysid << "role:" << role << "port:" << port;
        return;
    }

    const int existingIndex = _indexForSysid(sysid);
    if (existingIndex >= 0) {
        setRole(existingIndex, role);
        setName(existingIndex, name);
        setPort(existingIndex, port);
        return;
    }

    _roleEntries->append(new VehicleRoleEntry(sysid, role, name, port, this));
    _save();
}

void VehicleRoleController::removeEntry(int index)
{
    if (auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))) {
        _roleEntries->removeOne(entry);
        entry->deleteLater();
        _save();
    }
}

void VehicleRoleController::setRole(int index, const QString& role)
{
    if (!availableRoles().contains(role)) {
        qCWarning(VehicleRoleLog) << "Ignoring invalid role:" << role;
        return;
    }
    if (auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))) {
        entry->setRole(role);
        _save();
    }
}

void VehicleRoleController::setName(int index, const QString& name)
{
    if (auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))) {
        entry->setName(name);
        _save();
    }
}

void VehicleRoleController::setPort(int index, int port)
{
    if (port < 0 || port > 65535) {
        qCWarning(VehicleRoleLog) << "Ignoring invalid port:" << port;
        return;
    }
    if (auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))) {
        entry->setPort(port);
        _save();
    }
}

void VehicleRoleController::createLinkForEntry(int index)
{
    auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index));
    if (!entry || entry->port() <= 0) {
        qCWarning(VehicleRoleLog) << "Cannot create a link - no port assigned for index:" << index;
        return;
    }

    auto* const udpConfig = new UDPConfiguration(entry->role());
    // Set autoConnect before the port: UDPConfiguration::setAutoConnect() can overwrite localPort
    // with the global default as a side effect when the flag changes from false to true. Setting
    // the desired port afterward guarantees the final value regardless of that behavior.
    udpConfig->setAutoConnect(true);
    udpConfig->setLocalPort(static_cast<quint16>(entry->port()));

    LinkManager* const linkManager = LinkManager::instance();
    SharedLinkConfigurationPtr config = linkManager->addConfiguration(udpConfig);
    linkManager->saveLinkConfigurationList();
    linkManager->createConnectedLink(config);
}

QString VehicleRoleController::nameForSysid(int sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->name() : QString();
}

QString VehicleRoleController::roleForSysid(int sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->role() : QString();
}

int VehicleRoleController::portForSysid(int sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->port() : 0;
}

void VehicleRoleController::_load()
{
    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/") + kRolesFileName;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isArray()) {
        qCWarning(VehicleRoleLog) << "Ignoring malformed roles file:" << filePath;
        return;
    }

    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        const int sysid = obj.value(QStringLiteral("sysid")).toInt();
        const QString role = obj.value(QStringLiteral("role")).toString();
        if (sysid < 1 || sysid > 255 || !availableRoles().contains(role)) {
            qCWarning(VehicleRoleLog) << "Skipping invalid saved entry - sysid:" << sysid << "role:" << role;
            continue;
        }
        // Missing "port" (files saved before this field existed) reads as 0 (unassigned), same as
        // an out-of-range value - treat both as unassigned rather than discarding the whole entry.
        const int savedPort = obj.value(QStringLiteral("port")).toInt();
        const int port = (savedPort >= 0 && savedPort <= 65535) ? savedPort : 0;
        _roleEntries->append(
            new VehicleRoleEntry(sysid, role, obj.value(QStringLiteral("name")).toString(), port, this));
    }
}

void VehicleRoleController::_save()
{
    QJsonArray array;
    for (int i = 0; i < _roleEntries->count(); i++) {
        auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(i));
        QJsonObject obj;
        obj[QStringLiteral("sysid")] = entry->sysid();
        obj[QStringLiteral("role")] = entry->role();
        obj[QStringLiteral("name")] = entry->name();
        obj[QStringLiteral("port")] = entry->port();
        array.append(obj);
    }

    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/") + kRolesFileName;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(VehicleRoleLog) << "Failed to open roles file for write:" << filePath;
        return;
    }
    file.write(QJsonDocument(array).toJson());
}
