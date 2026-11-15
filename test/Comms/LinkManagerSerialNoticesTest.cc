#include "LinkManagerTest.h"

#ifndef QGC_NO_SERIAL_LINK
#include <atomic>
#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QScopeGuard>
#include <QtCore/QThread>
#include <QtTest/QSignalSpy>

#include "AutoConnectSettings.h"
#include "LinkManager.h"
#include "QmlObjectListModel.h"
#include "SerialAutoConnect.h"
#include "SerialLink.h"
#include "SettingsManager.h"

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
class SerialNoticeTestLink : public LinkInterface
{
public:
    explicit SerialNoticeTestLink(SharedLinkConfigurationPtr& config)
        : LinkInterface(config)
    {}

    bool isConnected() const override { return _connected; }

    void markOpenWithoutSignal() { _connected = true; }

    void reportConnected()
    {
        _connected = true;
        emit connected();
    }

    void disconnect() override
    {
        _connected = false;
        emit disconnected();
    }

private:
    bool _connect() override { return false; }

    void _writeBytes(const QByteArray&) override {}

    bool _connected = false;
};

SerialPortManager::Port serialNoticeTestPort(const QString& path, QGCSerialPortInfo::BoardType_t board)
{
    SerialPortManager::Port port;
    port.systemLocation = path;
    port.portName = path;
    port.boardType = board;
    port.boardName = QStringLiteral("Radio");
    port.description = QStringLiteral("USB telemetry");
    return port;
}
}  // namespace

SharedLinkInterfacePtr LinkManagerTest::_serialNoticeLink(LinkManager& manager, SharedLinkConfigurationPtr& config)
{
    auto link = std::make_shared<SerialNoticeTestLink>(config);
    config->setLink(link);
    manager._rgLinks.append(link);
    manager._serialConnectionStates.insert(link.get(), {});
    (void) connect(link.get(), &LinkInterface::connected, &manager, &LinkManager::_linkConnected);
    (void) connect(link.get(), &LinkInterface::disconnected, &manager, &LinkManager::_linkDisconnected);
    return link;
}

void LinkManagerTest::_testSerialNoticeLoss_data()
{
    QTest::addColumn<QString>("policy");
    QTest::addColumn<QString>("reason");
    QTest::newRow("off") << QStringLiteral("off") << QStringLiteral("auto_connect_off");
    QTest::newRow("suppressed-unexpectedly") << QStringLiteral("suppressed") << QStringLiteral("suppressed");
    QTest::newRow("never-started") << QStringLiteral("never_started") << QStringLiteral("never_started");
    QTest::newRow("suspended") << QStringLiteral("connections_suspended") << QStringLiteral("connections_suspended");
    QTest::newRow("exclusion-does-not-gate-saved-retry") << QStringLiteral("routing_excluded") << QString();
    QTest::newRow("bootloader") << QStringLiteral("bootloader") << QStringLiteral("bootloader");
    QTest::newRow("eligible-awaits-threshold") << QStringLiteral("eligible") << QString();
    QTest::newRow("unverified") << QStringLiteral("unverified") << QString();
    QTest::newRow("explicit-config") << QStringLiteral("disconnect-config") << QString();
    QTest::newRow("explicit-link") << QStringLiteral("disconnect-link") << QString();
    QTest::newRow("disconnect-all") << QStringLiteral("disconnect-all") << QString();
    QTest::newRow("remove") << QStringLiteral("remove") << QString();
}

void LinkManagerTest::_testSerialNoticeLoss()
{
    QFETCH(QString, policy);
    QFETCH(QString, reason);
    LinkManager manager;
    auto port = serialNoticeTestPort(QStringLiteral("/test/notice-loss"), QGCSerialPortInfo::BoardTypeUnknown);
    manager._updateSerialNotices({port});
    auto* serial = new SerialConfiguration(QStringLiteral("Saved radio"));
    serial->setPortName(port.systemLocation);
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(policy != QStringLiteral("off") && policy != QStringLiteral("unverified"));
    config->setAutoConnectStarted(policy != QStringLiteral("never_started"));
    config->setSuppressAutoReconnect(policy == QStringLiteral("suppressed"));
    auto link = _serialNoticeLink(manager, config);
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    if (policy != QStringLiteral("unverified")) {
        static_cast<SerialNoticeTestLink*>(link.get())->reportConnected();
    }
    SerialPortManager::ReservationPtr exclusion;
    if (policy == QStringLiteral("connections_suspended")) {
        manager.setConnectionsSuspended(QStringLiteral("Firmware update"));
    } else if (policy == QStringLiteral("routing_excluded")) {
        exclusion = SerialPortManager::instance()->excludeFromAutoConnect(port.systemLocation);
    } else if (policy == QStringLiteral("bootloader")) {
        port.bootloader = true;
        manager._updateSerialNotices({port});
    }
    if (policy == QStringLiteral("disconnect-config")) {
        manager.disconnectLinkConfiguration(config.get());
    } else if (policy == QStringLiteral("disconnect-link")) {
        manager.disconnectLink(link.get());
    } else if (policy == QStringLiteral("disconnect-all")) {
        manager.disconnectAll();
    } else if (policy == QStringLiteral("remove")) {
        manager.removeConfiguration(config.get());
    } else {
        link->disconnect();
    }
    const int expected = reason.isEmpty() ? 0 : 1;
    QCOMPARE(manager.serialNotices()->count(), expected);
    QCOMPARE(raised.count(), expected);
    if (expected) {
        auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
        QCOMPARE(notice->reason(), reason);
        QCOMPARE(notice->source(), QStringLiteral("saved"));
        const QMap<QString, QString> remedies{
            {QStringLiteral("auto_connect_off"), QStringLiteral("Reattach the device")},
            {QStringLiteral("suppressed"), QStringLiteral("select Connect")},
            {QStringLiteral("never_started"), QStringLiteral("not been started this session")},
            {QStringLiteral("connections_suspended"), QStringLiteral("Firmware update")},
            {QStringLiteral("bootloader"), QStringLiteral("finish starting")}};
        QVERIFY(notice->text().contains(remedies.value(reason)));
        QVERIFY(notice->active());
        QVERIFY(!notice->acknowledged());
        QVERIFY(!notice->id().isEmpty());
        QVERIFY(notice->generation() > 0);
        QVERIFY(notice->text().startsWith(QStringLiteral("Saved radio, /test/notice-loss, USB telemetry\n")));
        QCOMPARE(notice->actions(), (QStringList{QStringLiteral("OpenCommLinks"), QStringLiteral("Dismiss")}));
        if (reason == QStringLiteral("auto_connect_off")) {
            QVERIFY(notice->text().contains(QStringLiteral("Automatic retry for this saved link is off.")));
            QVERIFY(notice->text().contains(QStringLiteral("Automatically Connect on Start, and Save.")));
            QVERIFY(!notice->text().contains(QStringLiteral("Automatic retry is pending.")));
        }
        for (int tick = 0; tick < 4; ++tick) {
            manager._updateSerialNotices({port});
        }
        QCOMPARE(raised.count(), 1);
        if (reason == QStringLiteral("auto_connect_off")) {
            manager._updateSerialNotices({});
            QCOMPARE(manager.serialNotices()->count(), 1);
            config->setAutoConnect(true);
            QCOMPARE(manager.serialNotices()->count(), 0);
            QCOMPARE(manager.serialNoticeHistory()->count(), 1);
        }
        manager.disconnectLinkConfiguration(config.get());
        QCOMPARE(manager.serialNotices()->count(), 0);
        QCOMPARE(manager.serialNoticeHistory()->count(), 0);
    }
    if (manager._rgLinkConfigs.contains(config)) {
        manager._removeConfiguration(config.get());
    }
}

