#include "COPStressTest.h"

#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
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
    roles.addEntry(243, QStringLiteral("Hex"), QString(), 0);
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
    if (_links.isEmpty()) {
        auto* manager = MultiVehicleManager::instance();
        connect(manager, &MultiVehicleManager::vehicleAdded, _window, [this, manager](Vehicle* vehicle) {
            if (vehicle && manager->vehicles()->count() > 1) {
                expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle %1").arg(vehicle->id())));
                QTimer::singleShot(0, _window, [this]() {
                    QVERIFY(acceptDialog());
                    verifyExpectedLogMessage();
                });
            }
        });
    }
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

}  // namespace

COPController* COPStressUITest::_controller() const
{
    return _engine ? _engine->singletonInstance<COPController*>("QGC", "COPController") : nullptr;
}

COPVehicle* COPStressUITest::_entryFor(int sysid) const
{
    auto* controller = _controller();
    if (!controller) {
        return nullptr;
    }
    auto* model = controller->vehicles();
    for (int i = 0; i < model->count(); ++i) {
        auto* entry = model->value<COPVehicle*>(i);
        if (entry && entry->sysid() == sysid) {
            return entry;
        }
    }
    return nullptr;
}

bool COPStressUITest::_selectTab(int sysid)
{
    auto* entry = _entryFor(sysid);
    const QString text = sysid == 0 ? QStringLiteral("COP") : entry ? entry->label() : QString();
    auto* button = findText(_rootItem, text);
    return button && _clickItemAt(button, 0.5, 0.5, text);
}

void COPStressUITest::cleanup()
{
    if (auto* controller = _controller()) {
        controller->cancelControl();
    }
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
    auto* controller = _controller();
    QVERIFY(controller);
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
    roles->addEntry(roverId, QStringLiteral("Rover"), QString(), 0);
    roles->addEntry(hexId, QStringLiteral("Hex"), QString(), 0);
    roles->addEntry(stallionId, QStringLiteral("Stallion"), QString(), 0);

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
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(id) && _entryFor(id)->vehicle(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(id)->vehicle()->isInitialConnectComplete(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(id)->coordinate().isValid(), TestTimeout::longMs());
        QCOMPARE(_entryFor(id)->coordinate(), _entryFor(id)->vehicle()->coordinate());
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
    auto* retained = _entryFor(stallionId);
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
    bool returningLinkDropped = false;
    const auto drop = connect(manager, &MultiVehicleManager::vehicleAdded, this, [&](Vehicle* vehicle) {
        if (vehicle && vehicle->id() == stallionId && stallion) {
            stallion->disconnect();
            returningLinkDropped = true;
        }
    });
    switching.start();
    QTRY_VERIFY_WITH_TIMEOUT(returningLinkDropped, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(controller->pendingSysid(), 0, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(!retained->vehicle(), TestTimeout::longMs());
    disconnect(drop);
    switching.stop();
    QCOMPARE(_entryFor(stallionId), retained);
    stallion = _start(MAV_TYPE_FIXED_WING, true);
    QVERIFY(stallion);
    QCOMPARE(stallion->vehicleId(), stallionId);
    QTRY_VERIFY_WITH_TIMEOUT(retained->connected(), TestTimeout::longMs());

    auto* extra = _start(MAV_TYPE_GROUND_ROVER);
    QVERIFY(extra);
    const int extraId = extra->vehicleId();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 4, TestTimeout::longMs());
    QVERIFY(_entryFor(extraId));
    QCOMPARE(_entryFor(extraId)->label(), QStringLiteral("Vehicle %1").arg(extraId));
    const int tabCount = controller->vehicles()->count();
    for (const auto& link : {hex, QPointer<MockLink>(extra), rover, stallion}) {
        QVERIFY(link);
        const int id = link->vehicleId();
        QPointer<Vehicle> oldVehicle = _entryFor(id)->vehicle();
        switching.start();
        link->disconnect();
        QTRY_VERIFY_WITH_TIMEOUT(oldVehicle.isNull(), TestTimeout::longMs());
        QVERIFY(!_entryFor(id)->connected());
        QVERIFY(!_entryFor(id)->vehicle());
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
    auto* controller = _controller();
    QVERIFY(controller);
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    auto* a = _entryFor(first->vehicleId());
    auto* b = _entryFor(second->vehicleId());
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
    auto* controller = _controller();
    QVERIFY(controller);
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 3, TestTimeout::longMs());
    auto* selected = _entryFor(hex->vehicleId());
    QVERIFY(selected && selected->vehicle());
    controller->selectVehicle(selected->sysid());
    QVERIFY(QGCCorePlugin::instance()->startStandardVideoReceivers());
    controller->assumeControl();
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), selected->vehicle(), TestTimeout::longMs());
    auto* removedEntry = _entryFor(stallion->vehicleId());
    QVERIFY(removedEntry && removedEntry->vehicle());
    QPointer<Vehicle> removed = removedEntry->vehicle();
    stallion->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(removed.isNull(), TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), selected->vehicle(), TestTimeout::mediumMs());
}

