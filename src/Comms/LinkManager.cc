#include "LinkManager.h"

#include "AppMessages.h"
#include "AutoConnectSettings.h"
#include "BluetoothLink.h"
#include "ConnectionDiagnostics.h"
#include "LogReplayLink.h"
#include "MAVLinkProtocol.h"
#include "MavlinkSettings.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QGCNetworkHelper.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "TCPLink.h"
#include "UDPLink.h"

#ifndef QGC_NO_SERIAL_LINK
#include "SerialAutoConnect.h"
#include "SerialLink.h"
#include "SerialPortManager.h"
#ifdef Q_OS_ANDROID
#include "AndroidSerial.h"
#include "AppSettings.h"
#endif
#endif

#ifdef QT_DEBUG
#include "MockLink.h"
#endif

#include <algorithm>
#include <utility>

#include <QtCore/QApplicationStatic>
#include <QtCore/QTimer>

QGC_LOGGING_CATEGORY(LinkManagerLog, "Comms.LinkManager")
QGC_LOGGING_CATEGORY(LinkManagerVerboseLog, "Comms.LinkManager:verbose")

Q_APPLICATION_STATIC(LinkManager, _linkManagerInstance);

LinkManager::LinkManager(QObject *parent)
    : QObject(parent)
    , _portListTimer(new QTimer(this))
    , _qmlConfigurations(new QmlObjectListModel(this))
{
    qCDebug(LinkManagerLog) << this;

    (void) qRegisterMetaType<QAbstractSocket::SocketError>("QAbstractSocket::SocketError");
    (void) qRegisterMetaType<LinkInterface*>("LinkInterface*");
#ifndef QGC_NO_SERIAL_LINK
    (void) qRegisterMetaType<QGCSerialPortInfo>("QGCSerialPortInfo");
    (void) connect(SerialPortManager::instance(), &SerialPortManager::portsEnumerated, this,
                   &LinkManager::_updateSerialPorts);
#endif
}

LinkManager::~LinkManager()
{
    qCDebug(LinkManagerLog) << this;
}

LinkManager *LinkManager::instance()
{
    return _linkManagerInstance();
}

void LinkManager::init()
{
    _autoConnectSettings = SettingsManager::instance()->autoConnectSettings();

#ifndef QGC_NO_SERIAL_LINK
#ifdef Q_OS_ANDROID
    // The serial backend is fixed at startup. Changing the setting requires an app restart.
    AndroidSerial::setUsePosixSerial(
        SettingsManager::instance()->appSettings()->androidUsePosixSerial()->rawValue().toBool());
    SerialPortManager::instance()->setSinglePortOnly(!AndroidSerial::usePosixSerial());
#endif
    if (!_serialAutoConnect) {
        _serialAutoConnect = std::make_unique<SerialAutoConnect>(
            *SerialPortManager::instance(), [this](SharedLinkConfigurationPtr& config) { createConnectedLink(config); },
            [this](const QString& location) {
                const auto liveLinks = links();
                return std::ranges::any_of(liveLinks, [&location](const auto& liveLink) {
                    const auto config = liveLink->linkConfiguration();
                    const auto* serial = qobject_cast<const SerialConfiguration*>(config.get());
                    return serial && serial->portName() == location && config->link() == liveLink.get() &&
                           liveLink->isConnected();
                });
            });
    }
#endif

    if (!QGC::runningUnitTests()) {
        (void) connect(_portListTimer, &QTimer::timeout, this, &LinkManager::_updateAutoConnectLinks);
        _portListTimer->start(_autoconnectUpdateTimerMSecs); // timeout must be long enough to get past bootloader on second pass
    }
}

QList<SharedLinkInterfacePtr> LinkManager::links()
{
    QMutexLocker locker(&_linksMutex);
    return _rgLinks;
}

QmlObjectListModel *LinkManager::_qmlLinkConfigurations()
{
    return _qmlConfigurations;
}

void LinkManager::createConnectedLink(const LinkConfiguration *config)
{
    for (SharedLinkConfigurationPtr &sharedConfig : _rgLinkConfigs) {
        if (sharedConfig.get() == config) {
            sharedConfig->setAutoConnectStarted(true);
            sharedConfig->resetReconnectBackoff();
            createConnectedLink(sharedConfig);
        }
    }
}

void LinkManager::disconnectLink(LinkInterface *link)
{
    if (!link) {
        return;
    }

    const SharedLinkConfigurationPtr config = link->linkConfiguration();
    if (config) {
        config->setSuppressAutoReconnect(true);
    }

#ifndef QGC_NO_SERIAL_LINK
    if (_serialConnectionStates.contains(link)) {
        _serialConnectionStates[link].intentionalDisconnect = true;
    }
#endif
    link->disconnect();
}

