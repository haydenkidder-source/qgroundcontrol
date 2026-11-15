#include "LinkManager.h"
#include "QmlObjectListModel.h"

void LinkManager::dismissSerialNotice(const QString& id)
{
    for (int index = 0; index < _serialNotices->count(); ++index) {
        auto* notice = _serialNotices->value<SerialConnectionNotice*>(index);
        if (notice->id() == id && !notice->_acknowledged) {
            notice->_acknowledged = true;
            emit notice->changed();
            emit serialNoticesChanged();
            return;
        }
    }
}

#ifndef QGC_NO_SERIAL_LINK
#include <algorithm>
#include <utility>

#include <QtCore/QUuid>

#include "AutoConnectSettings.h"
#include "SerialAutoConnect.h"
#include "SerialLink.h"

bool LinkManager::_currentSerialAttachment(const LinkConfiguration* config) const
{
    if (!config || !config->isDynamic()) {
        return true;
    }
    const auto* serial = qobject_cast<const SerialConfiguration*>(config);
    const quint64 generation = _serialConfigurationGenerations.value(config);
    const auto attached = serial ? _serialAttachments.constFind(serial->portName()) : _serialAttachments.cend();
    return !generation || (attached != _serialAttachments.cend() && attached->generation == generation);
}

bool LinkManager::_serialRecoveryEligible(const LinkConfiguration* config) const
{
    if (!config || !config->isAutoConnect() || config->suppressAutoReconnect() || !_currentSerialAttachment(config)) {
        return false;
    }
    if (!config->isDynamic()) {
        return config->autoConnectStarted();
    }
    const auto* serial = qobject_cast<const SerialConfiguration*>(config);
    const auto attached = serial ? _serialAttachments.constFind(serial->portName()) : _serialAttachments.cend();
    if (!serial || !_autoConnectSettings || attached == _serialAttachments.cend() || attached->port.bootloader ||
        _serialNoticeSuppressed(serial->portName()) ||
        SerialPortManager::instance()->isAutoConnectExcluded(serial->portName())) {
        return false;
    }
    switch (attached->port.boardType) {
        case QGCSerialPortInfo::BoardTypePixhawk:
            return _autoConnectSettings->autoConnectPixhawk()->rawValue().toBool();
        case QGCSerialPortInfo::BoardTypeSiKRadio:
            return _autoConnectSettings->autoConnectSiKRadio()->rawValue().toBool();
        case QGCSerialPortInfo::BoardTypeOpenPilot:
            return _autoConnectSettings->autoConnectLibrePilot()->rawValue().toBool();
        default:
            return false;
    }
}

bool LinkManager::_verifiedSerialConnection(const LinkConfiguration* config) const
{
    if (!config || !config->link()) {
        return false;
    }
    const auto state = _serialConnectionStates.constFind(config->link());
    return state != _serialConnectionStates.cend() && state->connected && !state->intentionalDisconnect &&
           config->link()->isConnected();
}

bool LinkManager::_serialNoticeSuppressed(const QString& port) const
{
    const auto snapshot = SerialPortManager::instance()->portSnapshot();
    QSet<QString> seenDevices;
    for (const auto& entry : snapshot) {
        const bool matches = entry.systemLocation == port || entry.portName == port;
        if (matches && entry.boardType == QGCSerialPortInfo::BoardTypeRTKGPS) {
            return true;
        }
        const bool recognised = entry.boardType == QGCSerialPortInfo::BoardTypePixhawk ||
                                entry.boardType == QGCSerialPortInfo::BoardTypeSiKRadio ||
                                entry.boardType == QGCSerialPortInfo::BoardTypeOpenPilot;
        if (recognised && !SerialPortManager::isPrimaryInterface(entry, seenDevices) && matches) {
            return true;
        }
    }
    const auto attached = _serialAttachments.constFind(port);
    return _serialSecondaryPorts.contains(port) ||
           (attached != _serialAttachments.cend() && attached->port.boardType == QGCSerialPortInfo::BoardTypeRTKGPS);
}

