#include "COPStressTest.h"

#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "COPVideoSession.h"
#include "LinkManager.h"
#include "MultiVehicleManager.h"
#include "QGCCorePlugin.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"

void COPStressTest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void COPStressTest::cleanup()
{
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    UnitTest::cleanup();
}

void COPStressUITest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void COPStressTest::_videoOverrideSurvivesRestart()
{
    VehicleRoleController roles;
    roles.addEntry(243, QStringLiteral("Hex"), QString());
    const QString uri = QStringLiteral("udp://0.0.0.0:5643");
    {
        COPController first;
        first.initialize(&roles);
        first.selectVehicle(243);
        QVERIFY(first.selected());
        first.selected()->setVideoUri(uri);
    }
    QCOMPARE(QSettings().value(QStringLiteral("COP/Video/243")).toString(), uri);
    COPController restored;
    restored.initialize(&roles);
    restored.selectVehicle(243);
    QVERIFY(restored.selected());
    QCOMPARE(restored.selected()->videoUri(), uri);
}

void COPStressTest::_unacknowledgedBurstIsNotDiscarded()
{
    VehicleRoleController roles;
    COPController controller;
    controller.initialize(&roles);
    for (int i = 0; i < 205; ++i) {
        emit QGCCorePlugin::instance() -> operatorNotification(QStringLiteral("Unacknowledged advisory %1").arg(i));
    }
    QVERIFY(controller.unacknowledged());
    // This deliberately asserts the requested no-loss contract, beyond the documented history cap.
    QCOMPARE(controller.messages().size(), 205);
}

MockLink* COPStressUITest::_start(MAV_TYPE type, bool increment)
{
    // Same shared configuration/createConnectedLink ownership as VehicleLinkManagerTest::_startMockLink.
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("COP stress %1").arg(_configs.size()));
    config->setDynamic(true);
    config->setFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    config->setVehicleType(type);
    config->setIncrementVehicleId(increment);
    if (qEnvironmentVariableIsSet("QGC_TEST_ENABLE_GSTREAMER")) {
        config->setEnableCamera(true);
        // MockLink fixes ports by stream type: use 5600, 5601 and 8554, not three senders on 5600.
        if (type == MAV_TYPE_FIXED_WING) {
            config->setVideoStreamType(MockConfiguration::VideoStreamRtspH264);
        } else if (type == MAV_TYPE_QUADROTOR) {
            config->setVideoStreamType(MockConfiguration::VideoStreamRtpUdpH265);
        } else if (_configs.isEmpty()) {
            config->setVideoStreamType(MockConfiguration::VideoStreamRtpUdpH264);
        }
    }
    SharedLinkConfigurationPtr sharedConfig = config;
    _configs.append(sharedConfig);
    if (!LinkManager::instance()->createConnectedLink(sharedConfig)) {
        return nullptr;
    }
    auto* link = qobject_cast<MockLink*>(config->link());
    _links.append(link);
    return link;
}

namespace {
QQuickItem* findText(QQuickItem* root, const QString& text)
{
    if (!root || !root->isVisible()) {
        return nullptr;
    }
    if (root->property("text").toString() == text && root->metaObject()->indexOfSignal("clicked()") >= 0) {
        return root;
    }
    for (auto* child : root->childItems()) {
        if (auto* item = findText(child, text)) {
            return item;
        }
    }
    return nullptr;
}

COPVehicle* entryFor(int sysid)
{
    auto* model = COPController::instance()->vehicles();
    for (int i = 0; i < model->count(); ++i) {
        auto* entry = model->value<COPVehicle*>(i);
        if (entry && entry->sysid() == sysid) {
            return entry;
        }
    }
    return nullptr;
}
}  // namespace

bool COPStressUITest::_selectTab(int sysid)
{
    auto* entry = entryFor(sysid);
    const QString text = sysid == 0 ? QStringLiteral("COP") : entry ? entry->label() : QString();
    auto* button = findText(_rootItem, text);
    return button && _clickItemAt(button, 0.5, 0.5, text);
}