void LinkManager::disconnectLinkConfiguration(LinkConfiguration *config)
{
    if (!config) {
        return;
    }

    config->setSuppressAutoReconnect(true);

    if (LinkInterface *const link = config->link()) {
#ifndef QGC_NO_SERIAL_LINK
        if (_serialConnectionStates.contains(link)) {
            _serialConnectionStates[link].intentionalDisconnect = true;
        }
#endif
        link->disconnect();
    }
}

bool LinkManager::createConnectedLink(SharedLinkConfigurationPtr &config)
{
    config->setSuppressAutoReconnect(false);

    if (config->link() != nullptr) {
        _logCreationFailure(config.get(), AutoConnectReason::HasLink);
        return true;
    }

    SharedLinkInterfacePtr link = nullptr;

    switch(config->type()) {
#ifndef QGC_NO_SERIAL_LINK
        case LinkConfiguration::TypeSerial: {
            const auto* serialConfig = qobject_cast<const SerialConfiguration*>(config.get());
            if (!serialConfig) {
                _logCreationFailure(config.get(), AutoConnectReason::InvalidSerialConfiguration);
                return false;
            }
            auto reservation = SerialPortManager::instance()->reservePort(
                serialConfig->portName(), SerialPortManager::OwnerKind::MAVLink, config->name());
            if (!reservation) {
                _logCreationFailure(config.get(), AutoConnectReason::ReservationFailed);
                return false;
            }
            link = std::make_shared<SerialLink>(config, std::move(reservation));
            break;
        }
#endif
    case LinkConfiguration::TypeUdp:
        link = std::make_shared<UDPLink>(config);
        break;
    case LinkConfiguration::TypeTcp:
        link = std::make_shared<TCPLink>(config);
        break;
    case LinkConfiguration::TypeBluetooth:
        link = std::make_shared<BluetoothLink>(config);
        break;
    case LinkConfiguration::TypeLogReplay:
        link = std::make_shared<LogReplayLink>(config);
        break;
#ifdef QT_DEBUG
    case LinkConfiguration::TypeMock:
        link = std::make_shared<MockLink>(config);
        break;
#endif
    case LinkConfiguration::TypeLast:
    default:
        break;
    }

    if (!link) {
        _logCreationFailure(config.get(), AutoConnectReason::DispatchFailed);
        return false;
    }

    if (!link->_allocateMavlinkChannel()) {
        _logCreationFailure(config.get(), AutoConnectReason::NoMavlinkChannel);
        qCWarning(LinkManagerLog) << "Link failed to setup mavlink channels";
        return false;
    }

    // Set up signal connections before adding to list, so link is fully initialized
    (void) connect(link.get(), &LinkInterface::communicationError, this, &LinkManager::_communicationError);
    // Links emit bytesReceived from their worker threads, so delivery is queued with a raw link pointer. Once this link
    // is destroyed another link can be allocated at the same address, and its stale bytes would then pass
    // MAVLinkProtocol's address-based liveness check and be attributed to the new link (e.g. creating a ghost vehicle
    // bound to another vehicle's link). Tie each delivery to this link's lifetime instead of its address.
    (void) connect(link.get(), &LinkInterface::bytesReceived, MAVLinkProtocol::instance(),
                   [weakLink = std::weak_ptr<LinkInterface>(link)](LinkInterface* sender, const QByteArray& data) {
                       const SharedLinkInterfacePtr liveLink = weakLink.lock();
                       if (liveLink && (liveLink.get() == sender)) {
                           MAVLinkProtocol::instance()->receiveBytes(sender, data);
                       }
                   });
    (void) connect(link.get(), &LinkInterface::bytesSent, MAVLinkProtocol::instance(), &MAVLinkProtocol::logSentBytes);
    (void) connect(link.get(), &LinkInterface::connected, this, &LinkManager::_linkConnected);
    (void) connect(link.get(), &LinkInterface::disconnected, this, &LinkManager::_linkDisconnected);

#ifndef QGC_NO_SERIAL_LINK
    if (config->type() == LinkConfiguration::TypeSerial) {
        _serialConnectionStates.insert(link.get(), {});
        (void) connect(link.get(), &QObject::destroyed, this,
                       [this, serialLink = link.get()] { _serialConnectionStates.remove(serialLink); });
    }
#endif

    MAVLinkProtocol::instance()->resetMetadataForLink(link.get());

    // Try to connect before adding to active links list
    if (!link->_connect()) {
        _logCreationFailure(config.get(), AutoConnectReason::DispatchFailed);
        (void) disconnect(link.get(), &LinkInterface::communicationError, this, &LinkManager::_communicationError);
        (void) disconnect(link.get(), &LinkInterface::bytesReceived, MAVLinkProtocol::instance(), nullptr);
        (void) disconnect(link.get(), &LinkInterface::bytesSent, MAVLinkProtocol::instance(), &MAVLinkProtocol::logSentBytes);
        (void) disconnect(link.get(), &LinkInterface::disconnected, this, &LinkManager::_linkDisconnected);
        link->_freeMavlinkChannel();
        config->setLink(nullptr);
        return false;
    }

    {
        QMutexLocker locker(&_linksMutex);
        _rgLinks.append(link);
    }
    config->setLink(link);

    return true;
}