void LinkManagerTest::_testSerialNoticeWorkerReasons_data()
{
    QTest::addColumn<int>("stage");
    QTest::addColumn<int>("nativeError");
    QTest::addColumn<QString>("reason");
    QTest::addColumn<QString>("remedy");
    QTest::newRow("permission") << int(SerialOpenOutcome::Stage::Open) << int(QSerialPort::PermissionError)
                                << QStringLiteral("access_denied") << QStringLiteral("serial-port permission");
    QTest::newRow("missing") << int(SerialOpenOutcome::Stage::Open) << int(QSerialPort::DeviceNotFoundError)
                             << QStringLiteral("device_not_found") << QStringLiteral("update Serial Port");
    QTest::newRow("other-open") << int(SerialOpenOutcome::Stage::Open) << int(QSerialPort::OpenError)
                                << QStringLiteral("open_failed") << QStringLiteral("Serial Port/Baud Rate");
    QTest::newRow("configuration") << int(SerialOpenOutcome::Stage::Configuration)
                                   << int(QSerialPort::UnsupportedOperationError)
                                   << QStringLiteral("configuration_failed") << QStringLiteral("Serial Port/Baud Rate");
    QTest::newRow("bootloader") << int(SerialOpenOutcome::Stage::Bootloader) << int(QSerialPort::NoError)
                                << QStringLiteral("bootloader") << QStringLiteral("finish starting");
}

void LinkManagerTest::_testSerialNoticeWorkerReasons()
{
    QFETCH(int, stage);
    QFETCH(int, nativeError);
    QFETCH(QString, reason);
    QFETCH(QString, remedy);
    LinkManager manager;
    const auto port = serialNoticeTestPort(QStringLiteral("/test/worker-notice"), QGCSerialPortInfo::BoardTypeUnknown);
    manager._updateSerialNotices({port});
    auto* serial = new SerialConfiguration(QStringLiteral("Attempted adapter"));
    serial->setPortName(port.systemLocation);
    serial->setDynamic(true);
    auto config = manager.addConfiguration(serial);
    const SerialOpenOutcome outcome{SerialOpenOutcome::Stage(stage), QSerialPort::SerialPortError(nativeError),
                                    QStringLiteral("native <detail> & permission"), true};
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    auto manualOutcome = outcome;
    manualOutcome.automatic = false;
    manager._serialOpenFailed(config.get(), manualOutcome);
    QCOMPARE(manager.serialNotices()->count(), 0);
    config->setAutoConnect(true);
    manager._serialOpenFailed(config.get(), manualOutcome);
    QCOMPARE(manager.serialNotices()->count(), 0);
    config->setSuppressAutoReconnect(true);
    manager._serialOpenFailed(config.get(), outcome);
    QCOMPARE(manager.serialNotices()->count(), 0);
    config->setSuppressAutoReconnect(false);
    for (int attempt = 0; attempt < 4; ++attempt) {
        manager._serialOpenFailed(config.get(), outcome);
    }
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(raised.count(), 1);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->reason(), reason);
    QCOMPARE(notice->source(), QStringLiteral("discovery"));
    QVERIFY(notice->text().contains(remedy));
    QVERIFY(notice->text().startsWith(QStringLiteral("Attempted adapter, /test/worker-notice, USB telemetry\n")));
    if (reason != QStringLiteral("bootloader")) {
        QVERIFY(notice->text().contains(outcome.errorString));
    }
    QCOMPARE(notice->actions(), (QStringList{QStringLiteral("OpenCommLinks"), QStringLiteral("Dismiss")}));
    manager._updateSerialNotices({});
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(manager.serialNoticeHistory()->count(), 0);
    manager._removeConfiguration(config.get());
}