LinkManager::SerialNoticeIncident& LinkManager::_serialIncident(LinkConfiguration* config)
{
    const auto* serial = qobject_cast<const SerialConfiguration*>(config);
    const QString port = serial ? serial->portName() : QString();
    const auto attached = _serialAttachments.constFind(port);
    QString key = _serialConfigurationKeys.value(config);
    if (key.isEmpty()) {
        key = config->isDynamic() ? QStringLiteral("discovery:") + port
                                  : QUuid::createUuid().toString(QUuid::WithoutBraces);
        _serialConfigurationKeys.insert(config, key);
        _serialConfigurationGenerations.insert(config,
                                               attached != _serialAttachments.cend() ? attached->generation : 0);
    }
    if (!_serialNoticeWatchedConfigs.contains(config)) {
        _serialNoticeWatchedConfigs.insert(config);
        (void) connect(config, &QObject::destroyed, this, [this, config] {
            _forgetSerialConfiguration(config);
            _serialNoticeWatchedConfigs.remove(config);
        });
        if (auto* serialConfig = qobject_cast<SerialConfiguration*>(config)) {
            (void) connect(serialConfig, &SerialConfiguration::portNameChanged, this,
                           [this, config] { _forgetSerialConfiguration(config); });
        }
        (void) connect(config, &LinkConfiguration::dynamicChanged, this,
                       [this, config] { _forgetSerialConfiguration(config); });
        (void) connect(config, &LinkConfiguration::autoConnectChanged, this, [this, config] {
            const auto found = _serialNoticeIncidents.find(_serialConfigurationKeys.value(config));
            if (found != _serialNoticeIncidents.end()) {
                _refreshSerialIncident(found.value());
            }
        });
    }
    auto found = _serialNoticeIncidents.find(key);
    if (found != _serialNoticeIncidents.end() && found->port != port) {
        _forgetSerialNotice(key);
        found = _serialNoticeIncidents.end();
    }
    if (found != _serialNoticeIncidents.end() && attached != _serialAttachments.cend() &&
        found->generation != attached->generation && (!found->notice || !found->notice->active())) {
        // An unresolved saved loss spans a missing device and its return. Queuing an open cannot resolve it.
        _serialNoticeIncidents.erase(found);
        found = _serialNoticeIncidents.end();
    }
    if (found == _serialNoticeIncidents.end()) {
        SerialNoticeIncident incident;
        incident.config = config;
        incident.configurationIdentity = config;
        incident.port = port;
        incident.name = config->name();
        incident.description = attached != _serialAttachments.cend() ? attached->port.description
                                                                     : (serial ? serial->portDisplayName() : QString());
        incident.generation = attached != _serialAttachments.cend() ? attached->generation : ++_serialNoticeGeneration;
        found = _serialNoticeIncidents.insert(key, incident);
    } else {
        found->config = config;
        found->configurationIdentity = config;
        found->name = config->name();
    }
    return found.value();
}

void LinkManager::_forgetSerialNotice(const QString& key)
{
    const auto found = _serialNoticeIncidents.find(key);
    if (found == _serialNoticeIncidents.end()) {
        return;
    }
    if (auto* notice = found->notice.data()) {
        if (_serialNotices->contains(notice)) {
            _serialNotices->removeOne(notice);
        }
        if (_serialNoticeHistory->contains(notice)) {
            _serialNoticeHistory->removeOne(notice);
        }
        notice->deleteLater();
    }
    _serialNoticeIncidents.erase(found);
    emit serialNoticesChanged();
}

void LinkManager::_forgetSerialConfiguration(const LinkConfiguration* config)
{
    const QString key = _serialConfigurationKeys.take(config);
    _serialConfigurationGenerations.remove(config);
    const auto found = _serialNoticeIncidents.constFind(key);
    // QPointer may already be cleared when QObject::destroyed reaches this callback.
    if (found != _serialNoticeIncidents.cend() && found->configurationIdentity == config) {
        _forgetSerialNotice(key);
    }
}

