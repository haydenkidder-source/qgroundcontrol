#include "COPStressTest.h"

#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "COPVideoSession.h"
#include "GeoFenceManager.h"
#include "MissionItem.h"
#include "PlanManager.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "MissionManager.h"
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
    // Vehicle roles persist under savePath, and a role makes COP remember that sysid under the role's label. Start each
    // test (and each stress iteration) without roles left behind by an earlier one, whose same-label tabs would
    // otherwise be matched in place of this test's vehicles.
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void COPStressTest::_videoOverrideSurvivesRestart()
{
    VehicleRoleController roles;
    roles.addEntry(243, QStringLiteral("Copter"), QString(), 0);
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
    roles->addEntry(hexId, QStringLiteral("Copter"), QString(), 0);
    roles->addEntry(stallionId, QStringLiteral("Plane"), QString(), 0);

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

void COPStressUITest::_activeHighlightRequiresActiveVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* manager = MultiVehicleManager::instance();
    auto* controller = _controller();
    QVERIFY(controller);
    auto* roles = _engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(roles);

    // A remembered-but-disconnected vehicle with no active vehicle at all - startup, or after every link drops
    constexpr int rememberedSysid = 127;  // Below MockLink's first sysid (128), so no connecting vehicle claims it
    roles->addEntry(rememberedSysid, QStringLiteral("Plane"), QStringLiteral("Remembered"), 0);
    auto* remembered = _entryFor(rememberedSysid);
    QVERIFY(remembered);
    QVERIFY(!remembered->vehicle());
    QVERIFY(!manager->activeVehicle());
    QVERIFY(_selectTab(0));

    auto* rememberedTab = findText(_rootItem, remembered->label());
    QVERIFY(rememberedTab);
    // No vehicle is being controlled, so no tab may claim to be the active vehicle
    QTRY_VERIFY_WITH_TIMEOUT(!rememberedTab->property("highlighted").toBool(), TestTimeout::shortMs());
    QVERIFY(!rememberedTab->property("_showHighlight").toBool());

    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> link = _start(MAV_TYPE_QUADROTOR);
    QVERIFY(link);
    QTRY_VERIFY_WITH_TIMEOUT(manager->activeVehicle(), TestTimeout::longMs());
    Vehicle* const activeVehicle = manager->activeVehicle();
    QVERIFY(activeVehicle);
    auto* active = _entryFor(activeVehicle->id());
    QVERIFY(active);
    auto* activeTab = findText(_rootItem, active->label());
    QVERIFY(activeTab);
    QTRY_VERIFY_WITH_TIMEOUT(activeTab->property("_showHighlight").toBool(), TestTimeout::shortMs());
    QVERIFY(!rememberedTab->property("_showHighlight").toBool());

    link->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(!manager->activeVehicle(), TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(!activeTab->property("_showHighlight").toBool(), TestTimeout::shortMs());
    QVERIFY(!rememberedTab->property("_showHighlight").toBool());
}

UT_REGISTER_TEST(COPStressTest, TestLabel::Unit)
UT_REGISTER_TEST(COPStressUITest, TestLabel::Integration, TestLabel::Vehicle)


namespace {
QQuickItem* findByName(QQuickItem* root, const QString& name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems()) {
        if (auto* item = findByName(child, name)) return item;
    }
    return nullptr;
}
void dumpMapItems(QQuickItem* root, int depth, int& n)
{
    for (auto* child : root->childItems()) {
        const QString cn = QString::fromLatin1(child->metaObject()->className());
        if (cn.contains(QStringLiteral("Map")) && !cn.contains(QStringLiteral("Quick"))) {
            qWarning() << "PREVIEW item" << cn << "vis" << child->isVisible() << "geom" << QRectF(child->x(), child->y(), child->width(), child->height())
                       << "pathLen" << child->property("path").toList().size() << "color" << child->property("color");
            n++;
        }
        dumpMapItems(child, depth + 1, n);
    }
}
}  // namespace