void LinkManagerTest::_testSerialNoticeTransitions()
{
    QCOMPARE(LinkManager::SERIAL_UNAVAILABLE_DELAY_US, quint64(15'000'000));
    LinkManager manager;
    quint64 nowUs = 100;
    manager._serialNoticeClock = [&nowUs] { return nowUs; };
    const auto port = serialNoticeTestPort(QStringLiteral("/test/transitions"), QGCSerialPortInfo::BoardTypeUnknown);
    manager._updateSerialNotices({port});
    auto* serial = new SerialConfiguration(QStringLiteral("Retry radio"));
    serial->setPortName(port.systemLocation);
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                             QStringLiteral("native permission detail"), true});
    QCOMPARE(raised.count(), 1);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    const QString id = notice->id();
    const quint64 generation = notice->generation();
    QCOMPARE(notice->reason(), QStringLiteral("access_denied"));
    QVERIFY(notice->text().contains(QStringLiteral("native permission detail")));
    QVERIFY(notice->text().contains(QStringLiteral("serial-port permission")));
    manager.dismissSerialNotice(id);
    manager.dismissSerialNotice(id);
    manager.dismissSerialNotice(QStringLiteral("unknown-id"));
    QVERIFY(notice->active());
    QVERIFY(notice->acknowledged());
    for (int tick = 0; tick < 4; ++tick) {
        config->noteReconnectAttempt();
        manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                                 QStringLiteral("another native detail"), true});
        manager._updateSerialNotices({port});
    }
    QCOMPARE(raised.count(), 1);
    QVERIFY(notice->acknowledged());
    manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                             QStringLiteral("native missing detail"), true});
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(notice->id(), id);
    QCOMPARE(notice->generation(), generation);
    QCOMPARE(notice->reason(), QStringLiteral("device_not_found"));
    QCOMPARE(raised.count(), 2);
    QVERIFY(!notice->acknowledged());
    QVERIFY(notice->text().contains(QStringLiteral("update Serial Port, Save, then Connect.")));
    nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US - 1;
    manager._updateSerialNotices({port});
    QCOMPARE(raised.count(), 2);
    ++nowUs;
    manager._updateSerialNotices({port});
    QCOMPARE(raised.count(), 3);
    QCOMPARE(notice->reason(), QStringLiteral("still_unavailable"));
    manager.dismissSerialNotice(id);
    for (int tick = 0; tick < 4; ++tick) {
        nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US;
        manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                                 QStringLiteral("same missing device"), true});
        manager._updateSerialNotices({port});
    }
    QCOMPARE(raised.count(), 3);
    QVERIFY(notice->acknowledged());
    manager._updateSerialNotices({});
    manager._updateSerialNotices({port});
    manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                             QStringLiteral("same missing device after reattachment"), true});
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(manager.serialNotices()->value<SerialConnectionNotice*>(0)->id(), id);
    QCOMPARE(raised.count(), 3);
    auto link = _serialNoticeLink(manager, config);
    static_cast<SerialNoticeTestLink*>(link.get())->markOpenWithoutSignal();
    manager._serialConnected(config.get());
    QCOMPARE(manager.serialNotices()->count(), 1);
    // A queued open and a live link object are not proof of restoration.
    QVERIFY(!notice->text().contains(QStringLiteral("serial connection restored")));
    static_cast<SerialNoticeTestLink*>(link.get())->reportConnected();
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(manager.serialNoticeHistory()->count(), 1);
    QVERIFY(!notice->active());
    QVERIFY(notice->actions().isEmpty());
    QCOMPARE(notice->text(), QStringLiteral("Retry radio: serial connection restored."));
    QCOMPARE(raised.count(), 3);
    manager.disconnectLink(link.get());
    manager._removeConfiguration(config.get());
}

void LinkManagerTest::_testSerialNoticeEscalationAfterLoss()
{
    LinkManager manager;
    quint64 nowUs = 10;
    manager._serialNoticeClock = [&nowUs] { return nowUs; };
    auto* serial = new SerialConfiguration(QStringLiteral("Missing saved radio"));
    serial->setPortName(QStringLiteral("/test/still-missing"));
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    auto link = _serialNoticeLink(manager, config);
    static_cast<SerialNoticeTestLink*>(link.get())->reportConnected();
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    link->disconnect();
    nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US - 1;
    manager._updateSerialNotices({});
    QCOMPARE(raised.count(), 0);
    ++nowUs;
    manager._updateSerialNotices({});
    QCOMPARE(raised.count(), 1);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->reason(), QStringLiteral("still_unavailable"));
    QVERIFY(notice->text().contains(QStringLiteral("/test/still-missing")));
    for (int tick = 0; tick < 4; ++tick) {
        nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US;
        manager._updateSerialNotices({});
    }
    QCOMPARE(raised.count(), 1);
    manager._removeConfiguration(config.get());
    QCOMPARE(manager.serialNotices()->count(), 0);
}

void LinkManagerTest::_testSerialNoticeEscalationAfterCreationFailure()
{
    LinkManager manager;
    quint64 nowUs = 1;
    manager._serialNoticeClock = [&nowUs] { return nowUs; };
    auto* serial = new SerialConfiguration(QStringLiteral("Unnamed reservation"));
    serial->setPortName(QStringLiteral("/test/creation-deadline"));
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    const auto claim = SerialPortManager::instance()->reservePort(serial->portName());
    QVERIFY(claim);
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    QVERIFY(!manager.createConnectedLink(config));
    QCOMPARE(raised.count(), 0);
    nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US - 1;
    manager._updateSerialNotices({});
    QCOMPARE(raised.count(), 0);
    ++nowUs;
    manager._updateSerialNotices({});
    QCOMPARE(raised.count(), 1);
    QCOMPARE(manager.serialNotices()->value<SerialConnectionNotice*>(0)->reason(), QStringLiteral("still_unavailable"));
    QVERIFY(!manager.createConnectedLink(config));
    manager._updateSerialNotices({});
    QCOMPARE(raised.count(), 1);
    manager._removeConfiguration(config.get());
}