void LinkManager::_trimSerialNoticeHistory()
{
    while (_serialNoticeHistory->count() > SERIAL_NOTICE_HISTORY_CAP) {
        auto* notice = _serialNoticeHistory->removeAt(0);
        // QPointer is cleared immediately, so a new transition cannot revive an evicted history object.
        delete notice;
    }
}

void LinkManager::_raiseSerialNotice(SerialNoticeIncident& incident, const QString& reason, const QString& detail,
                                     const QString& ownerKey)
{
    if (_serialNoticeSuppressed(incident.port)) {
        return;
    }
    if (incident.config && incident.config->isDynamic() &&
        std::ranges::any_of(_serialNoticeIncidents, [&incident](const auto& other) {
            return other.config && !other.config->isDynamic() && other.port == incident.port && other.notice &&
                   other.notice->active();
        })) {
        return;
    }
    if (reason != QStringLiteral("still_unavailable") && incident.escalated && incident.issue == reason &&
        incident.ownerKey == ownerKey && incident.notice &&
        incident.notice->reason() == QStringLiteral("still_unavailable")) {
        return;
    }
    if (reason != QStringLiteral("still_unavailable")) {
        incident.issue = reason;
        incident.detail = detail;
        incident.ownerKey = ownerKey;
    }
    if (incident.config && !incident.config->isDynamic()) {
        _forgetSerialNotice(QStringLiteral("discovery:") + incident.port);
    }
    if (!incident.notice) {
        if (incident.id.isEmpty()) {
            incident.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        incident.notice = new SerialConnectionNotice(
            incident.id,
            incident.config && !incident.config->isDynamic() ? QStringLiteral("saved") : QStringLiteral("discovery"),
            incident.generation, this);
    }
    auto* notice = incident.notice.data();
    const QString transitionOwner = reason == QStringLiteral("still_unavailable") ? incident.ownerKey : ownerKey;
    const QString transition = reason + u'\n' + transitionOwner;
    const bool alert = !notice->_active || notice->_transition != transition;
    const QString text = _serialNoticeText(incident, reason);
    const QStringList actions = incident.issue == QStringLiteral("reservation_failed") &&
                                        incident.ownerKey.startsWith(QStringLiteral("GPSReceiver:"))
                                    ? QStringList{QStringLiteral("OpenGnssReceiver"), QStringLiteral("OpenCommLinks"),
                                                  QStringLiteral("Dismiss")}
                                    : QStringList{QStringLiteral("OpenCommLinks"), QStringLiteral("Dismiss")};
    const bool changed = alert || notice->_text != text || notice->_actions != actions;
    notice->_reason = reason;
    notice->_transition = transition;
    notice->_text = text;
    notice->_actions = actions;
    notice->_active = true;
    if (alert) {
        notice->_acknowledged = false;
    }
    if (_serialNoticeHistory->contains(notice)) {
        _serialNoticeHistory->removeOne(notice);
    }
    if (!_serialNotices->contains(notice)) {
        _serialNotices->append(notice);
    }
    if (changed) {
        emit notice->changed();
        emit serialNoticesChanged();
    }
    if (alert) {
        emit serialNoticeRaised(notice->id());
    }
}

void LinkManager::_resolveSerialNotice(SerialNoticeIncident& incident, const QString& text)
{
    auto* notice = incident.notice.data();
    if (!notice || !notice->_active) {
        return;
    }
    notice->_active = false;
    notice->_text = text;
    notice->_actions.clear();
    _serialNotices->removeOne(notice);
    _serialNoticeHistory->append(notice);
    emit notice->changed();
    emit serialNoticesChanged();
    _trimSerialNoticeHistory();
}

void LinkManager::_serialConnected(LinkConfiguration* config)
{
    if (!_verifiedSerialConnection(config) || !_currentSerialAttachment(config)) {
        return;
    }
    auto& incident = _serialIncident(config);
    _resolveSerialNotice(incident, tr("%1: serial connection restored.").arg(incident.name));
    incident.lost = false;
    incident.deadlineUs = 0;
    incident.escalated = false;
    incident.issue.clear();
    // A verified saved transport also satisfies discovery at this exact location.
    if (!config->isDynamic()) {
        _forgetSerialNotice(QStringLiteral("discovery:") + incident.port);
    }
}

void LinkManager::_serialLost(LinkConfiguration* config, AutoConnectReason recovery)
{
    if (!_currentSerialAttachment(config)) {
        return;
    }
    auto& incident = _serialIncident(config);
    incident.lost = true;
    incident.escalated = false;
    incident.deadlineUs = _serialNoticeClock() + SERIAL_UNAVAILABLE_DELAY_US;
    if (!config->isDynamic() && recovery != AutoConnectReason::RetryEnabled) {
        _raiseSerialNotice(incident, QString::fromLatin1(autoConnectReasonToken(recovery)));
    }
    _refreshSerialIncident(incident);
}

void LinkManager::_serialOpenFailed(LinkConfiguration* config, const SerialOpenOutcome& outcome)
{
    if (!config || !outcome.automatic || config->suppressAutoReconnect() || !_currentSerialAttachment(config)) {
        return;
    }
    auto& incident = _serialIncident(config);
    if (!incident.deadlineUs) {
        incident.deadlineUs = _serialNoticeClock() + SERIAL_UNAVAILABLE_DELAY_US;
    }
    const QString reason =
        outcome.stage == SerialOpenOutcome::Stage::Bootloader
            ? QString::fromLatin1(SerialAutoConnect::skipReasonToken(SerialAutoConnect::SkipReason::Bootloader))
        : outcome.stage == SerialOpenOutcome::Stage::Configuration ? QStringLiteral("configuration_failed")
        : outcome.error == QSerialPort::PermissionError            ? QStringLiteral("access_denied")
        : outcome.error == QSerialPort::DeviceNotFoundError        ? QStringLiteral("device_not_found")
                                                                   : QStringLiteral("open_failed");
    _raiseSerialNotice(incident, reason, outcome.errorString);
}

void LinkManager::_serialReservationFailed(LinkConfiguration* config)
{
    if (!_currentSerialAttachment(config)) {
        return;
    }
    auto& incident = _serialIncident(config);
    const auto owner = SerialPortManager::instance()->reservationInfo(incident.port);
    if (!owner || owner->kind == SerialPortManager::OwnerKind::Unknown) {
        return;
    }
    // Another verified vehicle transport already satisfies discovery, but not a different saved configuration.
    if (config->isDynamic()) {
        const auto liveLinks = links();
        if (std::ranges::any_of(liveLinks, [this, &incident](const auto& liveLink) {
                const auto* serial = qobject_cast<const SerialConfiguration*>(liveLink->linkConfiguration().get());
                return serial && serial->portName() == incident.port && _verifiedSerialConnection(serial);
            })) {
            return;
        }
    }
    if (!incident.deadlineUs && _serialRecoveryEligible(config)) {
        incident.deadlineUs = _serialNoticeClock() + SERIAL_UNAVAILABLE_DELAY_US;
    }
    _raiseSerialNotice(incident, QString::fromLatin1(autoConnectReasonToken(AutoConnectReason::ReservationFailed)),
                       owner->label,
                       QString::fromLatin1(SerialPortManager::ownerToken(owner->kind)) + u':' + owner->label);
}

void LinkManager::_refreshSerialIncident(SerialNoticeIncident& incident)
{
    const auto* config = incident.config.data();
    if (config) {
        incident.name = config->name();
    }
    if (!config || _verifiedSerialConnection(config) || _serialNoticeSuppressed(incident.port)) {
        return;
    }
    const bool eligible = _serialRecoveryEligible(config);
    QString blocker;
    QString detail;
    if (incident.lost && !config->isDynamic()) {
        if (!config->isAutoConnect()) {
            blocker = QString::fromLatin1(autoConnectReasonToken(AutoConnectReason::AutoConnectOff));
        } else if (config->suppressAutoReconnect()) {
            blocker = QString::fromLatin1(autoConnectReasonToken(AutoConnectReason::Suppressed));
        } else if (!config->autoConnectStarted()) {
            blocker = QString::fromLatin1(autoConnectReasonToken(AutoConnectReason::NeverStarted));
        } else if (_connectionsSuspended) {
            blocker = QStringLiteral("connections_suspended");
            detail = _connectionsSuspendedReason;
        } else {
            const auto attached = _serialAttachments.constFind(incident.port);
            if (attached != _serialAttachments.cend() && attached->port.bootloader) {
                blocker =
                    QString::fromLatin1(SerialAutoConnect::skipReasonToken(SerialAutoConnect::SkipReason::Bootloader));
            }
        }
    }
    if (!blocker.isEmpty()) {
        _raiseSerialNotice(incident, blocker, detail);
        return;
    }
    const QStringList policyBlockers{QStringLiteral("auto_connect_off"), QStringLiteral("suppressed"),
                                     QStringLiteral("never_started"), QStringLiteral("connections_suspended"),
                                     QStringLiteral("bootloader")};
    if (eligible && policyBlockers.contains(incident.issue)) {
        _resolveSerialNotice(incident, tr("%1: connection policy now permits automatic retry.").arg(incident.name));
        incident.issue.clear();
        incident.deadlineUs = _serialNoticeClock() + SERIAL_UNAVAILABLE_DELAY_US;
    }
    if (eligible && !_connectionsSuspended && incident.deadlineUs && !incident.escalated &&
        _serialNoticeClock() >= incident.deadlineUs) {
        incident.escalated = true;
        _raiseSerialNotice(incident, QStringLiteral("still_unavailable"));
    }
}

void LinkManager::_updateSerialNotices(const QList<SerialPortManager::Port>& ports)
{
    QMap<QString, SerialAttachment> attachments;
    QSet<QString> primaryLocations;
    QSet<QString> seenDevices;
    for (const auto& port : ports) {
        const auto previous = _serialAttachments.constFind(port.systemLocation);
        const bool same = previous != _serialAttachments.cend() &&
                          previous->port.physicalDeviceId == port.physicalDeviceId &&
                          previous->port.boardType == port.boardType && previous->port.bootloader == port.bootloader;
        attachments.insert(port.systemLocation, {port, same ? previous->generation : ++_serialNoticeGeneration});
        const bool recognised = port.boardType == QGCSerialPortInfo::BoardTypePixhawk ||
                                port.boardType == QGCSerialPortInfo::BoardTypeSiKRadio ||
                                port.boardType == QGCSerialPortInfo::BoardTypeOpenPilot;
        if (!recognised || SerialPortManager::isPrimaryInterface(port, seenDevices)) {
            primaryLocations.insert(port.systemLocation);
        }
    }
    _serialAttachments = std::move(attachments);
    _serialNoticeStoppedPorts.removeIf([this](const QString& port) { return !_serialAttachments.contains(port); });
    _serialSecondaryPorts.clear();
    for (const auto& port : ports) {
        if (!primaryLocations.contains(port.systemLocation) && port.boardType != QGCSerialPortInfo::BoardTypeRTKGPS) {
            _serialSecondaryPorts.insert(port.systemLocation);
        }
    }
    const auto keys = _serialNoticeIncidents.keys();
    for (const auto& key : keys) {
        const auto found = _serialNoticeIncidents.find(key);
        if (found == _serialNoticeIncidents.end()) {
            continue;
        }
        auto& incident = found.value();
        if (_serialNoticeSuppressed(incident.port)) {
            _forgetSerialNotice(key);
            continue;
        }
        if (!incident.config) {
            const auto attached = _serialAttachments.constFind(incident.port);
            if (attached == _serialAttachments.cend() || attached->generation != incident.generation) {
                _forgetSerialNotice(key);
                continue;
            }
        } else if (incident.config->isDynamic()) {
            const auto attached = _serialAttachments.constFind(incident.port);
            if (attached == _serialAttachments.cend() || attached->generation != incident.generation) {
                _forgetSerialNotice(key);
                continue;
            }
        }
        _refreshSerialIncident(incident);
    }
    if (!_autoConnectSettings) {
        return;
    }
    for (const auto& port : ports) {
        const auto board = port.boardType;
        const bool recognised = board == QGCSerialPortInfo::BoardTypePixhawk ||
                                board == QGCSerialPortInfo::BoardTypeSiKRadio ||
                                board == QGCSerialPortInfo::BoardTypeOpenPilot;
        if (!recognised || !primaryLocations.contains(port.systemLocation)) {
            continue;
        }
        const bool enabled = board == QGCSerialPortInfo::BoardTypePixhawk
                                 ? _autoConnectSettings->autoConnectPixhawk()->rawValue().toBool()
                             : board == QGCSerialPortInfo::BoardTypeSiKRadio
                                 ? _autoConnectSettings->autoConnectSiKRadio()->rawValue().toBool()
                                 : _autoConnectSettings->autoConnectLibrePilot()->rawValue().toBool();
        const QString key = QStringLiteral("discovery:") + port.systemLocation;
        const auto owner = SerialPortManager::instance()->reservationInfo(port.systemLocation);
        const auto liveLinks = links();
        const bool connected = std::ranges::any_of(liveLinks, [this, &port](const auto& liveLink) {
            const auto* serial = qobject_cast<const SerialConfiguration*>(liveLink->linkConfiguration().get());
            return serial && serial->portName() == port.systemLocation && _verifiedSerialConnection(serial);
        });
        const bool savedIncident = std::ranges::any_of(_serialNoticeIncidents, [&port](const auto& incident) {
            return incident.config && !incident.config->isDynamic() && incident.port == port.systemLocation &&
                   incident.notice && incident.notice->active();
        });
        if (connected || savedIncident || _serialNoticeStoppedPorts.contains(port.systemLocation) ||
            (owner && owner->kind == SerialPortManager::OwnerKind::GPSReceiver)) {
            _forgetSerialNotice(key);
            continue;
        }
        auto found = _serialNoticeIncidents.find(key);
        if (enabled) {
            if (found != _serialNoticeIncidents.end() && found->issue == QStringLiteral("class_disabled")) {
                _resolveSerialNotice(found.value(),
                                     tr("%1: automatic connection for this device class is enabled.").arg(found->name));
            }
            continue;
        }
        // A policy notice belongs to an attachment even before discovery creates a configuration.
        if (found == _serialNoticeIncidents.end()) {
            SerialNoticeIncident incident;
            incident.port = port.systemLocation;
            incident.name = port.boardName.isEmpty() ? port.portName : port.boardName;
            incident.description = port.description;
            incident.generation = _serialAttachments.value(port.systemLocation).generation;
            found = _serialNoticeIncidents.insert(key, incident);
        }
        const QString label =
            board == QGCSerialPortInfo::BoardTypePixhawk    ? tr("Automatically connect to a Pixhawk board")
            : board == QGCSerialPortInfo::BoardTypeSiKRadio ? tr("Automatically connect to a SiK Radio")
                                                            : tr("Automatically connect to a LibrePilot");
        _raiseSerialNotice(
            found.value(),
            QString::fromLatin1(SerialAutoConnect::skipReasonToken(SerialAutoConnect::SkipReason::ClassDisabled)),
            label);
    }
}

QString LinkManager::_serialNoticeText(const SerialNoticeIncident& incident, const QString& reason) const
{
    const QString identity = tr("%1, %2, %3")
                                 .arg(incident.name, incident.port,
                                      incident.description.isEmpty() ? tr("Serial device") : incident.description);
    const QString messageReason =
        reason == QStringLiteral("still_unavailable") && !incident.issue.isEmpty() ? incident.issue : reason;
    QString body;
    if (messageReason == QStringLiteral("auto_connect_off")) {
        body = tr("Serial link '%1' on %2 was lost. Automatic retry for this saved link is off.\n"
                  "Reattach the device, then open Application Settings > Comm Links > Links and select Connect.\n"
                  "For future automatic recovery, edit the disconnected link with its pencil icon, enable "
                  "Automatically Connect on Start, and Save.")
                   .arg(incident.name, incident.port);
    } else if (messageReason == QStringLiteral("class_disabled")) {
        body = tr("%1 (%2) was detected. Automatic connection for this device class is off.\n"
                  "Open Application Settings > Comm Links > AutoConnect and enable '%3', or configure a named link "
                  "under Links.")
                   .arg(incident.name, incident.port, incident.detail);
    } else if (messageReason == QStringLiteral("suppressed") || messageReason == QStringLiteral("never_started")) {
        body = tr("Serial link '%1' was lost. Automatic connection is enabled, but this saved link is paused or has "
                  "not been started this session.\nOpen Application Settings > Comm Links > Links and select Connect.")
                   .arg(incident.name);
    } else if (messageReason == QStringLiteral("reservation_failed")) {
        if (incident.ownerKey.startsWith(QStringLiteral("GPSReceiver:"))) {
            body = tr("%1 is being used by the GNSS receiver '%2'.\nIf vehicle telemetry should use this port, open "
                      "Application Settings > GNSS Receiver and select Disconnect, then select the correct telemetry "
                      "Serial Port in Comm Links.")
                       .arg(incident.port, incident.detail);
        } else {
            body = tr("'%1' cannot use %2; QGroundControl link '%3' owns it.\nIn Application Settings > Comm Links, "
                      "disconnect the unintended link or select another Serial Port, Save, then Connect.")
                       .arg(incident.name, incident.port, incident.detail);
        }
    } else if (messageReason == QStringLiteral("connections_suspended")) {
        body = tr("Serial link '%1' was lost. New connections are paused: %2.\n"
                  "Finish or leave the operation that paused connections, then use Application Settings > "
                  "Comm Links > Links to select Connect if needed.")
                   .arg(incident.name, incident.detail);
    } else if (messageReason == QStringLiteral("bootloader")) {
        body = tr("'%1' is waiting for %2 to finish starting.\nLet the device leave its bootloader. If needed, check "
                  "device power and USB cable, then use Application Settings > Comm Links > Links to select Connect.")
                   .arg(incident.name, incident.port);
    } else if (messageReason == QStringLiteral("device_not_found") ||
               messageReason == QStringLiteral("still_unavailable")) {
        body = messageReason == QStringLiteral("device_not_found")
                   ? tr("'%1' is waiting for %2, which is not detected.").arg(incident.name, incident.port)
                   : tr("'%1' is still unavailable on %2.").arg(incident.name, incident.port);
        body += u'\n' + tr("Check device power and USB cable. If its port changed, stop retries with Disconnect in "
                           "Application Settings > Comm Links > Links, use the pencil icon to update Serial Port, "
                           "Save, then Connect.");
        if (!incident.detail.isEmpty()) {
            body += u'\n' + tr("Last connection error: %1").arg(incident.detail);
        }
    } else {
        body = messageReason == QStringLiteral("configuration_failed")
                   ? tr("'%1' cannot configure %2: %3.").arg(incident.name, incident.port, incident.detail)
                   : tr("'%1' cannot open %2: %3.").arg(incident.name, incident.port, incident.detail);
        body += u'\n' + tr("Check the cable and selected Serial Port/Baud Rate in Application Settings > Comm Links > "
                           "Links; close another application using the device. If access is denied, check "
                           "operating-system serial-port permission.");
    }
    if (incident.config && _serialRecoveryEligible(incident.config) && !_connectionsSuspended &&
        messageReason != QStringLiteral("class_disabled") && messageReason != QStringLiteral("bootloader")) {
        body += u'\n' + tr("Automatic retry is pending.");
    }
    if (reason == QStringLiteral("still_unavailable") && !incident.issue.isEmpty()) {
        body = tr("Serial connection is still unavailable.\n") + body;
    }
    return identity + u'\n' + body;
}
#endif