void LinkManager::_linkConnected()
{
    const LinkInterface *const link = qobject_cast<LinkInterface*>(sender());
    const SharedLinkConfigurationPtr config = link ? link->linkConfiguration() : nullptr;
    if (config) {
        config->noteConnected();
#ifndef QGC_NO_SERIAL_LINK
        if (config->type() == LinkConfiguration::TypeSerial) {
            _serialConnectionStates[link].connected = true;
        }
#endif
    }
}

void LinkManager::_communicationError(const QString &title, const QString &error)
{
    const LinkInterface *const link = qobject_cast<LinkInterface*>(sender());
    const SharedLinkConfigurationPtr config = link ? link->linkConfiguration() : nullptr;

    // Auto-connect links retry on a timer; a popup per failed attempt is just noise. Log only.
    if (config && config->isAutoConnect() && !config->suppressAutoReconnect()) {
        qCDebug(LinkManagerLog) << "Auto-connect link error (will retry):" << title << error;
        return;
    }

    // Dynamic links recreated on a timer: only the first attempt of a failure streak pops up
    if (config && (config->reconnectAttempts() > 1)) {
        qCDebug(LinkManagerLog) << "Link error on retry:" << title << error;
        return;
    }

    QGC::showAppMessage(error, title);
}

SharedLinkInterfacePtr LinkManager::mavlinkForwardingLink()
{
    QMutexLocker locker(&_linksMutex);

    for (const SharedLinkInterfacePtr &link : _rgLinks) {
        const SharedLinkConfigurationPtr linkConfig = link->linkConfiguration();
        if (linkConfig && (linkConfig->type() == LinkConfiguration::TypeUdp) && (linkConfig->name() == _mavlinkForwardingLinkName)) {
            return link;
        }
    }

    return nullptr;
}

SharedLinkInterfacePtr LinkManager::mavlinkForwardingSupportLink()
{
    QMutexLocker locker(&_linksMutex);

    for (const SharedLinkInterfacePtr &link : _rgLinks) {
        const SharedLinkConfigurationPtr linkConfig = link->linkConfiguration();
        if (linkConfig && (linkConfig->type() == LinkConfiguration::TypeUdp) && (linkConfig->name() == _mavlinkForwardingSupportLinkName)) {
            return link;
        }
    }

    return nullptr;
}

void LinkManager::disconnectAll()
{
    QList<SharedLinkInterfacePtr> links;
    {
        QMutexLocker locker(&_linksMutex);
        links = _rgLinks;
    }

    for (const SharedLinkInterfacePtr &sharedLink: links) {
#ifndef QGC_NO_SERIAL_LINK
        if (_serialConnectionStates.contains(sharedLink.get())) {
            _serialConnectionStates[sharedLink.get()].intentionalDisconnect = true;
        }
#endif
        sharedLink->disconnect();
    }
}