void LinkManagerTest::_testSerialNoticeDiscovery_data()
{
    QTest::addColumn<int>("board");
    QTest::addColumn<QString>("label");
    QTest::newRow("pixhawk") << int(QGCSerialPortInfo::BoardTypePixhawk)
                             << QStringLiteral("Automatically connect to a Pixhawk board");
    QTest::newRow("sik") << int(QGCSerialPortInfo::BoardTypeSiKRadio)
                         << QStringLiteral("Automatically connect to a SiK Radio");
    QTest::newRow("openpilot") << int(QGCSerialPortInfo::BoardTypeOpenPilot)
                               << QStringLiteral("Automatically connect to a LibrePilot");
}

void LinkManagerTest::_testSerialNoticeDiscovery()
{
    QFETCH(int, board);
    QFETCH(QString, label);
    LinkManager manager;
    manager.init();
    auto* settings = SettingsManager::instance()->autoConnectSettings();
    const QList<Fact*> switches{settings->autoConnectPixhawk(), settings->autoConnectSiKRadio(),
                                settings->autoConnectLibrePilot()};
    QList<QVariant> oldValues;
    for (auto* setting : switches) {
        oldValues.append(setting->rawValue());
        setting->setRawValue(false);
    }
    const auto restore = qScopeGuard([&] {
        for (qsizetype index = 0; index < switches.size(); ++index) {
            switches[index]->setRawValue(oldValues[index]);
        }
    });
    auto port = serialNoticeTestPort(QStringLiteral("/test/class-off"), QGCSerialPortInfo::BoardType_t(board));
    port.physicalDeviceId = QStringLiteral("one-composite-device");
    auto secondary = port;
    secondary.systemLocation = QStringLiteral("/test/class-off-secondary");
    auto unknown = serialNoticeTestPort(QStringLiteral("/test/unknown"), QGCSerialPortInfo::BoardTypeUnknown);
    auto rtk = serialNoticeTestPort(QStringLiteral("/test/rtk"), QGCSerialPortInfo::BoardTypeRTKGPS);
    const auto gpsClaim = SerialPortManager::instance()->reservePort(
        rtk.systemLocation, SerialPortManager::OwnerKind::GPSReceiver, QStringLiteral("GNSS receiver"));
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    for (int tick = 0; tick < 4; ++tick) {
        manager._updateSerialNotices({port, secondary, unknown, rtk});
    }
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(raised.count(), 1);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->source(), QStringLiteral("discovery"));
    QCOMPARE(notice->reason(), QStringLiteral("class_disabled"));
    QVERIFY(notice->text().contains(u'\'' + label + u'\''));
    QVERIFY(notice->text().contains(QStringLiteral("Application Settings > Comm Links > AutoConnect")));
    QCOMPARE(notice->actions(), (QStringList{QStringLiteral("OpenCommLinks"), QStringLiteral("Dismiss")}));
    const QString oldId = notice->id();
    const quint64 oldGeneration = notice->generation();
    manager.dismissSerialNotice(oldId);
    manager._updateSerialNotices({port, secondary});
    QCOMPARE(raised.count(), 1);
    auto another = port;
    another.systemLocation = QStringLiteral("/test/another-radio");
    another.physicalDeviceId = QStringLiteral("another-device");
    manager._updateSerialNotices({port, secondary, another});
    QCOMPARE(manager.serialNotices()->count(), 2);
    QCOMPARE(raised.count(), 2);
    manager._updateSerialNotices({another});
    QCOMPARE(manager.serialNotices()->count(), 1);
    manager._updateSerialNotices({port, another});
    QCOMPARE(manager.serialNotices()->count(), 2);
    auto* reattached = manager.serialNotices()->value<SerialConnectionNotice*>(1);
    QVERIFY(reattached->id() != oldId);
    QVERIFY(reattached->generation() != oldGeneration);
    QCOMPARE(raised.count(), 3);
    // Same location with a different physical identity starts a new attachment as well.
    port.physicalDeviceId = QStringLiteral("replacement-device");
    manager._updateSerialNotices({port, another});
    QCOMPARE(raised.count(), 4);
    for (auto* setting : switches) {
        setting->setRawValue(true);
    }
    manager._updateSerialNotices({port, another});
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(manager.serialNoticeHistory()->count(), 2);
    manager._updateSerialNotices({});
    QCOMPARE(manager.serialNoticeHistory()->count(), 0);
    for (auto* setting : switches) {
        setting->setRawValue(false);
    }
    auto receiverPort = port;
    receiverPort.systemLocation = QStringLiteral("/test/receiver-on-vehicle-class");
    const auto receiverClaim = SerialPortManager::instance()->reservePort(
        receiverPort.systemLocation, SerialPortManager::OwnerKind::GPSReceiver, QStringLiteral("GNSS receiver"));
    QVERIFY(receiverClaim);
    manager._updateSerialNotices({receiverPort});
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(raised.count(), 4);
}

void LinkManagerTest::_testSerialNoticeOwners_data()
{
    QTest::addColumn<bool>("gps");
    QTest::newRow("mavlink") << false;
    QTest::newRow("gnss") << true;
}