// SCRATCH preview (not for commit): renders COP overlays for 3 vehicles and saves PNGs.
void COPStressUITest::_overlayPreview()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    auto* manager = MultiVehicleManager::instance();
    auto* controller = _controller();
    QVERIFY(controller);
    QPointer<MockLink> rover = _start(MAV_TYPE_GROUND_ROVER);
    QPointer<MockLink> hex = _start(MAV_TYPE_QUADROTOR);
    QPointer<MockLink> stallion = _start(MAV_TYPE_FIXED_WING, false);
    QVERIFY(rover && hex && stallion);
    const int ids[3] = {rover->vehicleId(), hex->vehicleId(), stallion->vehicleId()};
    auto* roles = _engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(roles);
    roles->addEntry(ids[0], QStringLiteral("Rover"), QString(), 0);
    roles->addEntry(ids[1], QStringLiteral("Copter"), QString(), 0);
    roles->addEntry(ids[2], QStringLiteral("Plane"), QString(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 3, TestTimeout::longMs());
    for (int id : ids) {
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(id) && _entryFor(id)->vehicle(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(id)->vehicle()->isInitialConnectComplete(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(id)->coordinate().isValid(), TestTimeout::longMs());
    }
    const QGeoCoordinate base = _entryFor(ids[0])->coordinate();
    qWarning() << "PREVIEW base" << base << "coords" << _entryFor(ids[0])->coordinate() << _entryFor(ids[1])->coordinate() << _entryFor(ids[2])->coordinate();
    auto at = [&](double d, double az) { return base.atDistanceAndAzimuth(d, az); };
    auto wp = [&](int seq, const QGeoCoordinate& c, MAV_CMD cmd = MAV_CMD_NAV_WAYPOINT) {
        return new MissionItem(seq, cmd, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, 0, c.latitude(), c.longitude(), 30, true, false, this);
    };
    struct Spec { int id; double az; };
    const Spec specs[3] = {{ids[0], 270}, {ids[1], 45}, {ids[2], 135}};
    for (const Spec& sp : specs) {
        Vehicle* v = _entryFor(sp.id)->vehicle();
        const double a = sp.az;
        QList<MissionItem*> items;
        items << wp(0, base);
        items << wp(1, at(150, a));
        items << wp(2, at(300, a + 20));
        items << wp(3, at(450, a));
        items << wp(4, at(300, a - 25));
        QSignalSpy sent(v->missionManager(), &PlanManager::sendComplete);
        v->missionManager()->writeMissionItems(items);
        QTRY_VERIFY_WITH_TIMEOUT(sent.count() > 0, TestTimeout::longMs());

        QmlObjectListModel polys, circles;
        auto* incl = new QGCFencePolygon(true);
        for (double b : {0.0, 90.0, 180.0, 270.0}) { incl->appendVertex(at(550, a + b * 0.25 + b)); }
        polys.append(incl);
        auto* excl = new QGCFencePolygon(false);
        for (double b : {0.0, 120.0, 240.0}) { excl->appendVertex(at(80, a + 10).atDistanceAndAzimuth(40, b)); }
        polys.append(excl);
        circles.append(new QGCFenceCircle(at(300, a - 60), 90, sp.id == ids[1]));
        QSignalSpy fenceSent(v->geoFenceManager(), &GeoFenceManager::sendComplete);
        v->geoFenceManager()->sendToVehicle(base, polys, circles);
        QTRY_VERIFY_WITH_TIMEOUT(fenceSent.count() > 0, TestTimeout::longMs());

        QSignalSpy rallySent(v->rallyPointManager(), &RallyPointManager::sendComplete);
        v->rallyPointManager()->sendToVehicle({at(120, a + 60), at(200, a - 70)});
        QTRY_VERIFY_WITH_TIMEOUT(rallySent.count() > 0, TestTimeout::longMs());
    }
    for (int id : ids) {
        auto* e = _entryFor(id);
        qWarning() << "PREVIEW after-upload (no reload)" << e->label() << "mission" << e->missionCoordinates().size()
                   << "polys" << e->fencePolygons().size() << "circles" << e->fenceCircles().size() << "rally" << e->rallyPoints().size();
    }
    // Simulate the download-complete signals COP listens for, using the data the managers already hold.
    for (int id : ids) {
        auto* v = _entryFor(id)->vehicle();
        emit v->missionManager()->newMissionItemsAvailable(false);
        emit v->geoFenceManager()->loadComplete();
        emit v->rallyPointManager()->loadComplete();
    }
    QTest::qWait(1000);
    for (int id : ids) {
        auto* e = _entryFor(id);
        qWarning() << "PREVIEW after-reload" << e->label() << "mission" << e->missionCoordinates().size() << e->missionCoordinates()
                   << "polys" << e->fencePolygons().size() << "circles" << e->fenceCircles().size() << "rally" << e->rallyPoints().size();
        e->setPlanOverlayVisible(true);
    }
    controller->selectVehicle(0);
    QTest::qWait(1000);
    if (auto* map = findByName(_rootItem, QStringLiteral("copMap"))) {
        map->setProperty("zoomLevel", 15.2);
        map->setProperty("center", QVariant::fromValue(base));
    }
    QTest::qWait(2500);
    if (auto* map = findByName(_rootItem, QStringLiteral("copMap"))) {
        qWarning() << "PREVIEW map zoom" << map->property("zoomLevel") << "center" << map->property("center") << "size" << map->width() << map->height() << "visible" << map->isVisible();
        int n = 0;
        dumpMapItems(map, 0, n);
        qWarning() << "PREVIEW item count" << n;
    }
    const QString dir = QStringLiteral("/tmp/qgc-scratch/");
    qWarning() << "PREVIEW saved" << _window->grabWindow().save(dir + QStringLiteral("cop_overlay_all.png"));
    _entryFor(ids[1])->setPlanOverlayVisible(false);
    _entryFor(ids[2])->setPlanOverlayVisible(false);
    QTest::qWait(1500);
    qWarning() << "PREVIEW saved rover-only" << _window->grabWindow().save(dir + QStringLiteral("cop_overlay_rover_only.png"));
}
