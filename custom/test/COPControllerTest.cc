#include "COPControllerTest.h"

#include <limits>

#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "COPReferencePoint.h"
#include "Checksums.h"
#include "GPSCorrectionManager.h"
#include "GPSCorrectionSettings.h"
#include "LinkManager.h"
#include "ManualScheduler.h"
#include "MockLink.h"
#include "SettingsManager.h"
#include "VehicleLinkManager.h"
#include "VehicleSystemIdTestData.h"

void COPControllerTest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    QSettings().remove(QStringLiteral("COP"));
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
    roles.addEntry(201, QStringLiteral("Copter"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);
    controller.selectVehicle(202);
    QVERIFY(controller.selected());
    QCOMPARE(controller.selected()->label(), QStringLiteral("Rover - North"));
    const auto order = settings.value(QStringLiteral("COP/VehicleOrder")).toList();
    QVERIFY(order.indexOf(202) < order.indexOf(201));
    roles.addEntry(202, QStringLiteral("Plane"), QStringLiteral("South"), 0);
    QCOMPARE(controller.selected()->label(), QStringLiteral("Plane - South"));
    const int count = controller.vehicles()->count();
    controller.initialize(&roles);
    QCOMPARE(controller.vehicles()->count(), count);
    COPController restored;
    restored.initialize(&roles);
    QCOMPARE(restored.vehicles()->count(), count);
    QCOMPARE(settings.value(QStringLiteral("COP/VehicleOrder")).toList(), order);
}