void LinkManager::_linkDisconnected()
{
    LinkInterface* const link = qobject_cast<LinkInterface*>(sender());

    if (!link) {
        return;
    }

    SharedLinkInterfacePtr linkToCleanup;
    SharedLinkConfigurationPtr config;
    {
        QMutexLocker locker(&_linksMutex);

        for (auto it = _rgLinks.begin(); it != _rgLinks.end(); ++it) {
            if (it->get() == link) {
                config = it->get()->linkConfiguration();
                const QString linkName = config ? config->name() : QStringLiteral("<null config>");
                qCDebug(LinkManagerLog) << linkName << "use_count:" << it->use_count();
                linkToCleanup = *it;
                (void) _rgLinks.erase(it);
                break;
            }
        }
    }

    if (!linkToCleanup) {
        qCDebug(LinkManagerLog) << "link already removed";
        return;
    }

#ifndef QGC_NO_SERIAL_LINK
    const auto intent = _serialConnectionStates.constFind(link);
    if (intent != _serialConnectionStates.cend()) {
        if (intent->connected && !intent->intentionalDisconnect && config) {
            const AutoConnectReason recovery = !config->isAutoConnect()          ? AutoConnectReason::AutoConnectOff
                                               : config->suppressAutoReconnect() ? AutoConnectReason::Suppressed
                                               : (!config->isDynamic() && !config->autoConnectStarted())
                                                   ? AutoConnectReason::NeverStarted
                                                   : AutoConnectReason::RetryEnabled;
            qCDebug(LinkManagerLog).noquote().nospace()
                << "link_manager event=serial_loss" << _diagnosticConfiguration(config.get())
                << " recovery=" << autoConnectReasonToken(recovery);
        }
        _serialConnectionStates.remove(link);
    }
#endif
    if (config) {
        config->noteDisconnected();
        config->setLink(nullptr);

        if (config->isDynamic() && (config->name() == _mavlinkForwardingSupportLinkName) &&
            _mavlinkSupportForwardingEnabled) {
            _mavlinkSupportForwardingEnabled = false;
            emit mavlinkSupportForwardingEnabledChanged();
        }
    }

    (void) disconnect(link, &LinkInterface::communicationError, this, &LinkManager::_communicationError);
    (void) disconnect(link, &LinkInterface::bytesReceived, MAVLinkProtocol::instance(), nullptr);
    (void) disconnect(link, &LinkInterface::bytesSent, MAVLinkProtocol::instance(), &MAVLinkProtocol::logSentBytes);
    (void) disconnect(link, &LinkInterface::connected, this, &LinkManager::_linkConnected);
    (void) disconnect(link, &LinkInterface::disconnected, this, &LinkManager::_linkDisconnected);

    link->_freeMavlinkChannel();
}

SharedLinkInterfacePtr LinkManager::sharedLinkInterfacePointerForLink(const LinkInterface *link)
{
    QMutexLocker locker(&_linksMutex);

    for (const SharedLinkInterfacePtr &sharedLink: _rgLinks) {
        if (sharedLink.get() == link) {
            return sharedLink;
        }
    }

    // Link not found - this is normal during disconnect when queued signals are still processing.
    // Callers should check for nullptr return value.
    qCDebug(LinkManagerLog) << "link not in list (likely disconnected)";
    return SharedLinkInterfacePtr(nullptr);
}

bool LinkManager::_connectionsSuspendedMsg() const
{
    if (_connectionsSuspended) {
        QGC::showAppMessage(tr("Connect not allowed: %1").arg(_connectionsSuspendedReason));
        return true;
    }

    return false;
}

void LinkManager::saveLinkConfigurationList()
{
    QSettings settings;
    settings.remove(LinkConfiguration::settingsRoot());

    int trueCount = 0;
    for (int i = 0; i < _rgLinkConfigs.count(); i++) {
        SharedLinkConfigurationPtr linkConfig = _rgLinkConfigs[i];
        if (!linkConfig) {
            qCWarning(LinkManagerLog) << "Internal error for link configuration in LinkManager";
            continue;
        }

        if (linkConfig->isDynamic()) {
            continue;
        }

        const QString root = LinkConfiguration::settingsRoot() + QStringLiteral("/Link%1").arg(trueCount++);
        settings.setValue(root + "/name", linkConfig->name());
        settings.setValue(root + "/type", linkConfig->type());
        settings.setValue(root + "/auto", linkConfig->isAutoConnect());
        settings.setValue(root + "/high_latency", linkConfig->isHighLatency());
        linkConfig->saveSettings(settings, root);
    }

    const QString root = QString(LinkConfiguration::settingsRoot());
    settings.setValue(root + "/count", trueCount);
}