void LinkManagerTest::_testSerialNoticeOwners()
{
    QFETCH(bool, gps);
    LinkManager manager;
    quint64 nowUs = 1;
    manager._serialNoticeClock = [&nowUs] { return nowUs; };
    const auto port = serialNoticeTestPort(QStringLiteral("/test/owned-notice"), QGCSerialPortInfo::BoardTypeSiKRadio);
    manager._updateSerialNotices({port});
    auto* serial = new SerialConfiguration(QStringLiteral("Intended telemetry"));
    serial->setPortName(port.systemLocation);
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    const auto kind = gps ? SerialPortManager::OwnerKind::GPSReceiver : SerialPortManager::OwnerKind::MAVLink;
    auto claim = SerialPortManager::instance()->reservePort(port.systemLocation, kind, QStringLiteral("Owner A"));
    QVERIFY(claim);
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    for (int attempt = 0; attempt < 4; ++attempt) {
        QVERIFY(!manager.createConnectedLink(config));
    }
    QCOMPARE(raised.count(), 1);
    QCOMPARE(manager.serialNotices()->count(), 1);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->reason(), QStringLiteral("reservation_failed"));
    QVERIFY(notice->text().contains(QStringLiteral("Owner A")));
    const QString id = notice->id();
    QCOMPARE(notice->actions(), gps ? (QStringList{QStringLiteral("OpenGnssReceiver"), QStringLiteral("OpenCommLinks"),
                                                   QStringLiteral("Dismiss")})
                                    : (QStringList{QStringLiteral("OpenCommLinks"), QStringLiteral("Dismiss")}));
    const QStringList originalActions = notice->actions();
    manager.dismissSerialNotice(id);
    claim.reset();
    claim = SerialPortManager::instance()->reservePort(port.systemLocation, kind, QStringLiteral("Owner B"));
    QVERIFY(claim);
    QVERIFY(!manager.createConnectedLink(config));
    QCOMPARE(raised.count(), 2);
    QCOMPARE(notice->id(), id);
    QVERIFY(!notice->acknowledged());
    QVERIFY(notice->text().contains(QStringLiteral("Owner B")));
    nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US;
    manager._updateSerialNotices({port});
    QCOMPARE(notice->reason(), QStringLiteral("still_unavailable"));
    QCOMPARE(notice->actions(), originalActions);
    QVERIFY(notice->text().contains(QStringLiteral("Owner B")));
    QCOMPARE(raised.count(), 3);
    for (int attempt = 0; attempt < 4; ++attempt) {
        QVERIFY(!manager.createConnectedLink(config));
        manager._updateSerialNotices({port});
    }
    QCOMPARE(raised.count(), 3);
    QCOMPARE(notice->reason(), QStringLiteral("still_unavailable"));
    manager.removeConfiguration(config.get());
    QCOMPARE(manager.serialNotices()->count(), 0);
}

void LinkManagerTest::_testSerialNoticeSuppression()
{
    LinkManager manager;
    auto* serial = new SerialConfiguration(QStringLiteral("GNSS device"));
    serial->setPortName(QStringLiteral("/test/rtk-no-notice"));
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    const auto rtk = serialNoticeTestPort(serial->portName(), QGCSerialPortInfo::BoardTypeRTKGPS);
    manager._updateSerialNotices({rtk});
    auto claim = SerialPortManager::instance()->reservePort(
        serial->portName(), SerialPortManager::OwnerKind::GPSReceiver, QStringLiteral("Expected RTK"));
    QVERIFY(!manager.createConnectedLink(config));
    manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                             QStringLiteral("not a vehicle link"), true});
    manager._serialLost(config.get(), LinkManager::AutoConnectReason::AutoConnectOff);
    QCOMPARE(manager.serialNotices()->count(), 0);
    manager._removeConfiguration(config.get());
    claim.reset();

    auto primary = serialNoticeTestPort(QStringLiteral("/test/notice-primary"), QGCSerialPortInfo::BoardTypePixhawk);
    primary.physicalDeviceId = QStringLiteral("notice-composite");
    auto secondary = primary;
    secondary.systemLocation = QStringLiteral("/test/notice-secondary");
    manager._updateSerialNotices({primary, secondary});
    auto* secondarySerial = new SerialConfiguration(QStringLiteral("Secondary interface"));
    secondarySerial->setPortName(secondary.systemLocation);
    secondarySerial->setAutoConnect(true);
    secondarySerial->setAutoConnectStarted(true);
    config = manager.addConfiguration(secondarySerial);
    manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                             QStringLiteral("secondary interface"), true});
    manager._serialLost(config.get(), LinkManager::AutoConnectReason::AutoConnectOff);
    QCOMPARE(manager.serialNotices()->count(), 0);
    manager._removeConfiguration(config.get());

    manager.init();
    auto port = serialNoticeTestPort(QStringLiteral("/test/verified-notice"), QGCSerialPortInfo::BoardTypePixhawk);
    auto* vehicleSerial = new SerialConfiguration(QStringLiteral("Verified owner"));
    vehicleSerial->setPortName(port.systemLocation);
    config = manager.addConfiguration(vehicleSerial);
    auto link = _serialNoticeLink(manager, config);
    const QVariant oldValue = SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->rawValue();
    const auto restore = qScopeGuard(
        [&] { SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->setRawValue(oldValue); });
    SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->setRawValue(false);
    manager._updateSerialNotices({port});
    QCOMPARE(manager.serialNotices()->count(), 1);
    manager._serialConnected(config.get());
    QCOMPARE(manager.serialNotices()->count(), 1);
    static_cast<SerialNoticeTestLink*>(link.get())->reportConnected();
    QCOMPARE(manager.serialNotices()->count(), 0);
    claim = SerialPortManager::instance()->reservePort(port.systemLocation, SerialPortManager::OwnerKind::MAVLink,
                                                       QStringLiteral("Verified owner"));
    QVERIFY(claim);
    auto* satisfied = new SerialConfiguration(QStringLiteral("Satisfied discovery"));
    satisfied->setPortName(port.systemLocation);
    satisfied->setAutoConnect(true);
    satisfied->setDynamic(true);
    auto satisfiedConfig = manager.addConfiguration(satisfied);
    QVERIFY(!manager.createConnectedLink(satisfiedConfig));
    QCOMPARE(manager.serialNotices()->count(), 0);
    manager._removeConfiguration(satisfiedConfig.get());
    manager.disconnectLink(link.get());
    manager._removeConfiguration(config.get());
    claim.reset();

    // Enabled discovery in normal settling and backoff produces no incident.
    SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->setRawValue(true);
    manager._serialAutoConnect->update({port}, {.pixhawk = true});
    manager._updateSerialNotices({port});
    QCOMPARE(manager.serialNotices()->count(), 0);
    auto retry = std::make_shared<SerialConfiguration>(QStringLiteral("Ordinary backoff"));
    retry->setPortName(port.systemLocation);
    retry->setAutoConnect(true);
    retry->setDynamic(true);
    retry->noteReconnectAttempt();
    manager._serialAutoConnect->_configs.insert(port.systemLocation, retry);
    manager._serialAutoConnect->update({port}, {.pixhawk = true});
    manager._updateSerialNotices({port});
    QCOMPARE(manager.serialNotices()->count(), 0);
}

