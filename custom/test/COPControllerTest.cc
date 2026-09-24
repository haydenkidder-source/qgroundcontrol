#include "COPControllerTest.h"

#include <QtCore/QSettings>

#include "AppSettings.h"
#include "COPController.h"
#include "COPReferencePoint.h"
#include "LinkManager.h"
#include "RTCMFramer.h"
#include "SettingsManager.h"
#include "VehicleLinkManager.h"

void COPControllerTest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void COPControllerTest::cleanup()
{
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    UnitTest::cleanup();
}

void COPControllerTest::_rolesAndOrder()
{
    QSettings settings;
    settings.remove(QStringLiteral("COP"));
    VehicleRoleController roles;
    roles.addEntry(202, QStringLiteral("Rover"), QStringLiteral("North"), 0);
    roles.addEntry(201, QStringLiteral("Hex"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);
    controller.selectVehicle(202);
    QVERIFY(controller.selected());
    QCOMPARE(controller.selected()->label(), QStringLiteral("Rover · North"));
    const auto order = settings.value(QStringLiteral("COP/VehicleOrder")).toList();
    QVERIFY(order.indexOf(202) < order.indexOf(201));
    roles.addEntry(202, QStringLiteral("Stallion"), QStringLiteral("South"), 0);
    QCOMPARE(controller.selected()->label(), QStringLiteral("Stallion · South"));
    const int count = controller.vehicles()->count();
    controller.initialize(&roles);
    QCOMPARE(controller.vehicles()->count(), count);
    COPController restored;
    restored.initialize(&roles);
    QCOMPARE(restored.vehicles()->count(), count);
    QCOMPARE(settings.value(QStringLiteral("COP/VehicleOrder")).toList(), order);
}

void COPControllerTest::_pendingControl()
{
    VehicleRoleController roles;
    roles.addEntry(203, QStringLiteral("Hex"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);
    QCOMPARE(controller.selectedSysid(), 0);
    QVERIFY(!controller.selected());
    controller.assumeControl();
    QCOMPARE(controller.pendingSysid(), 0);
    controller.selectVehicle(203);
    controller.assumeControl();
    QCOMPARE(controller.pendingSysid(), 203);
    controller.selectVehicle(999);
    QCOMPARE(controller.selectedSysid(), 203);
    controller.cancelControl();
    QCOMPARE(controller.pendingSysid(), 0);
}

void COPControllerTest::_notificationQueue()
{
    COPController controller;
    controller.notify(QStringLiteral(" "));
    QVERIFY(controller.messages().isEmpty());
    for (int i = 0; i < 205; ++i) {
        controller.notify(QString::number(i));
    }
    QCOMPARE(controller.messages().size(), 205);
    QCOMPARE(controller.messages().first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("204"));
    QVERIFY(controller.unacknowledged());
    controller.acknowledge();
    QVERIFY(!controller.unacknowledged());
    QCOMPARE(controller.messages().size(), 200);
    controller.notify(QStringLiteral("Next"));
    QVERIFY(controller.unacknowledged());
    for (int i = 0; i < 1005; ++i) {
        controller.notify(QString::number(i));
    }
    QCOMPARE(controller.messages().size(), 1000);
    QCOMPARE(controller.droppedMessages(), quint64(6));
    controller.acknowledge();
    QCOMPARE(controller.messages().size(), 200);
    QCOMPARE(controller.droppedMessages(), quint64(0));
}

void COPControllerTest::_referencePoint()
{
    QByteArray frame(25, '\0');
    const auto setBits = [&frame](int offset, int count, quint64 value) {
        for (int i = 0; i < count; ++i) {
            const int bit = offset + i;
            const quint8 mask = static_cast<quint8>(((value >> (count - i - 1)) & 1) << (7 - bit % 8));
            frame[bit / 8] = static_cast<char>(static_cast<quint8>(frame[bit / 8]) | mask);
        }
    };
    frame[0] = static_cast<char>(0xd3);
    setBits(14, 10, 19);
    setBits(24, 12, 1005);
    // WGS84 equator, longitude 180 degrees; exercises signed ECEF extraction.
    setBits(58, 38, static_cast<quint64>(-63781370000LL));
    const auto crc = RTCMFramer::crc24q({reinterpret_cast<const uint8_t*>(frame.constData()), 22});
    setBits(176, 24, crc);
    const auto coordinate = COPReferencePoint::decode(frame);
    QVERIFY(coordinate.isValid());
    QVERIFY(qAbs(coordinate.latitude()) < 0.000001);
    QVERIFY(qAbs(qAbs(coordinate.longitude()) - 180) < 0.000001);
    QVERIFY(qAbs(coordinate.altitude()) < 0.001);
    frame[10] = static_cast<char>(frame[10] ^ 1);
    QVERIFY(!COPReferencePoint::decode(frame).isValid());
    QVERIFY(!COPReferencePoint::decode(frame.first(10)).isValid());
}

UT_REGISTER_TEST(COPControllerTest, TestLabel::Unit)

void COPVehicleLifecycleTest::_disconnectAndReconnect()
{
    VehicleRoleController roles;
    COPController controller;
    controller.initialize(&roles);
    const auto reconnect = [this]() {
        auto* mockConfig = new MockConfiguration(QStringLiteral("COP reconnect"));
        mockConfig->setFirmwareType(MAV_AUTOPILOT_PX4);
        mockConfig->setVehicleType(MAV_TYPE_QUADROTOR);
        mockConfig->setIncrementVehicleId(false);
        mockConfig->setDynamic(true);
        auto config = LinkManager::instance()->addConfiguration(mockConfig);
        if (!LinkManager::instance()->createConnectedLink(config)) {
            return false;
        }
        _mockLink = qobject_cast<MockLink*>(config->link());
        return _mockLink != nullptr;
    };
    QVERIFY(reconnect());
    QTRY_VERIFY_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), TestTimeout::longMs());
    _vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->isInitialConnectComplete(), TestTimeout::longMs());
    const int sysid = vehicle()->id();
    controller.selectVehicle(sysid);
    auto* entry = controller.selected();
    QVERIFY(entry);
    QVERIFY(entry->connected());
    QCOMPARE(entry->vehicle(), vehicle());
    Vehicle* currentVehicle = vehicle();
    QVERIFY(currentVehicle);
    const int notificationCount = controller.messages().size();
    for (int severity = MAV_SEVERITY_CRITICAL; severity <= MAV_SEVERITY_DEBUG; ++severity) {
        emit currentVehicle->textMessageReceived(sysid, 1, severity, QStringLiteral("Compass not healthy"), QString());
    }
    QCOMPARE(controller.messages().size(), notificationCount);
    for (int severity : {MAV_SEVERITY_EMERGENCY, MAV_SEVERITY_ALERT}) {
        emit currentVehicle->textMessageReceived(sysid, 1, severity, QStringLiteral("Immediate action required"),
                                                 QString());
    }
    QCOMPARE(controller.messages().size(), notificationCount + 2);
    emit currentVehicle->vehicleLinkManager()->communicationLostChanged(true);
    QCOMPARE(controller.messages().size(), notificationCount + 3);
    emit currentVehicle->vehicleLinkManager()->communicationLostChanged(false);
    QCOMPARE(controller.messages().size(), notificationCount + 3);
    const int count = controller.vehicles()->count();
    const QString mode = entry->flightMode();
    _disconnectMockLink();
    QVERIFY(!entry->connected());
    QVERIFY(!entry->vehicle());
    QCOMPARE(entry->flightMode(), mode);
    QCOMPARE(controller.vehicles()->count(), count);
    controller.assumeControl();
    QCOMPARE(controller.pendingSysid(), sysid);
    QVERIFY(reconnect());
    QTRY_VERIFY_WITH_TIMEOUT(MultiVehicleManager::instance()->activeVehicle(), TestTimeout::longMs());
    _vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle());
    QCOMPARE(vehicle()->id(), sysid);
    QTRY_COMPARE_WITH_TIMEOUT(controller.pendingSysid(), 0, TestTimeout::mediumMs());
    QCOMPARE(controller.selected(), entry);
    QCOMPARE(entry->vehicle(), vehicle());
    QCOMPARE(controller.vehicles()->count(), count);
}

UT_REGISTER_TEST(COPVehicleLifecycleTest, TestLabel::Integration, TestLabel::Vehicle)
