#include "SerialAutoConnect.h"

#include <algorithm>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QSet>

#include "ConnectionDiagnostics.h"
#include "MonotonicClock.h"
#include "QGCLoggingCategory.h"
#include "SerialLink.h"

QGC_LOGGING_CATEGORY(SerialAutoConnectLog, "Comms.Serial.SerialAutoConnect")

SerialAutoConnect::SerialAutoConnect(SerialPortManager& ports, Connect connect, Connected connected)
    : _ports(ports)
    , _connect(std::move(connect))
    , _connected(std::move(connected))
{
    qCDebug(SerialAutoConnectLog) << this;
}

SerialAutoConnect::~SerialAutoConnect()
{
    qCDebug(SerialAutoConnectLog) << this;
}

void SerialAutoConnect::update(const QList<SerialPortManager::Port>& ports, const Options& options)
{
    const auto isAbsentLocation = [&ports](const QString& location) {
        return std::none_of(ports.cbegin(), ports.cend(),
                            [&location](const auto& port) { return port.systemLocation == location; });
    };
    const auto isAbsent = [&isAbsentLocation](const auto& entry) { return isAbsentLocation(entry.key()); };
    for (auto it = _skipStates.cbegin(); it != _skipStates.cend();) {
        const QString location = it.key();
        ++it;
        if (isAbsentLocation(location)) {
            _clearSkip(location, "removed");
        }
    }
    _identities.removeIf(isAbsent);
    for (const auto& port : ports) {
        const Identity identity{port.physicalDeviceId, port.boardType, port.bootloader};
        const auto previous = _identities.constFind(port.systemLocation);
        if (previous != _identities.cend() && previous.value() != identity) {
            _clearSkip(port.systemLocation, "identity_changed");
            _configs.remove(port.systemLocation);
            _waitingPorts.remove(port.systemLocation);
        }
        _identities.insert(port.systemLocation, identity);
    }
    _configs.removeIf(isAbsent);
    _waitingPorts.removeIf(isAbsentLocation);

    QSet<QString> seenDevices;
    for (const SerialPortManager::Port& port : ports) {
        const Decision allowed = _allowed(port.boardType, options);
        if (allowed.reason != SkipReason::None) {
            _recordSkip(port, allowed);
            continue;
        }
        if (port.bootloader) {
            _recordSkip(port, {SkipReason::Bootloader});
            continue;
        }
        if (_ports.isAutoConnectExcluded(port.systemLocation)) {
            _recordSkip(port, {SkipReason::RoutingExcluded});
            continue;
        }
        // Select one MAVLink interface per physical device, even while its port is reserved or retrying.
        if (!SerialPortManager::isPrimaryInterface(port, seenDevices)) {
            _recordSkip(port, {SkipReason::SecondaryInterface});
            continue;
        }
        if (!_ports.canAutoConnectPort(port.systemLocation)) {
            const SkipReason reason = _ports.isPortReserved(port.systemLocation) ? SkipReason::PortReserved
                                      : port.systemLocation.trimmed().isEmpty()  ? SkipReason::InvalidPort
                                                                                 : SkipReason::BackendBusy;
            _recordSkip(port, {reason});
            continue;
        }
        auto config = _configs.value(port.systemLocation);
        if (config) {
            if (config->link()) {
                _recordSkip(port, {SkipReason::ConfigurationExists});
            } else if (config->suppressAutoReconnect()) {
                _recordSkip(port, {SkipReason::Suppressed});
            } else if (!config->reconnectReady()) {
                _recordSkip(port, {SkipReason::Backoff});
            } else {
                config->noteReconnectAttempt();
                _connect(config);
            }
            continue;
        }
        if (!_waitingPorts.settled(port.systemLocation, MonotonicClock::nowUs())) {
            _recordSkip(port, {SkipReason::Settling});
            continue;
        }
        _waitingPorts.remove(port.systemLocation);
        auto* serialConfig = new SerialConfiguration(
            QCoreApplication::translate("LinkManager", "%1 on %2 (AutoConnect)").arg(port.boardName, port.portName));
        serialConfig->setUsbDirect(port.boardType == QGCSerialPortInfo::BoardTypePixhawk);
        serialConfig->setSikRadio(port.boardType == QGCSerialPortInfo::BoardTypeSiKRadio);
        serialConfig->setBaud(port.boardType == QGCSerialPortInfo::BoardTypeSiKRadio ? 57600 : 115200);
        serialConfig->setDynamic(true);
        serialConfig->setPortName(port.systemLocation);
        serialConfig->setAutoConnect(true);
        config.reset(serialConfig);
        _configs.insert(port.systemLocation, config);
        config->noteReconnectAttempt();
        _connect(config);
    }
}