void COPStressUITest::_navigationAndLayout()
{
    // Exercise resizing and navigation within the same window lifetime.
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_selectTab(0));
    auto* controller = _controller();
    QVERIFY(controller);
    QVERIFY(!QGCCorePlugin::instance()->startStandardVideoReceivers());
    const int priorMessageCount = controller->messages().size();
    controller->notify(QStringLiteral("Communication lost: review vehicle status"));
    QCOMPARE(controller->messages().size(), priorMessageCount + 1);
    auto* fly = findItem(_rootItem, QStringLiteral("mainView_fly"));
    auto* map = findItem(_rootItem, QStringLiteral("copMap"));
    auto* video = findItem(_rootItem, QStringLiteral("copVideoRegion"));
    auto* notifications = findItem(_rootItem, QStringLiteral("copNotifications"));
    auto* logo = findItem(_rootItem, QStringLiteral("toolbar_qgcLogo"));
    QVERIFY(fly && map && video && notifications && logo);
    QVERIFY(logo->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(notifications->height() > 0, TestTimeout::shortMs());
    auto* header = qvariant_cast<QQuickItem*>(_window->property("header"));
    QVERIFY(header);
    const auto sceneRect = [](QQuickItem* item) { return item->mapRectToScene(item->boundingRect()); };

    for (const QSize windowSize : {QSize(1280, 800), QSize(800, 600), QSize(480, 600)}) {
        _window->resize(windowSize);
        QTRY_VERIFY_WITH_TIMEOUT(map->width() >= fly->width() * 0.4, TestTimeout::shortMs());
        QTRY_VERIFY_WITH_TIMEOUT(video->width() >= fly->width() * 0.2, TestTimeout::shortMs());
        QVERIFY(sceneRect(map).left() >= sceneRect(fly).left());
        QVERIFY(sceneRect(map).right() <= sceneRect(video).left());
        QVERIFY(sceneRect(video).right() <= sceneRect(fly).right());
        QVERIFY(sceneRect(header).bottom() <= sceneRect(logo).top());
        QVERIFY(sceneRect(logo).bottom() <= sceneRect(map).top());
        QVERIFY(sceneRect(fly).bottom() <= sceneRect(notifications).top());
        QTRY_VERIFY_WITH_TIMEOUT(qRound(sceneRect(notifications).bottom()) <= _window->height(),
                                 TestTimeout::shortMs());
    }

    _window->resize(1280, 1000);
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewAnalyze")));
    QVERIFY(clickButton(QStringLiteral("analyzeButton_Vehicle Roles")));
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewSettings")));
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewConfigure")));
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewPlan")));
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewFly")));
}

UT_REGISTER_TEST(COPStressTest, TestLabel::Unit)
UT_REGISTER_TEST(COPStressUITest, TestLabel::Integration, TestLabel::Vehicle)