void LinkManager::loadLinkConfigurationList()
{
    QSettings settings;
    // Is the group even there?
    if (settings.contains(LinkConfiguration::settingsRoot() + "/count")) {
        // Find out how many configurations we have
        const int count = settings.value(LinkConfiguration::settingsRoot() + "/count").toInt();
        for (int i = 0; i < count; i++) {
            const QString root = LinkConfiguration::settingsRoot() + QStringLiteral("/Link%1").arg(i);
            if (!settings.contains(root + "/type")) {
                qCWarning(LinkManagerLog) << "Link Configuration" << root << "has no type.";
                continue;
            }

            LinkConfiguration::LinkType type = static_cast<LinkConfiguration::LinkType>(settings.value(root + "/type").toInt());
            if (type >= LinkConfiguration::TypeLast) {
                qCWarning(LinkManagerLog) << "Link Configuration" << root << "an invalid type:" << type;
                continue;
            }

            if (!settings.contains(root + "/name")) {
                qCWarning(LinkManagerLog) << "Link Configuration" << root << "has no name.";
                continue;
            }

            const QString name = settings.value(root + "/name").toString();
            if (name.isEmpty()) {
                qCWarning(LinkManagerLog) << "Link Configuration" << root << "has an empty name.";
                continue;
            }

            LinkConfiguration* link = nullptr;
            switch(type) {
#ifndef QGC_NO_SERIAL_LINK
            case LinkConfiguration::TypeSerial:
                link = new SerialConfiguration(name);
                break;
#endif
            case LinkConfiguration::TypeUdp:
                link = new UDPConfiguration(name);
                break;
            case LinkConfiguration::TypeTcp:
                link = new TCPConfiguration(name);
                break;
            case LinkConfiguration::TypeBluetooth:
                link = new BluetoothConfiguration(name);
                break;
            case LinkConfiguration::TypeLogReplay:
                link = new LogReplayConfiguration(name);
                break;
#ifdef QT_DEBUG
            case LinkConfiguration::TypeMock:
                link = new MockConfiguration(name);
                break;
#endif
            case LinkConfiguration::TypeLast:
            default:
                break;
            }

            if (link) {
                const bool autoConnect = settings.value(root + "/auto").toBool();
                link->setAutoConnect(autoConnect);
                const bool highLatency = settings.value(root + "/high_latency").toBool();
                link->setHighLatency(highLatency);
                link->loadSettings(settings, root);
                addConfiguration(link);
            }
        }
    }

    // Enable automatic Serial PX4/3DR Radio hunting
    _configurationsLoaded = true;
}

void LinkManager::_addUDPAutoConnectLink()
{
    if (!_autoConnectSettings->autoConnectUDP()->rawValue().toBool()) {
        return;
    }

    _retryDynamicUdpLink(_defaultUDPLinkName, [this](UDPConfiguration& udpConfig) {
        udpConfig.setAutoConnect(true);
        // Listening is this link's main job; an unresolvable target must not stop it
        udpConfig.setRequireResolvedHosts(false);
        udpConfig.setLocalPort(_autoConnectSettings->udpListenPort()->rawValue().toUInt());
        const QString targetHostIP = _autoConnectSettings->udpTargetHostIP()->rawValue().toString();
        if (!targetHostIP.isEmpty()) {
            udpConfig.addHost(targetHostIP, _autoConnectSettings->udpTargetHostPort()->rawValue().toUInt());
        }
    });
}

void LinkManager::_addMAVLinkForwardingLink()
{
    if (!SettingsManager::instance()->mavlinkSettings()->forwardMavlink()->rawValue().toBool()) {
        return;
    }

    _retryDynamicUdpLink(_mavlinkForwardingLinkName, [](UDPConfiguration& udpConfig) {
        udpConfig.setForwarding(true);
        udpConfig.addHost(
            SettingsManager::instance()->mavlinkSettings()->forwardMavlinkHostName()->rawValue().toString());
    });
}

void LinkManager::_retryDynamicUdpLink(const QString& name, const std::function<void(UDPConfiguration&)>& configure)
{
    SharedLinkConfigurationPtr config = _findDynamicUdpConfiguration(name);
    if (config && (config->link() || !config->reconnectReady())) {
        return;
    }

    UDPConfiguration udpConfig(name);
    udpConfig.setDynamic(true);
    configure(udpConfig);

    // Reuse the config on retry so its backoff applies and configs don't pile up
    if (config) {
        config->copyFrom(&udpConfig);
        qCDebug(LinkManagerLog) << "Retrying dynamic UDP link:" << name;
    } else {
        config = addConfiguration(new UDPConfiguration(&udpConfig));
        qCDebug(LinkManagerLog) << "New dynamic UDP link added:" << name << udpConfig.hostList();
    }

    config->noteReconnectAttempt();
    createConnectedLink(config);
}

SharedLinkConfigurationPtr LinkManager::_findDynamicUdpConfiguration(const QString& name) const
{
    // User-created links may use the same name; those must never be reused
    for (const SharedLinkConfigurationPtr& config : _rgLinkConfigs) {
        if (config && config->isDynamic() && (config->type() == LinkConfiguration::TypeUdp) &&
            (config->name() == name)) {
            return config;
        }
    }

    return nullptr;
}

