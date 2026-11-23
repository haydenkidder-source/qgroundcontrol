#include "VehicleRoleController.h"

#include <cmath>
#include <limits>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QUuid>

#include "AppSettings.h"
#include "LinkManager.h"
#include "LinkPreferenceController.h"
#include "QGCCorePlugin.h"
#include "QGCLoggingCategory.h"
#include "QGCMAVLink.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"

QGC_LOGGING_CATEGORY(VehicleRoleLog, "Custom.VehicleRoles")

namespace {
const char* kRolesFileName = "VehicleRoles.json";
const char* kFailbackHoldKey = "Custom/LinkPreference/FailbackHoldSeconds";
}  // namespace

VehicleRoleLinkEntry::VehicleRoleLinkEntry(const QString& label, const QString& linkConfigName, QObject* parent)
    : QObject(parent)
    , _label(label)
    , _linkConfigName(linkConfigName)
{}

void VehicleRoleLinkEntry::setPreferred(bool preferred)
{
    if (_preferred != preferred) {
        _preferred = preferred;
        emit preferredChanged(_preferred);
    }
}

void VehicleRoleLinkEntry::setLabel(const QString& label)
{
    if (_label != label) {
        _label = label;
        emit labelChanged(_label);
    }
}

void VehicleRoleLinkEntry::setLinkConfigName(const QString& linkConfigName)
{
    if (_linkConfigName != linkConfigName) {
        _linkConfigName = linkConfigName;
        emit linkConfigNameChanged(_linkConfigName);
    }
}

VehicleRoleEntry::VehicleRoleEntry(quint32 sysid, const QString& role, const QString& name, int port, QObject* parent)
    : QObject(parent)
    , _sysid(sysid)
    , _role(role)
    , _name(name)
    , _port(port)
    , _links(new QmlObjectListModel(this))
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
    _failbackHoldSeconds = qBound(5, QSettings().value(kFailbackHoldKey, 60).toInt(), 300);
    _load();
    _linkPreference = new LinkPreferenceController(this);
    connect(_linkPreference, &LinkPreferenceController::linkReturnStatusChanged, this, [this]() {
        ++_linkReturnStatusRevision;
        emit linkReturnStatusChanged();
    });
}

QString VehicleRoleController::linkReturnStatus(quint32 sysid) const
{
    return _linkPreference ? _linkPreference->linkReturnStatus(sysid) : QString();
}

int VehicleRoleController::_indexForSysid(quint32 sysid) const
{
    for (int i = 0; i < _roleEntries->count(); i++) {
        if (qobject_cast<VehicleRoleEntry*>(_roleEntries->get(i))->sysid() == sysid) {
            return i;
        }
    }
    return -1;
}

VehicleRoleLinkEntry* VehicleRoleController::_linkAt(int index, int linkIndex) const
{
    auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index));
    return entry ? qobject_cast<VehicleRoleLinkEntry*>(entry->links()->get(linkIndex)) : nullptr;
}