void COPControllerTest::_colorAssignedByDiscoveryOrder()
{
    QSettings settings;
    settings.remove(QStringLiteral("COP"));
    VehicleRoleController roles;
    roles.addEntry(101, QStringLiteral("Rover"), QString(), 0);
    roles.addEntry(102, QStringLiteral("Copter"), QString(), 0);
    roles.addEntry(103, QStringLiteral("Plane"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);

    controller.selectVehicle(101);
    auto* first = controller.selected();
    controller.selectVehicle(102);
    auto* second = controller.selected();
    controller.selectVehicle(103);
    auto* third = controller.selected();
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(third);
    QVERIFY(first->color() != second->color());
    QVERIFY(second->color() != third->color());
    QVERIFY(first->color() != third->color());

    // Stable across a fresh controller loading the same persisted order, not re-derived from
    // current role/sysid values - an operator should keep seeing "their" vehicle in the same color.
    COPController restored;
    restored.initialize(&roles);
    restored.selectVehicle(101);
    QCOMPARE(restored.selected()->color(), first->color());
    restored.selectVehicle(102);
    QCOMPARE(restored.selected()->color(), second->color());
    restored.selectVehicle(103);
    QCOMPARE(restored.selected()->color(), third->color());
}

void COPControllerTest::_pendingControl_data()
{
    VehicleSystemIdTestData::addValidRows();
}

void COPControllerTest::_pendingControl()
{
    QFETCH(quint32, sysid);
    VehicleRoleController roles;
    roles.addEntry(sysid, QStringLiteral("Copter"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);
    QCOMPARE(controller.selectedSysid(), 0);
    QVERIFY(!controller.selected());
    controller.assumeControl();
    QCOMPARE(controller.pendingSysid(), 0);
    controller.selectVehicle(sysid);
    controller.assumeControl();
    QCOMPARE(controller.pendingSysid(), sysid);
    controller.selectVehicle(999);
    QCOMPARE(controller.selectedSysid(), sysid);
    controller.cancelControl();
    QCOMPARE(controller.pendingSysid(), 0);
}

void COPControllerTest::_systemIdSettingsRoundTrip_data()
{
    VehicleSystemIdTestData::addValidRows();
}

void COPControllerTest::_systemIdSettingsRoundTrip()
{
    QFETCH(quint32, sysid);
    const quint32 otherSysid = sysid == 1 ? 257 : 1;
    VehicleRoleController emptyRoles;
    QColor color;
    const QString uri = QStringLiteral("rtsp://camera/%1").arg(sysid);
    {
        VehicleRoleController roles;
        roles.addEntry(sysid, QStringLiteral("Rover"), QStringLiteral("North"), 0);
        roles.addEntry(otherSysid, QStringLiteral("Plane"), QStringLiteral("South"), 0);
        COPController controller;
        controller.initialize(&roles);
        QCOMPARE(controller.vehicles()->count(), 2);
        controller.selectVehicle(sysid);
        QVERIFY(controller.selected());
        QCOMPARE(controller.selected()->sysid(), sysid);
        QCOMPARE(controller.selected()->label(), QStringLiteral("Rover - North"));
        controller.selected()->setVideoUri(uri);
        color = controller.selected()->color();
        controller.selectVehicle(otherSysid);
        QCOMPARE(controller.selected()->sysid(), otherSysid);
        QCOMPARE(controller.selected()->label(), QStringLiteral("Plane - South"));
        QCOMPARE(controller.selected()->videoUri(), QString());
        QSettings settings;
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QCOMPARE(settings.value(QStringLiteral("COP/Video/%1").arg(sysid)).toString(), uri);
        QCOMPARE(settings.value(QStringLiteral("COP/VehicleOrder")).toList(),
                 QVariantList({QVariant::fromValue(sysid), QVariant::fromValue(otherSysid)}));
    }

    // No role entries can repopulate this controller: both identities must come from QSettings.
    COPController restored;
    restored.initialize(&emptyRoles);
    QCOMPARE(restored.vehicles()->count(), 2);
    QCOMPARE(restored.vehicles()->value<COPVehicle*>(0)->sysid(), sysid);
    QCOMPARE(restored.vehicles()->value<COPVehicle*>(1)->sysid(), otherSysid);
    restored.selectVehicle(sysid);
    QVERIFY(restored.selected());
    QCOMPARE(restored.selected()->sysid(), sysid);
    QCOMPARE(restored.selected()->videoUri(), uri);
    QCOMPARE(restored.selected()->color(), color);
    restored.selectVehicle(otherSysid);
    QCOMPARE(restored.selected()->sysid(), otherSysid);
    QCOMPARE(restored.selected()->videoUri(), QString());
}

void COPControllerTest::_savedOrderValidation_data()
{
    QTest::addColumn<QVariant>("savedId");
    QTest::addColumn<quint32>("expected");
    QTest::newRow("old-int") << QVariant(255) << quint32(255);
    QTest::newRow("old-string") << QVariant(QStringLiteral("255")) << quint32(255);
    QTest::newRow("wide-string") << QVariant(QStringLiteral(" 4294967295 ")) << quint32(4294967295U);
    QTest::newRow("wide-double") << QVariant(2147483648.0) << quint32(2147483648U);
    QTest::newRow("wide-uint64") << QVariant::fromValue(qulonglong(4294967295U)) << quint32(4294967295U);
    QTest::newRow("zero") << QVariant(0) << quint32(0);
    QTest::newRow("negative") << QVariant(-1) << quint32(0);
    QTest::newRow("negative-string") << QVariant(QStringLiteral("-1")) << quint32(0);
    QTest::newRow("fraction") << QVariant(1.5) << quint32(0);
    QTest::newRow("fraction-string") << QVariant(QStringLiteral("1.5")) << quint32(0);
    QTest::newRow("overflow") << QVariant(4294967296.0) << quint32(0);
    QTest::newRow("overflow-string") << QVariant(QStringLiteral("4294967296")) << quint32(0);
    QTest::newRow("uint64-overflow") << QVariant::fromValue(std::numeric_limits<qulonglong>::max()) << quint32(0);
    QTest::newRow("uint64-overflow-string") << QVariant(QStringLiteral("18446744073709551616")) << quint32(0);
    QTest::newRow("text") << QVariant(QStringLiteral("invalid")) << quint32(0);
    QTest::newRow("text-suffix") << QVariant(QStringLiteral("255abc")) << quint32(0);
    QTest::newRow("empty") << QVariant(QString()) << quint32(0);
    QTest::newRow("boolean") << QVariant(true) << quint32(0);
    QTest::newRow("nan") << QVariant(std::numeric_limits<double>::quiet_NaN()) << quint32(0);
    QTest::newRow("infinity") << QVariant(std::numeric_limits<double>::infinity()) << quint32(0);
}

void COPControllerTest::_savedOrderValidation()
{
    QFETCH(QVariant, savedId);
    QFETCH(quint32, expected);
    QSettings().setValue(QStringLiteral("COP/VehicleOrder"), QVariantList{savedId, savedId});
    VehicleRoleController roles;
    COPController controller;
    controller.initialize(&roles);
    QCOMPARE(controller.vehicles()->count(), expected ? 1 : 0);
    if (expected) {
        controller.selectVehicle(expected);
        QVERIFY(controller.selected());
        QCOMPARE(controller.selected()->sysid(), expected);
        controller.assumeControl();
        QCOMPARE(controller.pendingSysid(), expected);
    }
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

namespace {
/// A valid RTCM 1005 frame whose antenna reference ECEF X is @a ecefX in 0.1 mm units.
QByteArray rtcm1005Frame(qint64 ecefX)
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
    setBits(58, 38, static_cast<quint64>(ecefX));
    const auto crc = QGC::crc24q({reinterpret_cast<const uint8_t*>(frame.constData()), 22});
    setBits(176, 24, crc);
    return frame;
}
}  // namespace

void COPControllerTest::_referencePoint()
{
    // WGS84 equator, longitude 180 degrees; exercises signed ECEF extraction.
    QByteArray frame = rtcm1005Frame(-63781370000LL);
    const auto coordinate = COPReferencePoint::decode(frame);
    QVERIFY(coordinate.isValid());
    QVERIFY(qAbs(coordinate.latitude()) < 0.000001);
    QVERIFY(qAbs(qAbs(coordinate.longitude()) - 180) < 0.000001);
    QVERIFY(qAbs(coordinate.altitude()) < 0.001);
    frame[10] = static_cast<char>(frame[10] ^ 1);
    QVERIFY(!COPReferencePoint::decode(frame).isValid());
    QVERIFY(!COPReferencePoint::decode(frame.first(10)).isValid());
}

void COPControllerTest::_referencePointFollowsSelectedStream()
{
    ManualScheduler scheduler;
    GPSCorrectionManager corrections(nullptr, &scheduler);
    COPReferencePoint reference(nullptr, &corrections, 500);
    QSignalSpy admitted(&corrections, &GPSCorrectionManager::selectedFrameAdmitted);
    const QByteArray west = rtcm1005Frame(-63781370000LL);
    const QByteArray east = rtcm1005Frame(63781370000LL);
    const auto advance = [&scheduler]() { return scheduler.advanceBy(std::chrono::seconds(1)); };

    // A valid frame of the selected stream sets the reference point
    auto ntrip = corrections.openSource(GPSCorrectionSettings::Ntrip, QStringLiteral("caster"));
    QVERIFY(advance());
    ntrip->submit(west, scheduler.nowMs());
    QCOMPARE(admitted.count(), 1);
    QCOMPARE(admitted.constFirst().at(2).toULongLong(), quint64(1));
    QVERIFY(reference.valid());
    QVERIFY(qAbs(qAbs(reference.longitude()) - 180) < 0.000001);
    QCOMPARE(reference.source(), QStringLiteral("NTRIP reference"));

    // A source that is not selected is never admitted and cannot move it
    auto udp = corrections.openSource(GPSCorrectionSettings::Udp, QStringLiteral("radio"));
    QVERIFY(advance());
    udp->submit(east, scheduler.nowMs());
    QCOMPARE(admitted.count(), 1);
    QVERIFY(qAbs(qAbs(reference.longitude()) - 180) < 0.000001);
    udp.reset();
    QVERIFY(advance());
    QVERIFY(reference.valid());

    // The selected source ending drops the reference without waiting for another frame
    ntrip.reset();
    QVERIFY(advance());
    QVERIFY(!reference.valid());

    // A restarted source is a new session: a corrupt frame cannot set a reference, a valid one does
    ntrip = corrections.openSource(GPSCorrectionSettings::Ntrip, QStringLiteral("caster"));
    QVERIFY(advance());
    QByteArray corrupt = east;
    corrupt[10] = static_cast<char>(corrupt[10] ^ 1);
    ntrip->submit(corrupt, scheduler.nowMs());
    QVERIFY(!reference.valid());
    ntrip->submit(east, scheduler.nowMs());
    QCOMPARE(admitted.count(), 3);
    QCOMPARE(admitted.constLast().at(2).toULongLong(), quint64(2));
    QVERIFY(reference.valid());
    QVERIFY(qAbs(reference.longitude()) < 0.000001);

    // Without further valid frames it expires
    QTRY_VERIFY_WITH_TIMEOUT(!reference.valid(), TestTimeout::mediumMs());
}

UT_REGISTER_TEST(COPControllerTest, TestLabel::Unit)

void COPVehicleLifecycleTest::_disconnectAndReconnect_data()
{
    QTest::addColumn<quint32>("requestedSysid");
    QTest::newRow("legacy") << quint32(128);
    QTest::newRow("wide") << quint32(4294967295U);
}

void COPVehicleLifecycleTest::_disconnectAndReconnect()
{
    QFETCH(quint32, requestedSysid);
    VehicleRoleController roles;
    COPController controller;
    controller.initialize(&roles);
    const auto reconnect = [this, requestedSysid]() {
        auto* mockConfig = new MockConfiguration(QStringLiteral("COP reconnect"));
        mockConfig->setFirmwareType(MAV_AUTOPILOT_PX4);
        mockConfig->setVehicleType(MAV_TYPE_QUADROTOR);
        mockConfig->setIncrementVehicleId(false);
        mockConfig->setSystemId(requestedSysid);
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
    const quint32 sysid = vehicle()->id();
    QCOMPARE(sysid, requestedSysid);
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