void LinkManager::_reconnectAutoConnectLinks()
{
    _retryGateStates.removeIf([this](const auto& entry) {
        return std::ranges::none_of(_rgLinkConfigs, [&entry](const auto& config) {
            return config && config.get() == entry.key() && !config->isDynamic();
        });
    });
    for (SharedLinkConfigurationPtr &config : _rgLinkConfigs) {
        if (!config || config->isDynamic()) {
            continue;
        }
        if (!config->isAutoConnect()) {
            _recordRetryGate(config.get(), AutoConnectReason::AutoConnectOff);
            continue;
        }

        // Preserve gate precedence for links started this session.
        if (config->link()) {
            _recordRetryGate(config.get(), AutoConnectReason::HasLink);
            continue;
        }
        if (config->suppressAutoReconnect()) {
            _recordRetryGate(config.get(), AutoConnectReason::Suppressed);
            continue;
        }
        if (!config->autoConnectStarted()) {
            _recordRetryGate(config.get(), AutoConnectReason::NeverStarted);
            continue;
        }
        if (!config->reconnectReady()) {
            _recordRetryGate(config.get(), AutoConnectReason::RetryPending);
            continue;
        }
        if (_retryGateStates.remove(config.get())) {
            qCDebug(LinkManagerLog).noquote().nospace()
                << "link_manager event=retry_clear" << _diagnosticConfiguration(config.get()) << " cause=ready";
        }
        qCDebug(LinkManagerLog) << "Reconnecting auto-connect link" << config->name();
        config->noteReconnectAttempt();
        createConnectedLink(config);
    }
}

void LinkManager::_updateAutoConnectLinks()
{
#ifndef QGC_NO_SERIAL_LINK
    // Existing links still need fresh unplug checks while new connections are suspended.
    const auto ports = SerialPortManager::instance()->availablePorts();
#endif
    if (_connectionsSuspended) {
        return;
    }

    _addUDPAutoConnectLink();
    _addMAVLinkForwardingLink();
    _reconnectAutoConnectLinks();

#ifndef QGC_NO_SERIAL_LINK
    if (_serialAutoConnect) {
        _serialAutoConnect->update(ports, {_autoConnectSettings->autoConnectPixhawk()->rawValue().toBool(),
                                           _autoConnectSettings->autoConnectSiKRadio()->rawValue().toBool(),
                                           _autoConnectSettings->autoConnectLibrePilot()->rawValue().toBool()});
    }
#endif
}

void LinkManager::shutdown()
{
    setConnectionsSuspended(tr("Shutdown"));
    disconnectAll();

    // Wait for all the vehicles to go away to ensure an orderly shutdown and deletion of all objects
    while (MultiVehicleManager::instance()->vehicles()->count()) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
}

QStringList LinkManager::linkTypeStrings() const
{
    //-- Must follow same order as enum LinkType in LinkConfiguration.h
    static QStringList list;
    if (!list.isEmpty()) {
        return list;
    }

#ifndef QGC_NO_SERIAL_LINK
    list += tr("Serial");
#endif
    list += tr("UDP");
    list += tr("TCP");
    list += tr("Bluetooth");
#ifdef QT_DEBUG
    list += tr("Mock Link");
#endif
    list += tr("Log Replay");

    if (list.size() != static_cast<int>(LinkConfiguration::TypeLast)) {
        qCWarning(LinkManagerLog) << "Internal error";
    }

    return list;
}

void LinkManager::endConfigurationEditing(LinkConfiguration *config, LinkConfiguration *editedConfig)
{
    if (!config || !editedConfig) {
        qCWarning(LinkManagerLog) << "Internal error";
        return;
    }

    config->copyFrom(editedConfig);
    saveLinkConfigurationList();
    emit config->nameChanged(config->name());
    // Discard temporary duplicate
    delete editedConfig;
}

void LinkManager::endCreateConfiguration(LinkConfiguration *config)
{
    if (!config) {
        qCWarning(LinkManagerLog) << "Internal error";
        return;
    }

    addConfiguration(config);
    saveLinkConfigurationList();
}

LinkConfiguration *LinkManager::createConfiguration(int type, const QString &name)
{
#ifndef QGC_NO_SERIAL_LINK
    if (static_cast<LinkConfiguration::LinkType>(type) == LinkConfiguration::TypeSerial) {
        _updateSerialPorts();
    }
#endif

    return LinkConfiguration::createSettings(type, name);
}

LinkConfiguration *LinkManager::startConfigurationEditing(LinkConfiguration *config)
{
    if (!config) {
        qCWarning(LinkManagerLog) << "Internal error";
        return nullptr;
    }

#ifndef QGC_NO_SERIAL_LINK
    if (config->type() == LinkConfiguration::TypeSerial) {
        _updateSerialPorts();
    }
#endif

    return LinkConfiguration::duplicateSettings(config);
}

void LinkManager::removeConfiguration(LinkConfiguration *config)
{
    if (!config) {
        qCWarning(LinkManagerLog) << "Internal error";
        return;
    }

    LinkInterface* const link = config->link();
    if (link) {
#ifndef QGC_NO_SERIAL_LINK
        if (_serialConnectionStates.contains(link)) {
            _serialConnectionStates[link].intentionalDisconnect = true;
        }
#endif
        link->disconnect();
    }

    _removeConfiguration(config);
    saveLinkConfigurationList();
}