void LinkManagerTest::_testSerialNoticePeriodicObservation()
{
    LinkManager manager;
    manager.init();
    auto* settings = SettingsManager::instance()->autoConnectSettings();
    const QList<Fact*> switches{settings->autoConnectPixhawk(), settings->autoConnectSiKRadio(),
                                settings->autoConnectLibrePilot()};
    QList<QVariant> oldValues;
    for (auto* setting : switches) {
        oldValues.append(setting->rawValue());
        setting->setRawValue(true);
    }
    const auto restore = qScopeGuard([&] {
        for (qsizetype index = 0; index < switches.size(); ++index) {
            switches[index]->setRawValue(oldValues[index]);
        }
    });
    settings->autoConnectPixhawk()->setRawValue(false);
    manager._updateSerialNotices(
        {serialNoticeTestPort(QStringLiteral("/test/s2-periodic-absent"), QGCSerialPortInfo::BoardTypePixhawk)});
    QCOMPARE(manager.serialNotices()->count(), 1);
    settings->autoConnectPixhawk()->setRawValue(true);
    manager.setConnectionsSuspended(QStringLiteral("Periodic notice observation"));
    manager._updateAutoConnectLinks();
    QCOMPARE(manager.serialNotices()->count(), 0);
}

void LinkManagerTest::_testSerialNoticeSavedPrecedence()
{
    LinkManager manager;
    manager.init();
    auto* setting = SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk();
    const auto oldValue = setting->rawValue();
    const auto restore = qScopeGuard([&] { setting->setRawValue(oldValue); });
    setting->setRawValue(false);
    const auto port =
        serialNoticeTestPort(QStringLiteral("/test/saved-precedence"), QGCSerialPortInfo::BoardTypePixhawk);
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    manager._updateSerialNotices({port});
    QCOMPARE(raised.count(), 1);
    auto* saved = new SerialConfiguration(QStringLiteral("Saved intended link"));
    saved->setPortName(port.systemLocation);
    saved->setAutoConnect(true);
    saved->setAutoConnectStarted(true);
    auto savedConfig = manager.addConfiguration(saved);
    const SerialOpenOutcome outcome{SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                    QStringLiteral("native permission"), true};
    manager._serialOpenFailed(savedConfig.get(), outcome);
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(raised.count(), 2);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->source(), QStringLiteral("saved"));
    auto* discovered = new SerialConfiguration(QStringLiteral("Discovery candidate"));
    discovered->setPortName(port.systemLocation);
    discovered->setAutoConnect(true);
    discovered->setDynamic(true);
    auto discoveredConfig = manager.addConfiguration(discovered);
    manager._serialOpenFailed(discoveredConfig.get(), outcome);
    manager._updateSerialNotices({port});
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(raised.count(), 2);
    QCOMPARE(manager.serialNotices()->value<SerialConnectionNotice*>(0)->id(), notice->id());
    manager.disconnectLinkConfiguration(savedConfig.get());
    manager._updateSerialNotices({port});
    QCOMPARE(manager.serialNotices()->count(), 0);
    manager._removeConfiguration(savedConfig.get());
    manager._removeConfiguration(discoveredConfig.get());
}

void LinkManagerTest::_testSerialNoticeConfigurationLifetime()
{
    LinkManager manager;
    auto config = std::make_shared<SerialConfiguration>(QStringLiteral("Externally owned configuration"));
    config->setPortName(QStringLiteral("/test/notice-lifetime"));
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                             QStringLiteral("missing"), true});
    QCOMPARE(manager.serialNotices()->count(), 1);
    config.reset();
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(manager.serialNoticeHistory()->count(), 0);
}