void COPStressUITest::cleanup()
{
    COPController::instance()->cancelControl();
    for (const auto& link : _links) {
        if (link) {
            link->disconnect();
        }
    }
    const bool removed = waitForCondition([] { return MultiVehicleManager::instance()->vehicles()->count() == 0; },
                                          TestTimeout::longMs(), QStringLiteral("all stress vehicles removed"));
    _links.clear();
    _configs.clear();
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    QmlUITestBase::cleanup();
    QVERIFY(removed);
}

void COPStressUITest::_threeVehicleChurn()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    auto* manager = MultiVehicleManager::instance();
    auto* controller = COPController::instance();
    QPointer<MockLink> rover = _start(MAV_TYPE_GROUND_ROVER);
    QPointer<MockLink> hex = _start(MAV_TYPE_QUADROTOR);
    // Keep the next ID unconsumed, following COPVehicleLifecycleTest's same-ID reconnect precedent.
    QPointer<MockLink> stallion = _start(MAV_TYPE_FIXED_WING, false);
    QVERIFY(rover && hex && stallion);
    const int roverId = rover->vehicleId();
    const int hexId = hex->vehicleId();
    const int stallionId = stallion->vehicleId();
    QVERIFY(roverId != hexId && hexId != stallionId && roverId != stallionId);
    auto* roles = _engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(roles);
    roles->addEntry(roverId, QStringLiteral("Rover"), QString());
    roles->addEntry(hexId, QStringLiteral("Hex"), QString());
    roles->addEntry(stallionId, QStringLiteral("Stallion"), QString());

    QTimer switching;
    switching.setInterval(1);
    int selection = 0;
    connect(&switching, &QTimer::timeout, this, [&] {
        const int ids[] = {0, roverId, hexId, stallionId};
        controller->selectVehicle(ids[selection++ % 4]);
    });
    switching.start();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 3, TestTimeout::longMs());
    for (int id : {roverId, hexId, stallionId}) {
        QTRY_VERIFY_WITH_TIMEOUT(entryFor(id) && entryFor(id)->vehicle(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(entryFor(id)->vehicle()->isInitialConnectComplete(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(entryFor(id)->coordinate().isValid(), TestTimeout::longMs());
        QCOMPARE(entryFor(id)->coordinate(), entryFor(id)->vehicle()->coordinate());
    }
    switching.stop();
    QVERIFY(_selectTab(0));
    QVERIFY(_selectTab(hexId));
    QVERIFY(_selectTab(0));

    if (qEnvironmentVariableIsSet("QGC_TEST_ENABLE_GSTREAMER")) {
        // Require real decoding before claiming to have tested disconnect during a stream.
        const auto decoded = [&] {
            int count = 0;
            for (auto* session : _rootItem->findChildren<COPVideoSession*>()) {
                count += session->decoding() ? 1 : 0;
            }
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(decoded(), 3, TestTimeout::longMs());
        stallion->disconnect();
        QTRY_COMPARE_WITH_TIMEOUT(decoded(), 2, TestTimeout::longMs());
    } else {
        stallion->disconnect();
    }
    auto* retained = entryFor(stallionId);
    QVERIFY(retained);
    QTRY_VERIFY_WITH_TIMEOUT(!retained->vehicle(), TestTimeout::longMs());
    QVERIFY(_selectTab(stallionId));
    auto* assume = findText(_rootItem, QStringLiteral("Assume Control"));
    QVERIFY(assume);
    QVERIFY(_clickItemAt(assume, 0.5, 0.5, QStringLiteral("Assume Control")));
    QCOMPARE(controller->pendingSysid(), stallionId);

    // Drop the returning link in vehicleAdded, before the deferred active-vehicle switch resolves.
    stallion = _start(MAV_TYPE_FIXED_WING, false);
    QVERIFY(stallion);
    QCOMPARE(stallion->vehicleId(), stallionId);
    const auto drop = connect(manager, &MultiVehicleManager::vehicleAdded, this, [&](Vehicle* vehicle) {
        if (vehicle && vehicle->id() == stallionId && stallion) {
            stallion->disconnect();
        }
    });
    switching.start();
    QTRY_COMPARE_WITH_TIMEOUT(controller->pendingSysid(), 0, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(!retained->vehicle(), TestTimeout::longMs());
    disconnect(drop);
    switching.stop();
    QCOMPARE(entryFor(stallionId), retained);
    stallion = _start(MAV_TYPE_FIXED_WING, true);
    QVERIFY(stallion);
    QCOMPARE(stallion->vehicleId(), stallionId);
    QTRY_VERIFY_WITH_TIMEOUT(retained->connected(), TestTimeout::longMs());

    auto* extra = _start(MAV_TYPE_GROUND_ROVER);
    QVERIFY(extra);
    const int extraId = extra->vehicleId();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 4, TestTimeout::longMs());
    QVERIFY(entryFor(extraId));
    QCOMPARE(entryFor(extraId)->label(), QStringLiteral("Vehicle %1").arg(extraId));
    const int tabCount = controller->vehicles()->count();
    for (const auto& link : {hex, QPointer<MockLink>(extra), rover, stallion}) {
        QVERIFY(link);
        const int id = link->vehicleId();
        QPointer<Vehicle> oldVehicle = entryFor(id)->vehicle();
        switching.start();
        link->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(oldVehicle.isNull(), TestTimeout::longMs());
        QVERIFY(!entryFor(id)->connected());
        QVERIFY(!entryFor(id)->vehicle());
        QCOMPARE(controller->vehicles()->count(), tabCount);
    }
    switching.stop();
    QVERIFY(_selectTab(0));
}

void COPStressUITest::_lastControlRequestWins()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    auto* first = _start(MAV_TYPE_GROUND_ROVER);
    auto* second = _start(MAV_TYPE_QUADROTOR);
    QVERIFY(first && second);
    auto* manager = MultiVehicleManager::instance();
    auto* controller = COPController::instance();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    auto* a = entryFor(first->vehicleId());
    auto* b = entryFor(second->vehicleId());
    QVERIFY(a && b && a->vehicle() && b->vehicle());
    manager->setActiveVehicle(a->vehicle());
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a->vehicle(), TestTimeout::longMs());
    QSignalSpy switched(manager, &MultiVehicleManager::activeVehicleChanged);
    QVERIFY(switched.isValid());
    controller->selectVehicle(b->sysid());
    controller->assumeControl();
    controller->selectVehicle(a->sysid());
    controller->assumeControl();
    QTimer settled;
    settled.setSingleShot(true);
    QSignalSpy settledSpy(&settled, &QTimer::timeout);
    settled.start(100);
    QVERIFY(settledSpy.wait(TestTimeout::mediumMs()));
    QCOMPARE(manager->activeVehicle(), a->vehicle());
}

void COPStressUITest::_disconnectOtherPreservesControl()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    auto* rover = _start(MAV_TYPE_GROUND_ROVER);
    auto* hex = _start(MAV_TYPE_QUADROTOR);
    QPointer<MockLink> stallion = _start(MAV_TYPE_FIXED_WING);
    QVERIFY(rover && hex && stallion);
    auto* manager = MultiVehicleManager::instance();
    auto* controller = COPController::instance();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 3, TestTimeout::longMs());
    auto* selected = entryFor(hex->vehicleId());
    QVERIFY(selected && selected->vehicle());
    controller->selectVehicle(selected->sysid());
    controller->assumeControl();
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), selected->vehicle(), TestTimeout::longMs());
    auto* removedEntry = entryFor(stallion->vehicleId());
    QVERIFY(removedEntry && removedEntry->vehicle());
    QPointer<Vehicle> removed = removedEntry->vehicle();
    stallion->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(removed.isNull(), TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), selected->vehicle(), TestTimeout::mediumMs());
}

UT_REGISTER_TEST(COPStressTest, TestLabel::Unit)
UT_REGISTER_TEST(COPStressUITest, TestLabel::Integration, TestLabel::Vehicle)