void LinkManager::createMavlinkForwardingSupportLink()
{
    SharedLinkConfigurationPtr config = _findDynamicUdpConfiguration(_mavlinkForwardingSupportLinkName);
    if (config && config->link()) {
        return;
    }

    UDPConfiguration udpConfig(_mavlinkForwardingSupportLinkName);
    udpConfig.setDynamic(true);
    udpConfig.setForwarding(true);
    udpConfig.addHost(
        SettingsManager::instance()->mavlinkSettings()->forwardMavlinkAPMSupportHostName()->rawValue().toString());

    if (config) {
        config->copyFrom(&udpConfig);
    } else {
        config = addConfiguration(new UDPConfiguration(&udpConfig));
    }

    if (!createConnectedLink(config)) {
        return;
    }

    // Cleared again in _linkDisconnected if the connect fails
    _mavlinkSupportForwardingEnabled = true;
    emit mavlinkSupportForwardingEnabledChanged();
}

void LinkManager::_removeConfiguration(const LinkConfiguration *config)
{
    _retryGateStates.remove(config);
    (void) _qmlConfigurations->removeOne(config);

    for (auto it = _rgLinkConfigs.begin(); it != _rgLinkConfigs.end(); ++it) {
        if (it->get() == config) {
            (void) _rgLinkConfigs.erase(it);
            return;
        }
    }

    qCWarning(LinkManagerLog) << "called with unknown config";
}

bool LinkManager::isBluetoothAvailable()
{
    return QGCNetworkHelper::isBluetoothAvailable();
}

bool LinkManager::containsLink(const LinkInterface *link)
{
    QMutexLocker locker(&_linksMutex);

    for (const SharedLinkInterfacePtr &sharedLink : _rgLinks) {
        if (sharedLink.get() == link) {
            return true;
        }
    }

    return false;
}

SharedLinkConfigurationPtr LinkManager::addConfiguration(LinkConfiguration *config)
{
    (void) _qmlConfigurations->append(config);
    (void) _rgLinkConfigs.append(SharedLinkConfigurationPtr(config));

    return _rgLinkConfigs.last();
}

bool LinkManager::containsConfiguration(const QString& name) const
{
    return std::ranges::any_of(_rgLinkConfigs, [&name](const SharedLinkConfigurationPtr& config) {
        return config && (config->name() == name);
    });
}

void LinkManager::startAutoConnectedLinks()
{
    for (SharedLinkConfigurationPtr &sharedConfig : _rgLinkConfigs) {
        if (sharedConfig->isAutoConnect()) {
            sharedConfig->setAutoConnectStarted(true);
            createConnectedLink(sharedConfig);
        }
    }
}

uint8_t LinkManager::allocateMavlinkChannel()
{
    for (uint8_t mavlinkChannel = 0; mavlinkChannel < MAVLINK_COMM_NUM_BUFFERS; mavlinkChannel++) {
        if (_mavlinkChannelsUsedBitMask & (1 << mavlinkChannel)) {
            continue;
        }

        mavlink_reset_channel_status(mavlinkChannel);
        mavlink_status_t* const mavlinkStatus = mavlink_get_channel_status(mavlinkChannel);
        mavlinkStatus->flags |= MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
        _mavlinkChannelsUsedBitMask |= (1 << mavlinkChannel);
        qCDebug(LinkManagerLog) << "allocateMavlinkChannel" << mavlinkChannel;
        return mavlinkChannel;
    }

    qCWarning(LinkManagerLog) << "allocateMavlinkChannel: all channels reserved!";
    return invalidMavlinkChannel();
}

void LinkManager::freeMavlinkChannel(uint8_t channel)
{
    qCDebug(LinkManagerLog) << "freeMavlinkChannel" << channel;

    if (invalidMavlinkChannel() == channel) {
        return;
    }

    _mavlinkChannelsUsedBitMask &= ~(1 << channel);
}

LogReplayLink *LinkManager::startLogReplay(const QString &logFile)
{
    LogReplayConfiguration* const linkConfig = new LogReplayConfiguration(tr("Log Replay"));
    linkConfig->setLogFilename(logFile);
    linkConfig->setName(linkConfig->logFilenameShort());

    SharedLinkConfigurationPtr sharedConfig = addConfiguration(linkConfig);
    if (createConnectedLink(sharedConfig)) {
        return qobject_cast<LogReplayLink*>(sharedConfig->link());
    }

    return nullptr;
}