SerialAutoConnect::Decision SerialAutoConnect::_allowed(QGCSerialPortInfo::BoardType_t boardType,
                                                        const Options& options)
{
    switch (boardType) {
        case QGCSerialPortInfo::BoardTypePixhawk:
            return options.pixhawk ? Decision{} : Decision{SkipReason::ClassDisabled, Setting::Pixhawk};
        case QGCSerialPortInfo::BoardTypeSiKRadio:
            return options.sikRadio ? Decision{} : Decision{SkipReason::ClassDisabled, Setting::SiKRadio};
        case QGCSerialPortInfo::BoardTypeOpenPilot:
            return options.openPilot ? Decision{} : Decision{SkipReason::ClassDisabled, Setting::OpenPilot};
        default:
            return {SkipReason::UnsupportedClass};
    }
}

const char* SerialAutoConnect::skipReasonToken(SkipReason reason)
{
    switch (reason) {
        case SkipReason::None:
            return "none";
        case SkipReason::ClassDisabled:
            return "class_disabled";
        case SkipReason::UnsupportedClass:
            return "unsupported_class";
        case SkipReason::Bootloader:
            return "bootloader";
        case SkipReason::RoutingExcluded:
            return "routing_excluded";
        case SkipReason::SecondaryInterface:
            return "secondary_interface";
        case SkipReason::PortReserved:
            return "port_reserved";
        case SkipReason::BackendBusy:
            return "backend_busy";
        case SkipReason::InvalidPort:
            return "invalid_port";
        case SkipReason::ConfigurationExists:
            return "configuration_exists";
        case SkipReason::Suppressed:
            return "suppressed";
        case SkipReason::Backoff:
            return "backoff";
        case SkipReason::Settling:
            return "settling";
    }
    return "unknown";
}

const char* SerialAutoConnect::settingToken(Setting setting)
{
    switch (setting) {
        case Setting::None:
            return "unknown";
        case Setting::Pixhawk:
            return "autoConnectPixhawk";
        case Setting::SiKRadio:
            return "autoConnectSiKRadio";
        case Setting::OpenPilot:
            return "autoConnectLibrePilot";
    }
    return "unknown";
}

void SerialAutoConnect::_recordSkip(const SerialPortManager::Port& port, Decision decision)
{
    const auto config = _configs.value(port.systemLocation);
    if ((config && config->link() && config->link()->isConnected()) ||
        (_connected && _connected(port.systemLocation))) {
        _clearSkip(port.systemLocation, "connected");
        return;
    }
    const auto owner = (decision.reason == SkipReason::RoutingExcluded ? _ports.exclusionInfo(port.systemLocation)
                                                                       : _ports.reservationInfo(port.systemLocation))
                           .value_or(SerialPortManager::Owner{});
    const auto previous = _skipStates.constFind(port.systemLocation);
    if (previous != _skipStates.cend() && previous->decision.reason == decision.reason &&
        previous->decision.setting == decision.setting && previous->owner == owner) {
        return;
    }
    _skipStates.insert(port.systemLocation, {decision, owner});
    const auto usbToken = [](const std::optional<quint16>& identifier) {
        return identifier ? QStringLiteral("%1").arg(*identifier, 4, 16, QLatin1Char('0')) : QStringLiteral("unknown");
    };
    const char* board = "Unknown";
    switch (port.boardType) {
        case QGCSerialPortInfo::BoardTypePixhawk:
            board = "Pixhawk";
            break;
        case QGCSerialPortInfo::BoardTypeSiKRadio:
            board = "SiKRadio";
            break;
        case QGCSerialPortInfo::BoardTypeOpenPilot:
            board = "OpenPilot";
            break;
        case QGCSerialPortInfo::BoardTypeRTKGPS:
            board = "RTKGPS";
            break;
        default:
            break;
    }
    qCDebug(SerialAutoConnectLog).noquote().nospace()
        << "serial_auto_connect event=skip port=" << ConnectionDiagnostics::quoted(port.systemLocation)
        << " name=" << ConnectionDiagnostics::quoted(port.portName)
        << " description=" << ConnectionDiagnostics::quoted(port.description)
        << " vid=" << usbToken(port.vendorIdentifier) << " pid=" << usbToken(port.productIdentifier)
        << " board=" << board << " reason=" << skipReasonToken(decision.reason)
        << " setting=" << settingToken(decision.setting)
        << " config=" << ConnectionDiagnostics::quoted(config ? config->name() : QString())
        << " owner=" << ConnectionDiagnostics::quoted(owner.label)
        << " owner_kind=" << SerialPortManager::ownerToken(owner.kind);
}

void SerialAutoConnect::_clearSkip(const QString& location, const char* cause)
{
    if (_skipStates.remove(location)) {
        qCDebug(SerialAutoConnectLog).noquote().nospace()
            << "serial_auto_connect event=clear port=" << ConnectionDiagnostics::quoted(location) << " cause=" << cause;
    }
}