void VehicleRoleController::addEntry(quint32 sysid, const QString& role, const QString& name, int port)
{
    if (sysid == 0 || !availableRoles().contains(role) || port < 0 || port > 65535) {
        qCWarning(VehicleRoleLog) << "Ignoring invalid entry - sysid:" << sysid << "role:" << role << "port:" << port;
        return;
    }

    if (isNicknameTaken(name, sysid)) {
        qCWarning(VehicleRoleLog) << "Ignoring entry - nickname already used by another vehicle - sysid:" << sysid
                                  << "name:" << name;
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
        for (int i = 0; i < entry->links()->count(); ++i) {
            _rawObjects.remove(entry->links()->get(i));
        }
        _rawObjects.remove(entry);
        _extraLinks.remove(entry);
        _roleEntries->removeOne(entry);
        entry->deleteLater();
        _save();
        emit linkPreferencesChanged();
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
        if (isNicknameTaken(name, entry->sysid())) {
            qCWarning(VehicleRoleLog) << "Ignoring nickname already used by another vehicle - sysid:" << entry->sysid()
                                      << "name:" << name;
            return;
        }
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

QString VehicleRoleController::roleForMavType(int mavType)
{
    const auto type = static_cast<MAV_TYPE>(mavType);
    if (QGCMAVLink::isRoverBoat(type)) {
        return QStringLiteral("Rover");
    }
    if (QGCMAVLink::isSub(type)) {
        return QStringLiteral("Sub");
    }
    if (QGCMAVLink::isMultiRotor(type)) {
        return QStringLiteral("Copter");
    }
    if (QGCMAVLink::isFixedWing(type) || QGCMAVLink::isVTOL(type)) {
        return QStringLiteral("Plane");
    }
    if (type == MAV_TYPE_ANTENNA_TRACKER) {
        return QStringLiteral("Tracker");
    }
    if (QGCMAVLink::vehicleClass(type) == QGCMAVLink::VehicleClassAirship) {
        return QStringLiteral("Blimp");
    }
    return {};
}

QString VehicleRoleController::suggestedRole(Vehicle* vehicle) const
{
    return vehicle ? roleForMavType(vehicle->vehicleType()) : QString();
}

bool VehicleRoleController::isNicknameTaken(const QString& name, quint32 exceptSysid) const
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty()) {
        return false;
    }
    for (int i = 0; i < _roleEntries->count(); i++) {
        const auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(i));
        if (entry && entry->sysid() != exceptSysid &&
            entry->name().trimmed().compare(wanted, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString VehicleRoleController::nameForSysid(quint32 sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->name() : QString();
}

QString VehicleRoleController::roleForSysid(quint32 sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->role() : QString();
}

int VehicleRoleController::portForSysid(quint32 sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->port() : 0;
}

QmlObjectListModel* VehicleRoleController::linksForSysid(quint32 sysid) const
{
    const int index = _indexForSysid(sysid);
    return index >= 0 ? qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index))->links() : nullptr;
}

int VehicleRoleController::addLink(int index, const QString& label, const QString& linkConfigName)
{
    auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index));
    if (!entry || label.isEmpty()) {
        qCWarning(VehicleRoleLog) << "Ignoring invalid link - index:" << index << "label:" << label;
        return -1;
    }

    entry->links()->append(new VehicleRoleLinkEntry(label, linkConfigName, entry));
    _save();
    return entry->links()->count() - 1;
}

void VehicleRoleController::removeLink(int index, int linkIndex)
{
    auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index));
    if (!entry) {
        return;
    }
    if (auto* link = qobject_cast<VehicleRoleLinkEntry*>(entry->links()->removeAt(linkIndex))) {
        _rawObjects.remove(link);
        link->deleteLater();
        _save();
        emit linkPreferencesChanged();
    }
}

void VehicleRoleController::setLinkLabel(int index, int linkIndex, const QString& label)
{
    if (label.isEmpty()) {
        qCWarning(VehicleRoleLog) << "Ignoring empty link label";
        return;
    }
    if (auto* link = _linkAt(index, linkIndex)) {
        link->setLabel(label);
        _save();
    }
}

void VehicleRoleController::setLinkConfigName(int index, int linkIndex, const QString& linkConfigName)
{
    if (auto* link = _linkAt(index, linkIndex)) {
        if (link->linkConfigName() != linkConfigName) {
            link->setPreferred(false);
            link->setLinkConfigName(linkConfigName);
            _save();
            emit linkPreferencesChanged();
        }
    }
}

void VehicleRoleController::setFailbackHoldSeconds(int seconds)
{
    seconds = qBound(5, seconds, 300);
    if (_failbackHoldSeconds != seconds) {
        _failbackHoldSeconds = seconds;
        QSettings().setValue(kFailbackHoldKey, seconds);
        emit failbackHoldSecondsChanged();
    }
}

void VehicleRoleController::setLinkPreferred(int index, int linkIndex, bool preferred)
{
    auto* link = _linkAt(index, linkIndex);
    if (!link || (preferred && link->linkConfigName().isEmpty())) {
        return;
    }
    auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(index));
    for (int i = 0; i < entry->links()->count(); ++i) {
        auto* other = qobject_cast<VehicleRoleLinkEntry*>(entry->links()->get(i));
        if (other != link && preferred) {
            other->setPreferred(false);
        }
    }
    link->setPreferred(preferred);
    _save();
    emit linkPreferencesChanged();
}

QString VehicleRoleController::preferredLinkConfigNameForSysid(quint32 sysid) const
{
    auto* links = linksForSysid(sysid);
    if (links) {
        for (int i = 0; i < links->count(); ++i) {
            const auto* link = qobject_cast<VehicleRoleLinkEntry*>(links->get(i));
            if (link && link->preferred()) {
                return link->linkConfigName();
            }
        }
    }
    return {};
}