void LinkManagerTest::_testSerialNoticeStaleDiscovery()
{
    LinkManager manager;
    auto port = serialNoticeTestPort(QStringLiteral("/test/discovery-generation"), QGCSerialPortInfo::BoardTypeUnknown);
    port.physicalDeviceId = QStringLiteral("old-device");
    manager._updateSerialNotices({port});
    auto* oldSerial = new SerialConfiguration(QStringLiteral("Old device"));
    oldSerial->setPortName(port.systemLocation);
    oldSerial->setDynamic(true);
    oldSerial->setAutoConnect(true);
    auto oldConfig = manager.addConfiguration(oldSerial);
    const SerialOpenOutcome outcome{SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                    QStringLiteral("native permission"), true};
    manager._serialOpenFailed(oldConfig.get(), outcome);
    const auto oldGeneration = manager.serialNotices()->value<SerialConnectionNotice*>(0)->generation();
    manager._updateSerialNotices({});
    manager._serialOpenFailed(oldConfig.get(), outcome);
    QCOMPARE(manager.serialNotices()->count(), 0);
    port.physicalDeviceId = QStringLiteral("new-device");
    manager._updateSerialNotices({port});
    auto* newSerial = new SerialConfiguration(QStringLiteral("New device"));
    newSerial->setPortName(port.systemLocation);
    newSerial->setDynamic(true);
    newSerial->setAutoConnect(true);
    auto newConfig = manager.addConfiguration(newSerial);
    manager._serialOpenFailed(newConfig.get(), outcome);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QVERIFY(notice->generation() != oldGeneration);
    QVERIFY(notice->text().contains(QStringLiteral("New device")));
    manager._serialOpenFailed(oldConfig.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                                QStringLiteral("stale worker result"), true});
    QCOMPARE(manager.serialNotices()->count(), 1);
    QCOMPARE(notice->reason(), QStringLiteral("access_denied"));
    QVERIFY(!notice->text().contains(QStringLiteral("stale worker result")));
    auto lateLink = _serialNoticeLink(manager, oldConfig);
    static_cast<SerialNoticeTestLink*>(lateLink.get())->reportConnected();
    QCOMPARE(manager.serialNotices()->count(), 1);
    manager.disconnectLink(lateLink.get());
    manager._removeConfiguration(oldConfig.get());
    oldConfig.reset();
    QCOMPARE(manager.serialNotices()->count(), 1);
    manager._removeConfiguration(newConfig.get());
    QCOMPARE(manager.serialNotices()->count(), 0);
}

void LinkManagerTest::_testSerialNoticePolicyTransitions()
{
    LinkManager manager;
    auto* serial = new SerialConfiguration(QStringLiteral("Policy transition"));
    serial->setPortName(QStringLiteral("/test/policy-transition"));
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    auto link = _serialNoticeLink(manager, config);
    static_cast<SerialNoticeTestLink*>(link.get())->reportConnected();
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    manager.setConnectionsSuspended(QStringLiteral("Firmware update"));
    link->disconnect();
    QCOMPARE(raised.count(), 1);
    auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    const QString id = notice->id();
    manager.dismissSerialNotice(id);
    config->setAutoConnect(false);
    QCOMPARE(raised.count(), 2);
    QCOMPARE(notice->id(), id);
    QCOMPARE(notice->reason(), QStringLiteral("auto_connect_off"));
    QVERIFY(!notice->acknowledged());
    config->setAutoConnect(true);
    QCOMPARE(raised.count(), 3);
    QCOMPARE(notice->reason(), QStringLiteral("connections_suspended"));
    manager.setConnectionsAllowed();
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(manager.serialNoticeHistory()->count(), 1);
    QVERIFY(notice->text().contains(QStringLiteral("policy now permits automatic retry")));
    config->setSuppressAutoReconnect(true);
    manager._updateSerialNotices({});
    QCOMPARE(raised.count(), 4);
    QCOMPARE(notice->reason(), QStringLiteral("suppressed"));
    config->setSuppressAutoReconnect(false);
    manager._updateSerialNotices({});
    QCOMPARE(manager.serialNotices()->count(), 0);
    manager._removeConfiguration(config.get());
}

void LinkManagerTest::_testSerialNoticeHistoryCap()
{
    QCOMPARE(LinkManager::SERIAL_NOTICE_HISTORY_CAP, 32);
    LinkManager manager;
    QList<SharedLinkConfigurationPtr> configs;
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    for (int index = 0; index < LinkManager::SERIAL_NOTICE_HISTORY_CAP + 3; ++index) {
        auto* serial = new SerialConfiguration(QStringLiteral("Radio %1").arg(index));
        serial->setPortName(QStringLiteral("/test/cap-%1").arg(index));
        auto config = manager.addConfiguration(serial);
        config->setAutoConnect(true);
        config->setAutoConnectStarted(true);
        configs.append(config);
        manager._serialOpenFailed(config.get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                                 QStringLiteral("missing"), true});
    }
    QCOMPARE(manager.serialNotices()->count(), LinkManager::SERIAL_NOTICE_HISTORY_CAP + 3);
    const QString evictedId = manager.serialNotices()->value<SerialConnectionNotice*>(1)->id();
    for (qsizetype index = 1; index < configs.size(); ++index) {
        auto& config = configs[index];
        {
            auto link = _serialNoticeLink(manager, config);
            static_cast<SerialNoticeTestLink*>(link.get())->reportConnected();
            // Keep the resolved configuration for history; intentional raw cleanup avoids a new loss.
            manager._serialConnectionStates.remove(link.get());
            manager._rgLinks.removeOne(link);
            config->setLink(nullptr);
        }
    }
    QCOMPARE(manager.serialNoticeHistory()->count(), LinkManager::SERIAL_NOTICE_HISTORY_CAP);
    QCOMPARE(manager.serialNotices()->count(), 1);
    auto* active = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QVERIFY(active->text().contains(QStringLiteral("Radio 0")));
    QVERIFY(active->active());
    QCOMPARE(raised.count(), LinkManager::SERIAL_NOTICE_HISTORY_CAP + 3);
    manager._serialOpenFailed(configs[1].get(), {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError,
                                                 QStringLiteral("missing again"), true});
    QCOMPARE(manager.serialNotices()->count(), 2);
    QCOMPARE(manager.serialNotices()->value<SerialConnectionNotice*>(1)->id(), evictedId);
    QCOMPARE(manager.serialNoticeHistory()->count(), LinkManager::SERIAL_NOTICE_HISTORY_CAP);
    for (auto& config : configs) {
        manager._removeConfiguration(config.get());
    }
    QCOMPARE(manager.serialNotices()->count(), 0);
    QCOMPARE(manager.serialNoticeHistory()->count(), 0);
}

