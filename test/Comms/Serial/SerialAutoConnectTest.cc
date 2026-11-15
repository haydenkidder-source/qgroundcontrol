#include "SerialAutoConnectTest.h"

#include <limits>

#include "ScopedConnectionLogCapture.h"
#include "SerialAutoConnect.h"
#include "SerialLink.h"

void SerialAutoConnectTest::_compositeSelection_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("firstSelected");
    QTest::addColumn<bool>("secondSelected");
    QTest::newRow("duplicate") << QStringLiteral("duplicate") << true << false;
    QTest::newRow("nmea-label-is-not-mavlink-policy") << QStringLiteral("nmea-label") << true << false;
    QTest::newRow("gps-sibling") << QStringLiteral("gps-sibling") << false << true;
    QTest::newRow("bootloader-sibling") << QStringLiteral("bootloader") << false << true;
    QTest::newRow("routed-sibling") << QStringLiteral("excluded") << false << true;
    QTest::newRow("busy-primary") << QStringLiteral("busy") << false << false;
    QTest::newRow("unknown-identities") << QStringLiteral("unknown") << true << true;
    QTest::newRow("distinct-devices") << QStringLiteral("distinct") << true << true;
}

void SerialAutoConnectTest::_compositeSelection()
{
    QFETCH(QString, scenario);
    QFETCH(bool, firstSelected);
    QFETCH(bool, secondSelected);
    SerialPortManager ports;
    SerialAutoConnect discovery(ports, [](SharedLinkConfigurationPtr&) {});
    SerialPortManager::Port first{QStringLiteral("/test/composite-first"), QStringLiteral("first"),
                                  QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    first.physicalDeviceId = QStringLiteral("1:2:serial");
    auto second = first;
    second.systemLocation = QStringLiteral("/test/composite-second");
    second.portName = QStringLiteral("second");
    if (scenario == QStringLiteral("nmea-label")) {
        second.description = QStringLiteral("NMEA interface");
    } else if (scenario == QStringLiteral("gps-sibling")) {
        first.boardType = QGCSerialPortInfo::BoardTypeRTKGPS;
    } else if (scenario == QStringLiteral("bootloader")) {
        first.bootloader = true;
    } else if (scenario == QStringLiteral("unknown")) {
        first.physicalDeviceId.clear();
        second.physicalDeviceId.clear();
    } else if (scenario == QStringLiteral("distinct")) {
        second.physicalDeviceId = QStringLiteral("1:2:other");
    }
    SerialPortManager::ReservationPtr claim;
    if (scenario == QStringLiteral("busy")) {
        claim = ports.reservePort(first.systemLocation);
        QVERIFY(claim);
    } else if (scenario == QStringLiteral("excluded")) {
        claim = ports.excludeFromAutoConnect(first.systemLocation);
        QVERIFY(claim);
    }
    // Inspect the discovery grace period before either candidate opens a serial port.
    discovery.update({first, second}, {.pixhawk = true});
    QCOMPARE(discovery._waitingPorts.contains(first.systemLocation), firstSelected);
    QCOMPARE(discovery._waitingPorts.contains(second.systemLocation), secondSelected);
}

void SerialAutoConnectTest::_discoveryDeadlineAndRetryState()
{
    SerialPortManager ports;
    QList<SharedLinkConfigurationPtr> attempts;
    SerialAutoConnect discovery(ports, [&](SharedLinkConfigurationPtr& config) { attempts.append(config); });
    const QString name = QStringLiteral("/test/autoconnect");
    const QList<SerialPortManager::Port> inventory{
        {name, QStringLiteral("autoconnect"), QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")}};
    const SerialAutoConnect::Options enabled{.pixhawk = true};
    discovery.update(inventory, enabled);
    QVERIFY(attempts.isEmpty());
    QVERIFY(!discovery._waitingPorts.settled(name, 0));
    discovery._waitingPorts._deadlinesUs[name] = std::numeric_limits<quint64>::max();
    for (int i = 0; i < 10; ++i) {
        discovery.update(inventory, enabled);
    }
    QVERIFY(attempts.isEmpty());
    discovery._waitingPorts._deadlinesUs[name] = 0;
    discovery.update(inventory, enabled);
    QCOMPARE(attempts.size(), 1);
    const auto config = attempts.first();
    QVERIFY(!config->reconnectReady());
    discovery.update(inventory, enabled);
    QCOMPARE(attempts.size(), 1);
    QCOMPARE(discovery._configs.value(name), config);

    config->resetReconnectBackoff();
    auto claim = ports.reservePort(name);
    QVERIFY(claim);
    discovery.update(inventory, enabled);
    QCOMPARE(attempts.size(), 1);
    QVERIFY(config->reconnectReady());
    claim.reset();
    auto exclusion = ports.excludeFromAutoConnect(name);
    discovery.update(inventory, enabled);
    QCOMPARE(attempts.size(), 1);
    QVERIFY(config->reconnectReady());
    exclusion.reset();
    discovery.update(inventory, {});
    QCOMPARE(attempts.size(), 1);
    discovery.update(inventory, enabled);
    QCOMPARE(attempts.size(), 2);
    QCOMPARE(attempts.last(), config);

    config->resetReconnectBackoff();
    config->setSuppressAutoReconnect(true);
    discovery.update(inventory, enabled);
    QCOMPARE(attempts.size(), 2);
    discovery.update({}, enabled);
    QVERIFY(discovery._configs.isEmpty());
    QVERIFY(discovery._waitingPorts.isEmpty());
    discovery.update(inventory, enabled);
    QVERIFY(discovery._waitingPorts.contains(name));
    QVERIFY(discovery._configs.isEmpty());
}

UT_REGISTER_TEST(SerialAutoConnectTest, TestLabel::Unit, TestLabel::Comms)

void SerialAutoConnectTest::_replacementDeviceResetsConfiguration()
{
    SerialPortManager ports;
    QList<SharedLinkConfigurationPtr> attempts;
    SerialAutoConnect discovery(ports, [&](SharedLinkConfigurationPtr& config) { attempts.append(config); });
    SerialPortManager::Port port{QStringLiteral("/test/reused"), QStringLiteral("reused"),
                                 QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Controller")};
    port.physicalDeviceId = QStringLiteral("controller-A");
    const SerialAutoConnect::Options options{.pixhawk = true, .sikRadio = true};
    discovery.update({port}, options);
    discovery._waitingPorts._deadlinesUs[port.systemLocation] = 0;
    discovery.update({port}, options);
    QCOMPARE(attempts.size(), 1);
    attempts.first()->setSuppressAutoReconnect(true);
    discovery.update({port}, options);
    QCOMPARE(attempts.size(), 1);

    port.physicalDeviceId = QStringLiteral("radio-B");
    port.boardType = QGCSerialPortInfo::BoardTypeSiKRadio;
    discovery.update({port}, options);
    QVERIFY(!discovery._configs.contains(port.systemLocation));
    QVERIFY(discovery._waitingPorts.contains(port.systemLocation));
    discovery._waitingPorts._deadlinesUs[port.systemLocation] = 0;
    discovery.update({port}, options);
    QCOMPARE(attempts.size(), 2);
    QVERIFY(attempts.last() != attempts.first());
    QVERIFY(!attempts.last()->suppressAutoReconnect());
    auto* config = qobject_cast<SerialConfiguration*>(attempts.last().get());
    QVERIFY(config);
    QCOMPARE(config->baud(), 57600);
    QVERIFY(!config->usbDirect());
    QVERIFY(config->sikRadio());
    auto* pixhawkConfig = qobject_cast<SerialConfiguration*>(attempts.first().get());
    QVERIFY(pixhawkConfig);
    QVERIFY(!pixhawkConfig->sikRadio());
}

namespace {
class SerialDiscoveryTestLink : public LinkInterface
{
public:
    explicit SerialDiscoveryTestLink(SharedLinkConfigurationPtr& config)
        : LinkInterface(config)
    {}

    bool connected = false;

    bool isConnected() const override { return connected; }

    void disconnect() override { connected = false; }

private:
    bool _connect() override { return false; }

    void _writeBytes(const QByteArray&) override {}
};
}  // namespace

void SerialAutoConnectTest::_classification_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<QString>("reason");
    QTest::addColumn<QString>("setting");
    QTest::addColumn<QString>("board");
    QTest::newRow("pixhawk-off") << QStringLiteral("pixhawk-off") << QStringLiteral("class_disabled")
                                 << QStringLiteral("autoConnectPixhawk") << QStringLiteral("Pixhawk");
    QTest::newRow("sik-off") << QStringLiteral("sik-off") << QStringLiteral("class_disabled")
                             << QStringLiteral("autoConnectSiKRadio") << QStringLiteral("SiKRadio");
    QTest::newRow("openpilot-off") << QStringLiteral("openpilot-off") << QStringLiteral("class_disabled")
                                   << QStringLiteral("autoConnectLibrePilot") << QStringLiteral("OpenPilot");
    for (const auto& name : {"rtk", "unknown", "bootloader", "excluded", "secondary", "reserved", "exists",
                             "suppressed", "backoff", "settling", "backend", "empty"}) {
        const QString scenario = QString::fromLatin1(name);
        const QString reason = scenario == QStringLiteral("rtk") || scenario == QStringLiteral("unknown")
                                   ? QStringLiteral("unsupported_class")
                               : scenario == QStringLiteral("excluded")  ? QStringLiteral("routing_excluded")
                               : scenario == QStringLiteral("secondary") ? QStringLiteral("secondary_interface")
                               : scenario == QStringLiteral("reserved")  ? QStringLiteral("port_reserved")
                               : scenario == QStringLiteral("exists")    ? QStringLiteral("configuration_exists")
                               : scenario == QStringLiteral("backend")   ? QStringLiteral("backend_busy")
                               : scenario == QStringLiteral("empty")     ? QStringLiteral("invalid_port")
                                                                         : scenario;
        const QString board = scenario == QStringLiteral("rtk")       ? QStringLiteral("RTKGPS")
                              : scenario == QStringLiteral("unknown") ? QStringLiteral("Unknown")
                                                                      : QStringLiteral("Pixhawk");
        QTest::newRow(name) << scenario << reason << QStringLiteral("unknown") << board;
    }
}

void SerialAutoConnectTest::_classification()
{
    QFETCH(QString, scenario);
    QFETCH(QString, reason);
    QFETCH(QString, setting);
    QFETCH(QString, board);
    SerialPortManager ports;
    int attempts = 0;
    SerialAutoConnect discovery(ports, [&](SharedLinkConfigurationPtr&) { ++attempts; });
    SerialPortManager::Port port{QStringLiteral("/test/decision"), QStringLiteral("decision"),
                                 QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    SerialAutoConnect::Options options{.pixhawk = true, .sikRadio = true, .openPilot = true};
    if (scenario == QStringLiteral("pixhawk-off")) {
        options.pixhawk = false;
    } else if (scenario == QStringLiteral("sik-off")) {
        port.boardType = QGCSerialPortInfo::BoardTypeSiKRadio;
        options.sikRadio = false;
    } else if (scenario == QStringLiteral("openpilot-off")) {
        port.boardType = QGCSerialPortInfo::BoardTypeOpenPilot;
        options.openPilot = false;
    } else if (scenario == QStringLiteral("rtk")) {
        port.boardType = QGCSerialPortInfo::BoardTypeRTKGPS;
    } else if (scenario == QStringLiteral("unknown")) {
        port.boardType = QGCSerialPortInfo::BoardTypeUnknown;
    } else if (scenario == QStringLiteral("bootloader")) {
        port.bootloader = true;
    } else if (scenario == QStringLiteral("empty")) {
        port.systemLocation.clear();
    }
    SerialPortManager::ReservationPtr claim;
    if (scenario == QStringLiteral("excluded")) {
        claim = ports.excludeFromAutoConnect(port.systemLocation, SerialPortManager::OwnerKind::GPSReceiver,
                                             QStringLiteral("GNSS receiver"));
    } else if (scenario == QStringLiteral("reserved") || scenario == QStringLiteral("rtk")) {
        claim = ports.reservePort(port.systemLocation, SerialPortManager::OwnerKind::GPSReceiver,
                                  QStringLiteral("GNSS receiver"));
    } else if (scenario == QStringLiteral("backend")) {
        ports.setSinglePortOnly(true);
        claim = ports.reservePort(QStringLiteral("/test/other"));
    }
    SharedLinkConfigurationPtr config;
    std::shared_ptr<SerialDiscoveryTestLink> link;
    if (scenario == QStringLiteral("exists") || scenario == QStringLiteral("suppressed") ||
        scenario == QStringLiteral("backoff")) {
        config = std::make_shared<SerialConfiguration>(QStringLiteral("Saved"));
        discovery._configs.insert(port.systemLocation, config);
        if (scenario == QStringLiteral("exists")) {
            link = std::make_shared<SerialDiscoveryTestLink>(config);
            config->setLink(link);
        } else if (scenario == QStringLiteral("suppressed")) {
            config->setSuppressAutoReconnect(true);
        } else {
            config->noteReconnectAttempt();
        }
    }
    QList<SerialPortManager::Port> inventory{port};
    if (scenario == QStringLiteral("secondary")) {
        port.physicalDeviceId = QStringLiteral("composite");
        auto primary = port;
        primary.systemLocation = QStringLiteral("/test/primary");
        inventory = {primary, port};
    }
    ScopedConnectionLogCapture capture("Comms.Serial.SerialAutoConnect", "serial_auto_connect ");
    for (int tick = 0; tick < 4; ++tick) {
        if (scenario == QStringLiteral("backoff")) {
            config->noteReconnectAttempt();
        }
        discovery.update(inventory, options);
        for (const auto& candidate : inventory) {
            if (discovery._waitingPorts.contains(candidate.systemLocation)) {
                discovery._waitingPorts._deadlinesUs[candidate.systemLocation] = std::numeric_limits<quint64>::max();
            }
        }
        QCOMPARE(attempts, 0);
        QCOMPARE(discovery._configs.size(), config ? 1 : 0);
        QCOMPARE(discovery._waitingPorts.contains(port.systemLocation), scenario == QStringLiteral("settling"));
    }
    const auto lines = capture.messages();
    QCOMPARE(lines.size(), scenario == QStringLiteral("secondary") ? 2 : 1);
    if (scenario == QStringLiteral("secondary")) {
        QVERIFY(lines.first().startsWith(QStringLiteral("serial_auto_connect event=skip port=\"/test/primary\" ")));
        QVERIFY(lines.first().contains(QStringLiteral(" reason=settling setting=unknown ")));
    }
    const QString line = lines.last();
    QVERIFY(line.startsWith(QStringLiteral("serial_auto_connect event=skip port=%1 ")
                                .arg(scenario == QStringLiteral("empty") ? QStringLiteral("\"unknown\"")
                                                                         : QStringLiteral("\"/test/decision\""))));
    QVERIFY2(line.contains(QStringLiteral(" reason=%1 setting=%2 ").arg(reason, setting)), qPrintable(line));
    QVERIFY(line.contains(QStringLiteral(" board=%1 ").arg(board)));
    QVERIFY(line.contains(QStringLiteral(" vid=unknown pid=unknown ")));
    if (claim && scenario != QStringLiteral("backend")) {
        QVERIFY(line.endsWith(QStringLiteral(" owner=\"GNSS receiver\" owner_kind=GPSReceiver")));
    }
    if (config) {
        QCOMPARE(discovery._configs.value(port.systemLocation), config);
    }
}

void SerialAutoConnectTest::_skipTransitions()
{
    SerialPortManager ports;
    int attempts = 0;
    SerialAutoConnect discovery(ports, [&](SharedLinkConfigurationPtr&) { ++attempts; });
    SerialPortManager::Port port{QStringLiteral("/test/transition"), QStringLiteral("transition"),
                                 QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    port.description = QStringLiteral("USB \"board\"\\path\n\r\t");
    port.vendorIdentifier = 0x0403;
    port.productIdentifier = 0x6015;
    ScopedConnectionLogCapture capture("Comms.Serial.SerialAutoConnect", "serial_auto_connect ");
    discovery.update({port}, {});
    QCOMPARE(
        capture.messages(),
        QStringList{QStringLiteral("serial_auto_connect event=skip port=\"/test/transition\" name=\"transition\" "
                                   "description=\"USB \\\"board\\\"\\\\path\\n\\r\\t\" vid=0403 pid=6015 board=Pixhawk "
                                   "reason=class_disabled setting=autoConnectPixhawk config=\"unknown\" "
                                   "owner=\"unknown\" owner_kind=unknown")});
    for (int tick = 0; tick < 5; ++tick) {
        discovery.update({port}, {});
    }
    QCOMPARE(capture.messages().size(), 1);
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 2);
    QVERIFY(capture.messages().last().contains(QStringLiteral("reason=settling")));
    QVERIFY(discovery._waitingPorts.contains(port.systemLocation));
    auto claim = ports.reservePort(port.systemLocation, SerialPortManager::OwnerKind::GPSReceiver, QStringLiteral("A"));
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 3);
    QVERIFY(capture.messages().last().contains(QStringLiteral(" reason=port_reserved ")));
    QVERIFY(capture.messages().last().endsWith(QStringLiteral(" owner=\"A\" owner_kind=GPSReceiver")));
    claim.reset();
    claim = ports.reservePort(port.systemLocation, SerialPortManager::OwnerKind::GPSReceiver, QStringLiteral("B"));
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 4);
    QVERIFY(capture.messages().last().contains(QStringLiteral("owner=\"B\"")));
    claim.reset();
    port.physicalDeviceId = QStringLiteral("replacement");
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 6);
    QVERIFY(capture.messages()[4].endsWith(QStringLiteral("cause=identity_changed")));
    QVERIFY(capture.messages()[5].contains(QStringLiteral("reason=settling")));
    discovery.update({}, {});
    QCOMPARE(capture.messages().size(), 7);
    QVERIFY(capture.messages().last().endsWith(QStringLiteral("cause=removed")));
    QVERIFY(discovery._configs.isEmpty());
    QVERIFY(discovery._waitingPorts.isEmpty());
    QVERIFY(discovery._skipStates.isEmpty());
    discovery.update({}, {});
    QCOMPARE(capture.messages().size(), 7);
    discovery.update({port}, {});
    QCOMPARE(capture.messages().size(), 8);
    QVERIFY(capture.messages().last().contains(QStringLiteral(" reason=class_disabled setting=autoConnectPixhawk ")));
    QCOMPARE(attempts, 0);
}

void SerialAutoConnectTest::_retryDoesNotClearSkip()
{
    SerialPortManager ports;
    int attempts = 0;
    SerialAutoConnect discovery(ports, [&](SharedLinkConfigurationPtr&) { ++attempts; });
    const SerialPortManager::Port port{QStringLiteral("/test/retry"), QStringLiteral("retry"),
                                       QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    ScopedConnectionLogCapture capture("Comms.Serial.SerialAutoConnect", "serial_auto_connect ");
    discovery.update({port}, {.pixhawk = true});
    discovery._waitingPorts._deadlinesUs[port.systemLocation] = 0;
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(attempts, 1);
    QCOMPARE(capture.messages().size(), 1);
    QVERIFY(capture.messages().first().contains(QStringLiteral(" reason=settling ")));
    const auto config = discovery._configs.value(port.systemLocation);
    QVERIFY(config);
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 2);
    QVERIFY(capture.messages().last().contains(QStringLiteral("reason=backoff")));
    for (int attempt = 0; attempt < 3; ++attempt) {
        config->resetReconnectBackoff();
        discovery.update({port}, {.pixhawk = true});
        QCOMPARE(attempts, attempt + 2);
        discovery.update({port}, {.pixhawk = true});
        QCOMPARE(capture.messages().size(), 2);
        QCOMPARE(discovery._configs.value(port.systemLocation), config);
        QVERIFY(discovery._waitingPorts.isEmpty());
    }
}

void SerialAutoConnectTest::_verifiedConnection_data()
{
    QTest::addColumn<bool>("externalOwner");
    QTest::newRow("discovery-link") << false;
    QTest::newRow("saved-link-owner") << true;
}

void SerialAutoConnectTest::_verifiedConnection()
{
    QFETCH(bool, externalOwner);
    SerialPortManager ports;
    bool ownerConnected = false;
    int attempts = 0;
    SerialAutoConnect discovery(
        ports, [&](SharedLinkConfigurationPtr&) { ++attempts; },
        [&](const QString&) { return externalOwner && ownerConnected; });
    const SerialPortManager::Port port{QStringLiteral("/test/live"), QStringLiteral("live"),
                                       QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    SharedLinkConfigurationPtr config = std::make_shared<SerialConfiguration>(QStringLiteral("Live"));
    auto link = std::make_shared<SerialDiscoveryTestLink>(config);
    config->setLink(link);
    if (!externalOwner) {
        discovery._configs.insert(port.systemLocation, config);
    }
    auto claim = ports.reservePort(port.systemLocation, SerialPortManager::OwnerKind::MAVLink, config->name());
    QVERIFY(claim);
    ScopedConnectionLogCapture capture("Comms.Serial.SerialAutoConnect", "serial_auto_connect ");
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 1);
    QVERIFY(capture.messages().first().contains(QStringLiteral(" reason=port_reserved ")));
    link->connected = true;
    ownerConnected = true;
    for (int tick = 0; tick < 5; ++tick) {
        discovery.update({port}, {.pixhawk = true});
    }
    QCOMPARE(capture.messages().size(), 2);
    QCOMPARE(capture.messages().last(),
             QStringLiteral("serial_auto_connect event=clear port=\"/test/live\" cause=connected"));
    QVERIFY(discovery._skipStates.isEmpty());
    QCOMPARE(attempts, 0);
    QVERIFY(discovery._waitingPorts.isEmpty());
    // A settings change while the transport is live must not recreate a skip either.
    discovery.update({port}, {});
    QCOMPARE(capture.messages().size(), 2);
    link->connected = false;
    ownerConnected = false;
    discovery.update({port}, {.pixhawk = true});
    QCOMPARE(capture.messages().size(), 3);
    QVERIFY(capture.messages().last().contains(QStringLiteral(" reason=port_reserved ")));
}

void SerialAutoConnectTest::_gatePrecedence_data()
{
    QTest::addColumn<QString>("gate");
    QTest::addColumn<QString>("reason");
    QTest::newRow("disabled-before-bootloader") << QStringLiteral("disabled") << QStringLiteral("class_disabled");
    QTest::newRow("bootloader-before-exclusion") << QStringLiteral("bootloader") << QStringLiteral("bootloader");
    QTest::newRow("exclusion-before-primary") << QStringLiteral("excluded") << QStringLiteral("routing_excluded");
    QTest::newRow("reserved-primary-still-seen") << QStringLiteral("reserved") << QStringLiteral("port_reserved");
}

void SerialAutoConnectTest::_gatePrecedence()
{
    QFETCH(QString, gate);
    QFETCH(QString, reason);
    SerialPortManager ports;
    int attempts = 0;
    SerialAutoConnect discovery(ports, [&](SharedLinkConfigurationPtr&) { ++attempts; });
    SerialPortManager::Port first{QStringLiteral("/test/first"), QStringLiteral("first"),
                                  QGCSerialPortInfo::BoardTypePixhawk, QStringLiteral("Test")};
    first.physicalDeviceId = QStringLiteral("shared");
    auto second = first;
    second.systemLocation = QStringLiteral("/test/second");
    second.boardType = QGCSerialPortInfo::BoardTypeSiKRadio;
    const bool disabled = gate == QStringLiteral("disabled");
    first.bootloader = disabled || gate == QStringLiteral("bootloader");
    auto claim = gate == QStringLiteral("reserved") ? ports.reservePort(first.systemLocation)
                                                    : ports.excludeFromAutoConnect(first.systemLocation);
    QVERIFY(claim);
    const SerialAutoConnect::Options options{.pixhawk = !disabled, .sikRadio = true};
    ScopedConnectionLogCapture capture("Comms.Serial.SerialAutoConnect", "serial_auto_connect ");
    discovery.update({first, second}, options);
    QCOMPARE(capture.messages().size(), 2);
    QVERIFY(capture.messages().first().contains(QStringLiteral("reason=%1 ").arg(reason)));
    QVERIFY(capture.messages().last().contains(QStringLiteral(" reason=%1 ")
                                                   .arg(gate == QStringLiteral("reserved")
                                                            ? QStringLiteral("secondary_interface")
                                                            : QStringLiteral("settling"))));
    QVERIFY(!discovery._waitingPorts.contains(first.systemLocation));
    QCOMPARE(discovery._waitingPorts.contains(second.systemLocation), gate != QStringLiteral("reserved"));
    QCOMPARE(attempts, 0);
    QVERIFY(discovery._configs.isEmpty());
}