bool VehicleRoleController::isLinkConfigConnected(const QString& linkConfigName) const
{
    if (linkConfigName.isEmpty()) {
        return false;
    }

    const QList<SharedLinkInterfacePtr> links = LinkManager::instance()->links();
    for (const SharedLinkInterfacePtr& link : links) {
        if (link && link->linkConfiguration() && link->linkConfiguration()->name() == linkConfigName) {
            return true;
        }
    }
    return false;
}

bool VehicleRoleController::_writeBytes(const QString& path, const QByteArray& bytes, QString* error)
{
#ifdef QGC_UNITTEST_BUILD
    if (_writeForTesting) {
        return _writeForTesting(path, bytes, error);
    }
#endif
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

bool VehicleRoleController::_readBytes(const QString& path, QByteArray* bytes, QString* error)
{
#ifdef QGC_UNITTEST_BUILD
    if (_readForTesting) {
        return _readForTesting(path, bytes, error);
    }
#endif
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    *bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *error = file.errorString();
        return false;
    }
    return true;
}

bool VehicleRoleController::_renameFile(const QString& from, const QString& to)
{
#ifdef QGC_UNITTEST_BUILD
    if (_renameForTesting) {
        return _renameForTesting(from, to);
    }
#endif
    return QFile::rename(from, to);
}

void VehicleRoleController::_setPersistenceError(const QString& error)
{
    if (_persistenceError != error) {
        _persistenceError = error;
        emit persistenceErrorChanged();
    }
}