void LinkManagerTest::_testSerialNoticeAutomaticOpen_data()
{
    QTest::addColumn<bool>("occupied");
    QTest::addColumn<bool>("changePolicy");
    QTest::newRow("access-denied") << true << false;
    QTest::newRow("device-not-found") << false << false;
    QTest::newRow("automatic-attempt-policy-changed") << false << true;
}

void LinkManagerTest::_testSerialNoticeAutomaticOpen()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QFETCH(bool, occupied);
    QFETCH(bool, changePolicy);
    LinkManager manager;
    QString portName = QStringLiteral("/test/notice-absent-device");
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    QVERIFY(master >= 0);
    const auto closeMaster = qScopeGuard([master] { ::close(master); });
    QCOMPARE(::grantpt(master), 0);
    QCOMPARE(::unlockpt(master), 0);
    const char* slaveName = ::ptsname(master);
    QVERIFY(slaveName);
    QSerialPort blocker(QString::fromLocal8Bit(slaveName));
    if (occupied) {
        QVERIFY(blocker.open(QIODevice::ReadWrite));
        portName = blocker.portName();
    }
    auto* serial = new SerialConfiguration(QStringLiteral("Automatic attempt"));
    serial->setPortName(portName);
    auto config = manager.addConfiguration(serial);
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    const auto cleanup = qScopeGuard([&] { manager._removeConfiguration(config.get()); });
    QSignalSpy raised(&manager, &LinkManager::serialNoticeRaised);
    QVERIFY(manager.createConnectedLink(config));
    auto* serialLink = qobject_cast<SerialLink*>(config->link());
    QVERIFY(serialLink);
    std::atomic<bool> forwardedOnApplicationThread{false};
    const auto outcomeConnection = connect(
        serialLink, &SerialLink::openFailed, &manager,
        [&manager, &forwardedOnApplicationThread](const SerialOpenOutcome&) {
            forwardedOnApplicationThread.store(QThread::currentThread() == manager.thread());
        },
        Qt::DirectConnection);
    const auto disconnectOutcome = qScopeGuard([outcomeConnection] { (void) QObject::disconnect(outcomeConnection); });
    if (changePolicy) {
        config->setAutoConnect(false);
    }
    QTRY_COMPARE_WITH_TIMEOUT(raised.count(), 1, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(!config->link(), TestTimeout::shortMs());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(raised.count(), 1);
    QVERIFY(forwardedOnApplicationThread.load());
    QCOMPARE(manager.serialNotices()->count(), 1);
    const auto* notice = manager.serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->reason(), occupied ? QStringLiteral("access_denied") : QStringLiteral("device_not_found"));
    QVERIFY(notice->text().contains(QStringLiteral("Automatic attempt")));
    QCOMPARE(notice->text().contains(QStringLiteral("Automatic retry is pending.")), !changePolicy);
    QVERIFY(manager.serialNoticeHistory()->isEmpty());
#else
    QSKIP("Automatic serial open outcome integration requires a Linux pseudo-terminal");
#endif
}

void LinkManagerTest::_testSerialNoticeUiVocabulary()
{
    const auto readSource = [](const char* relativePath) {
        QFile file(QFINDTESTDATA(relativePath));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto links = readSource("../../src/AppSettings/LinkConfigurationManager.qml");
    QVERIFY(!links.isEmpty());
    for (const auto& label : {"Links", "Automatically Connect on Start", "Connect", "Disconnect"}) {
        QVERIFY2(links.contains(QByteArray("qsTr(\"") + label + "\")"), label);
    }
    QVERIFY(links.contains("/res/pencil.svg"));
    QVERIFY(links.contains("Dialog.Save"));
    const auto serial = readSource("../../src/AppSettings/SerialSettings.qml");
    QVERIFY(serial.contains("qsTr(\"Serial Port\")"));
    QVERIFY(serial.contains("qsTr(\"Baud Rate\")"));
    QVERIFY(readSource("../../src/MainWindow/MainWindow.qml").contains("qsTr(\"Application Settings\")"));
    const auto pages = readSource("../../src/AppSettings/pages/SettingsPages.json");
    QVERIFY(pages.contains("\"name\": \"Comm Links\""));
    QVERIFY(pages.contains("\"name\": \"GNSS Receiver\""));
    QVERIFY(
        readSource("../../src/AppSettings/pages/CommLinks.SettingsUI.json").contains("\"heading\": \"AutoConnect\""));
    QVERIFY(readSource("../../src/GPS/Qml/GPSReceiverSettings.qml").contains("qsTr(\"Disconnect\")"));
    const auto metadata = QJsonDocument::fromJson(readSource("../../src/Settings/AutoConnect.SettingsGroup.json"));
    const auto facts = metadata.object().value(QStringLiteral("QGC.MetaData.Facts")).toArray();
    const QMap<QString, QString> expected{
        {QStringLiteral("autoConnectPixhawk"), QStringLiteral("Automatically connect to a Pixhawk board")},
        {QStringLiteral("autoConnectSiKRadio"), QStringLiteral("Automatically connect to a SiK Radio")},
        {QStringLiteral("autoConnectLibrePilot"), QStringLiteral("Automatically connect to a LibrePilot")}};
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        bool matched = false;
        for (const auto& fact : facts) {
            const auto object = fact.toObject();
            if (object.value(QStringLiteral("name")).toString() == it.key()) {
                QCOMPARE(object.value(QStringLiteral("label")).toString(), it.value());
                matched = true;
            }
        }
        QVERIFY2(matched, qPrintable(it.key()));
    }
}
#endif
