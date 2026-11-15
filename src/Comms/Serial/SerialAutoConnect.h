#pragma once

#include <QtCore/QMap>

#include <functional>

#include "LinkConfiguration.h"
#include "SerialPortManager.h"

/// Application-thread MAVLink serial discovery. LinkManager owns creation and live links.
class SerialAutoConnect
{
    friend class SerialAutoConnectTest;
    friend class LinkManagerTest;

public:
    enum class SkipReason
    {
        None,
        ClassDisabled,
        UnsupportedClass,
        Bootloader,
        RoutingExcluded,
        SecondaryInterface,
        PortReserved,
        BackendBusy,
        InvalidPort,
        ConfigurationExists,
        Suppressed,
        Backoff,
        Settling
    };
    enum class Setting
    {
        None,
        Pixhawk,
        SiKRadio,
        OpenPilot
    };
    static const char* skipReasonToken(SkipReason reason);
    static const char* settingToken(Setting setting);

    struct Options
    {
        bool pixhawk = false;
        bool sikRadio = false;
        bool openPilot = false;
    };

    using Connect = std::function<void(SharedLinkConfigurationPtr&)>;
    using Connected = std::function<bool(const QString&)>;
    SerialAutoConnect(SerialPortManager& ports, Connect connect, Connected connected = {});
    ~SerialAutoConnect();
    void update(const QList<SerialPortManager::Port>& ports, const Options& options);

private:
    struct Decision
    {
        SkipReason reason = SkipReason::None;
        Setting setting = Setting::None;
    };

    struct SkipState
    {
        Decision decision;
        SerialPortManager::Owner owner;
    };

    static Decision _allowed(QGCSerialPortInfo::BoardType_t boardType, const Options& options);
    void _recordSkip(const SerialPortManager::Port& port, Decision decision);
    void _clearSkip(const QString& location, const char* cause);

    struct Identity
    {
        QString physicalDeviceId;
        QGCSerialPortInfo::BoardType_t boardType;
        bool bootloader;
        bool operator==(const Identity&) const = default;
    };

    SerialPortManager& _ports;
    Connect _connect;
    Connected _connected;
    QMap<QString, SkipState> _skipStates;
    QMap<QString, Identity> _identities;
    SerialPortSettleTracker _waitingPorts;
    QMap<QString, SharedLinkConfigurationPtr> _configs;
};