void VehicleRoleController::_load()
{
    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/") + kRolesFileName;
    if (!QFileInfo::exists(filePath)) {
        return;
    }
    QByteArray bytes;
    QString error;
    if (!_readBytes(filePath, &bytes, &error)) {
        _persistenceReadOnly = true;
        _setPersistenceError(
            tr("Unable to read vehicle roles file %1: %2. Changes will not be saved.").arg(filePath, error));
        qCWarning(VehicleRoleLog) << "Failed to read roles file:" << filePath << error;
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (doc.isObject()) {
        qCWarning(VehicleRoleLog) << "Ignoring newer roles file:" << filePath;
        _persistenceReadOnly = true;
        _setPersistenceError(
            tr("Vehicle roles file %1 needs a newer application version. Changes will not be saved.").arg(filePath));
        return;
    }
    if (!doc.isArray()) {
        const QString prefix = filePath + QStringLiteral(".invalid-") +
                               QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmss")) +
                               QStringLiteral("-");
        QString quarantinePath;
        do {
            quarantinePath = prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
        } while (QFileInfo::exists(quarantinePath));
        if (!_renameFile(filePath, quarantinePath)) {
            _persistenceReadOnly = true;
            _setPersistenceError(
                tr("Unable to quarantine vehicle roles file %1. Changes will not be saved.").arg(filePath));
            qCWarning(VehicleRoleLog) << "Failed to quarantine roles file:" << filePath;
            return;
        }
        _persistenceNotice = tr("Invalid vehicle roles file moved to %1.").arg(quarantinePath);
        emit persistenceNoticeChanged();
        emit QGCCorePlugin::instance() -> operatorNotification(_persistenceNotice);
        return;
    }

    int rolePosition = 0;
    for (const QJsonValue& value : doc.array()) {
        const int position = rolePosition++;
        if (!value.isObject()) {
            _extraRoles.insert(position, value);
            continue;
        }
        const QJsonObject obj = value.toObject();
        const QJsonValue savedSysid = obj.value(QStringLiteral("sysid"));
        const double numericSysid = savedSysid.toDouble();
        if (!savedSysid.isDouble() || !std::isfinite(numericSysid) || numericSysid < 1 ||
            numericSysid > std::numeric_limits<quint32>::max() || std::floor(numericSysid) != numericSysid) {
            qCWarning(VehicleRoleLog) << "Skipping invalid saved entry - sysid:" << savedSysid;
            _extraRoles.insert(position, value);
            continue;
        }
        const auto sysid = static_cast<quint32>(numericSysid);
        QString role = obj.value(QStringLiteral("role")).toString();
        QString name = obj.value(QStringLiteral("name")).toString();
        // Older saves used a fixed call-sign ("Rover"/"Stallion"/"Hex") as the role. "Rover" still
        // matches the current ArduPilot-type list; anything else is now unrecognized - preserve it
        // as the nickname (if one wasn't already set) rather than discarding the whole entry, and
        // leave role unassigned so the operator picks the vehicle's actual type.
        if (!availableRoles().contains(role)) {
            if (name.isEmpty() && !role.isEmpty()) {
                name = role;
            }
            role.clear();
        }
        // Missing "port" (files saved before this field existed) reads as 0 (unassigned), same as
        // an out-of-range value - treat both as unassigned rather than discarding the whole entry.
        const int savedPort = obj.value(QStringLiteral("port")).toInt();
        const int port = (savedPort >= 0 && savedPort <= 65535) ? savedPort : 0;
        auto* entry = new VehicleRoleEntry(sysid, role, name, port, this);

        _rawObjects.insert(entry, obj);

        // "links" is absent in files saved before the multi-link model existed; toArray() on a
        // missing/non-array JSON value safely yields an empty array, so entries from those files
        // simply load with no link associations for the operator to re-add via the Vehicle Links
        // page. The legacy "port" above is preserved either way, but it never represented an
        // actual persisted link identity (see VehicleRoleEntry::port()), so nothing is silently
        // lost by not attempting to migrate it into a link association.
        bool hasPreferred = false;
        int linkPosition = 0;
        for (const QJsonValue& linkValue : obj.value(QStringLiteral("links")).toArray()) {
            const int linkIndex = linkPosition++;
            if (!linkValue.isObject()) {
                _extraLinks[entry].insert(linkIndex, linkValue);
                continue;
            }
            const QJsonObject linkObj = linkValue.toObject();
            const QString label = linkObj.value(QStringLiteral("label")).toString();
            if (label.isEmpty()) {
                qCWarning(VehicleRoleLog) << "Skipping saved link with empty label for sysid:" << sysid;
                _extraLinks[entry].insert(linkIndex, linkValue);
                continue;
            }
            auto* link =
                new VehicleRoleLinkEntry(label, linkObj.value(QStringLiteral("linkConfigName")).toString(), entry);
            const bool preferred = linkObj.value(QStringLiteral("preferred")).toBool(false) &&
                                   !link->linkConfigName().isEmpty() && !hasPreferred;
            _rawObjects.insert(link, linkObj);
            link->setPreferred(preferred);
            hasPreferred = hasPreferred || preferred;
            entry->links()->append(link);
        }

        _roleEntries->append(entry);
    }
}

void VehicleRoleController::_save()
{
    _dirty = true;
    if (_persistenceReadOnly) {
        return;
    }
    QJsonArray array;
    auto roleExtra = _extraRoles.cbegin();
    for (int i = 0; i < _roleEntries->count(); i++) {
        auto* entry = qobject_cast<VehicleRoleEntry*>(_roleEntries->get(i));
        while (roleExtra != _extraRoles.cend() && roleExtra.key() <= array.size()) {
            array.append(roleExtra.value());
            ++roleExtra;
        }
        QJsonObject obj = _rawObjects.value(entry);
        obj[QStringLiteral("sysid")] = static_cast<double>(entry->sysid());
        obj[QStringLiteral("role")] = entry->role();
        obj[QStringLiteral("name")] = entry->name();
        obj[QStringLiteral("port")] = entry->port();

        QJsonArray linksArray;
        const auto extras = _extraLinks.value(entry);
        auto linkExtra = extras.cbegin();
        for (int j = 0; j < entry->links()->count(); j++) {
            auto* link = qobject_cast<VehicleRoleLinkEntry*>(entry->links()->get(j));
            while (linkExtra != extras.cend() && linkExtra.key() <= linksArray.size()) {
                linksArray.append(linkExtra.value());
                ++linkExtra;
            }
            QJsonObject linkObj = _rawObjects.value(link);
            linkObj.remove(QStringLiteral("preferred"));
            linkObj[QStringLiteral("label")] = link->label();
            linkObj[QStringLiteral("linkConfigName")] = link->linkConfigName();
            if (link->preferred()) {
                linkObj[QStringLiteral("preferred")] = true;
            }
            linksArray.append(linkObj);
        }
        while (linkExtra != extras.cend()) {
            linksArray.append(linkExtra.value());
            ++linkExtra;
        }
        obj[QStringLiteral("links")] = linksArray;

        array.append(obj);
    }

    while (roleExtra != _extraRoles.cend()) {
        array.append(roleExtra.value());
        ++roleExtra;
    }
    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/") + kRolesFileName;
    QString error;
    if (!_writeBytes(filePath, QJsonDocument(array).toJson(), &error)) {
        _setPersistenceError(
            tr("Unable to save vehicle roles file %1: %2. Changes have not been saved.").arg(filePath, error));
        qCWarning(VehicleRoleLog) << "Failed to save roles file:" << filePath << error;
        return;
    }
    _dirty = false;
    _setPersistenceError({});
}