bool LinkManager::isLinkUSBDirect([[maybe_unused]] const LinkInterface *link)
{
#ifndef QGC_NO_SERIAL_LINK
    const SerialLink* const serialLink = qobject_cast<const SerialLink*>(link);
    if (!serialLink) {
        return false;
    }

    const SharedLinkConfigurationPtr config = serialLink->linkConfiguration();
    if (!config) {
        return false;
    }

    const SerialConfiguration* const serialConfig = qobject_cast<const SerialConfiguration*>(config.get());
    if (serialConfig && serialConfig->usbDirect()) {
        return link;
    }
#endif

    return false;
}

#ifndef QGC_NO_SERIAL_LINK // Serial Only Functions

void LinkManager::_updateSerialPorts()
{
    QStringList portList;
    QStringList displayList;
    const auto inventory = SerialPortManager::instance()->availablePorts();
    for (const SerialPortManager::Port& info : inventory) {
        const QString port = info.systemLocation;
        portList += port;
        displayList += info.displayName.isEmpty() ? info.portName : info.displayName;
    }
    const bool portsChanged = _commPortList != portList;
    const bool displayChanged = _commPortDisplayList != displayList;
    _commPortList = std::move(portList);
    _commPortDisplayList = std::move(displayList);
    if (portsChanged) {
        emit commPortsChanged();
    }
    if (displayChanged) {
        emit commPortStringsChanged();
    }
}

QStringList LinkManager::serialPortStrings()
{
    if (_commPortDisplayList.isEmpty()) {
        _updateSerialPorts();
    }

    return _commPortDisplayList;
}

QStringList LinkManager::serialPorts()
{
    if (_commPortList.isEmpty()) {
        _updateSerialPorts();
    }

    return _commPortList;
}

QStringList LinkManager::serialBaudRates()
{
    return SerialConfiguration::supportedBaudRates();
}

#endif // QGC_NO_SERIAL_LINK

const char* LinkManager::autoConnectReasonToken(AutoConnectReason reason)
{
    switch (reason) {
        case AutoConnectReason::AutoConnectOff:
            return "auto_connect_off";
        case AutoConnectReason::HasLink:
            return "has_link";
        case AutoConnectReason::Suppressed:
            return "suppressed";
        case AutoConnectReason::NeverStarted:
            return "never_started";
        case AutoConnectReason::RetryPending:
            return "retry_pending";
        case AutoConnectReason::RetryEnabled:
            return "retry_enabled";
        case AutoConnectReason::ReservationFailed:
            return "reservation_failed";
        case AutoConnectReason::InvalidSerialConfiguration:
            return "invalid_serial_configuration";
        case AutoConnectReason::NoMavlinkChannel:
            return "no_mavlink_channel";
        case AutoConnectReason::DispatchFailed:
            return "dispatch_failed";
    }
    return "unknown";
}

QString LinkManager::_diagnosticConfiguration(const LinkConfiguration* config)
{
    QString port;
#ifndef QGC_NO_SERIAL_LINK
    if (const auto* serial = qobject_cast<const SerialConfiguration*>(config)) {
        port = serial->portName();
    }
#endif
    return QStringLiteral(" config=%1 port=%2")
        .arg(ConnectionDiagnostics::quoted(config ? config->name() : QString()), ConnectionDiagnostics::quoted(port));
}

void LinkManager::_recordRetryGate(const LinkConfiguration* config, AutoConnectReason reason)
{
    const auto previous = _retryGateStates.constFind(config);
    if (previous != _retryGateStates.cend() && previous.value() == reason) {
        return;
    }
    _retryGateStates.insert(config, reason);
    qCDebug(LinkManagerLog).noquote().nospace() << "link_manager event=retry_skip" << _diagnosticConfiguration(config)
                                                << " reason=" << autoConnectReasonToken(reason);
}

void LinkManager::_logCreationFailure(const LinkConfiguration* config, AutoConnectReason reason)
{
    qCDebug(LinkManagerLog).noquote().nospace()
        << "link_manager event=create_failed" << _diagnosticConfiguration(config)
        << " reason=" << autoConnectReasonToken(reason);
}

void LinkManager::setConnectionsSuspended(const QString& reason)
{
    if (!_connectionsSuspended || _connectionsSuspendedReason != reason) {
        qCDebug(LinkManagerLog).noquote().nospace()
            << "link_manager event=suspended reason=" << ConnectionDiagnostics::quoted(reason);
    }
    _connectionsSuspended = true;
    _connectionsSuspendedReason = reason;
}

void LinkManager::setConnectionsAllowed()
{
    if (_connectionsSuspended) {
        qCDebug(LinkManagerLog).noquote().nospace()
            << "link_manager event=resumed reason=" << ConnectionDiagnostics::quoted(_connectionsSuspendedReason);
    }
    _connectionsSuspended = false;
}
