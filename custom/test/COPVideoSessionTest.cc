#include "COPVideoSessionTest.h"

#include <memory>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QThread>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "COPTestHelpers.h"
#include "COPVideoSession.h"
#include "LinkManager.h"
#include "MockLink.h"
#include "QGCCorePlugin.h"
#include "SettingsManager.h"
#include "VehicleLinkManager.h"
#include "VideoManager.h"
#include "VideoSettings.h"
#include "VideoStreamController.h"
#include "VideoTestHelpers.h"

void COPVideoSessionTest::_staleReadiness_data()
{
    QTest::addColumn<QString>("change");
    QTest::newRow("vehicle removed") << QStringLiteral("vehicle");
    QTest::newRow("URI changed") << QStringLiteral("uri");
    QTest::newRow("output destroyed") << QStringLiteral("output");
    QTest::newRow("shutdown") << QStringLiteral("shutdown");
    QTest::newRow("session destroyed") << QStringLiteral("session");
}

void COPVideoSessionTest::_staleReadiness()
{
    QFETCH(QString, change);
    VideoTestHelpers::FactRestorer facts;
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);

    Vehicle vehicle(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER);
    QQuickWindow window;
    auto output = std::make_unique<QQuickItem>(window.contentItem());
    auto session = std::make_unique<COPVideoSession>();
    session->_refresh.stop();
    QList<std::function<void(bool)>> ready;
    session->_requestBackendReady = [&ready](QObject*, std::function<void(bool)> continuation) {
        ready.append(std::move(continuation));
    };
    int created = 0;
    session->_receiverFactory = [&created](QObject* owner) {
        ++created;
        return new FakeVideoReceiver(owner);
    };
    session->setVehicle(&vehicle);
    session->setOutput(output.get());
    session->setUri(QStringLiteral("udp://0.0.0.0:5600"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(ready.size(), 1);
    QCOMPARE(created, 0);

    // Delivery was queued before the change; cancellation must guard the callback itself.
    QMetaObject::invokeMethod(this, [continuation = ready.first()] { continuation(true); }, Qt::QueuedConnection);
    if (change == QStringLiteral("vehicle")) {
        session->setVehicle(nullptr);
    } else if (change == QStringLiteral("uri")) {
        session->setUri(QStringLiteral("udp://0.0.0.0:5601"));
    } else if (change == QStringLiteral("output")) {
        output.reset();
    } else if (change == QStringLiteral("shutdown")) {
        session->shutdown();
    } else {
        session.reset();
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 0);
    if (change == QStringLiteral("uri")) {
        QCOMPARE(ready.size(), 2);
        ready.last()(true);
        QCOMPARE(created, 1);
        QVERIFY(session->_receiver);
        QCOMPARE(session->_receiver->config().uri, QStringLiteral("udp://0.0.0.0:5601"));
    }
}

void COPVideoSessionTest::_receiverOwnsReconnectAndConfiguration()
{
    VideoTestHelpers::FactRestorer facts;
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);
    facts.set(settings->rtspAutoReconnect(), true);
    facts.set(settings->rtspTimeout(), 12);
    facts.set(settings->lowLatencyMode(), true);
    facts.set(settings->rtpJitterLatencyMs(), 200);

    Vehicle vehicle(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER);
    QQuickWindow window;
    QQuickItem output(window.contentItem());
    COPVideoSession session;
    session._refresh.stop();
    session._requestBackendReady = [](QObject*, std::function<void(bool)> continuation) { continuation(true); };
    int created = 0;
    QPointer<FakeVideoReceiver> receiver;
    session._receiverFactory = [&created, &receiver](QObject* owner) {
        ++created;
        receiver = new FakeVideoReceiver(owner);
        return receiver.data();
    };
    session.setVehicle(&vehicle);
    session.setOutput(&output);
    session.setUri(QStringLiteral("  rtsp://camera.local/live \t"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 1);
    QVERIFY(receiver);
    QCOMPARE(receiver->thread(), QCoreApplication::instance()->thread());
    QCOMPARE(receiver->videoOutput(), &output);
    QVERIFY(receiver->active());
    QCOMPARE(session.uri(), QStringLiteral("rtsp://camera.local/live"));
    QCOMPARE(receiver->config().uri, QStringLiteral("rtsp://camera.local/live"));
    QVERIFY(receiver->config().autoReconnect);
    QVERIFY(receiver->config().lowLatency);
    QCOMPARE(receiver->config().rtpJitterLatencyMs, 200);
    QCOMPARE(receiver->config().timeout, std::chrono::seconds(12));

    receiver->setState(VideoReceiver::State::Reconnecting);
    emit receiver->errorOccurred(QStringLiteral("Camera unavailable"));
    session._update();
    QCOMPARE(created, 1);
    QCOMPARE(receiver->state(), VideoReceiver::State::Reconnecting);
    QCOMPARE(receiver->restarts, 0);
    QCOMPARE(session.status(), QStringLiteral("Camera unavailable"));

    session.setUri(QStringLiteral("rtsp://camera.local/other"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 1);
    QCOMPARE(session._receiver.data(), receiver.data());
    QCOMPARE(receiver->config().uri, QStringLiteral("rtsp://camera.local/other"));
    receiver->setDecoding(true);
    QVERIFY(session.decoding());

    session.setVehicle(nullptr);
    QVERIFY(receiver.isNull());
    QVERIFY(!session.decoding());
}

void COPVideoSessionTest::_endpointOwnershipAndShutdown()
{
    VideoTestHelpers::FactRestorer facts;
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);

    Vehicle vehicle(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER);
    QQuickWindow window;
    QQuickItem output(window.contentItem());
    COPVideoSession first;
    COPVideoSession second;
    int created = 0;
    for (COPVideoSession* session : {&first, &second}) {
        session->_refresh.stop();
        session->_requestBackendReady = [](QObject*, std::function<void(bool)> continuation) { continuation(true); };
        session->_receiverFactory = [&created](QObject* owner) {
            ++created;
            return new FakeVideoReceiver(owner);
        };
        session->setVehicle(&vehicle);
        session->setOutput(&output);
    }
    first.setUri(QStringLiteral("udp://0.0.0.0:5600"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    second.setUri(QStringLiteral("udp265://127.0.0.1:5600"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 1);
    QVERIFY(first._receiver && first._receiver->active());
    QVERIFY(!second._receiver);

    const QPointer<VideoReceiver> receiver = first._receiver;
    bool destroyedBeforeQuit = false;
    connect(receiver, &QObject::destroyed, this, [&destroyedBeforeQuit] {
        destroyedBeforeQuit = QCoreApplication::instance() && !QCoreApplication::closingDown();
    });
    first.shutdown();
    QVERIFY(receiver.isNull());
    QVERIFY(destroyedBeforeQuit);
    second._update();
    QCOMPARE(created, 2);
    QVERIFY(second._receiver && second._receiver->active());
    first.setEnabled(false);
    first.setEnabled(true);
    first._update();
    QCOMPARE(created, 2);
    QVERIFY(!first._receiver);
    QVERIFY(!first._refresh.isActive());
}

void COPVideoSessionTest::_overviewHandoff()
{
    VideoTestHelpers::FactRestorer facts;
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);
    facts.set(settings->videoSource(), QString(VideoSettings::videoSourceMPEGTS));
    facts.set(settings->udpUrl(), QStringLiteral("0.0.0.0:5602"));

    Vehicle vehicle(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER);
    QQuickWindow window;
    QQuickItem output(window.contentItem());
    COPVideoSession playing;
    COPVideoSession pending;
    int created = 0;
    QList<std::function<void(bool)>> readiness;
    for (COPVideoSession* session : {&playing, &pending}) {
        session->_refresh.stop();
        session->_receiverFactory = [&created](QObject* owner) {
            ++created;
            return new FakeVideoReceiver(owner);
        };
        session->setVehicle(&vehicle);
        session->setOutput(&output);
    }
    playing._requestBackendReady = [](QObject*, std::function<void(bool)> continuation) { continuation(true); };
    pending._requestBackendReady = [&readiness](QObject*, std::function<void(bool)> continuation) {
        readiness.append(std::move(continuation));
    };
    playing.setUri(QStringLiteral("mpegts://0.0.0.0:5602"));
    pending.setUri(QStringLiteral("udp://0.0.0.0:5603"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 1);
    QVERIFY(playing._receiver && playing._receiver->active());
    QCOMPARE(readiness.size(), 1);
    const QPointer<VideoReceiver> tileReceiver = playing._receiver;

    auto* manager = VideoManager::instance();
    const auto cleanupVideo = qScopeGuard([manager] { manager->cleanup(); });
    manager->stopVideo();
    auto* primary = new FakeVideoReceiver;
    manager->primaryStream()->setReceiver(primary);
    VehicleRoleController roles;
    roles.addEntry(201, QStringLiteral("Rover"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);
    bool releasedAtSelection = false;
    connect(&controller, &COPController::selectionChanged, this,
            [&] { releasedAtSelection = tileReceiver.isNull() && playing._endpoint.isEmpty() && !pending.enabled(); });

    controller.selectVehicle(201);
    QVERIFY(releasedAtSelection);
    QVERIFY(tileReceiver.isNull());
    QVERIFY(!playing.enabled());
    QVERIFY(!pending.enabled());
    readiness.first()(true);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 1);
    QVERIFY(!pending._receiver);
    QVERIFY(primary->active());
    QCOMPARE(primary->config().uri, QStringLiteral("mpegts://0.0.0.0:5602"));

    // The tile still resolves its own URI, even when the standard controller has another override.
    controller.selectVehicle(0);
    QVERIFY(!primary->active());
    manager->primaryStream()->setUriOverride(QStringLiteral("rtsp://other/stock-only"));
    playing.setEnabled(true);
    playing._update();
    QCOMPARE(created, 2);
    QVERIFY(playing._receiver && playing._receiver->active());
    QCOMPARE(playing._receiver->config().uri, QStringLiteral("mpegts://0.0.0.0:5602"));
    playing.setUri(QStringLiteral(" \n\t "));
    playing._update();
    QVERIFY(!playing._receiver);
}

UT_REGISTER_TEST(COPVideoSessionTest, TestLabel::Unit)

void COPVideoPreviewTest::init()
{
    VehicleTestManualConnect::init();
    QVERIFY(_directory.isValid());
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
    _engine = QGCCorePlugin::instance()->createQmlApplicationEngine(nullptr);
    QVERIFY(_engine);
}

void COPVideoPreviewTest::cleanup()
{
    if (_engine) {
        QGCCorePlugin::instance()->destroyQmlApplicationEngine(_engine);
        _engine = nullptr;
    }
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    VehicleTestManualConnect::cleanup();
}

void COPVideoPreviewTest::_disconnectAndReconnect_data()
{
    QTest::addColumn<quint32>("requestedSysid");
    QTest::newRow("legacy") << quint32(128);
    QTest::newRow("wide") << quint32(4294967295U);
}

void COPVideoPreviewTest::_disconnectAndReconnect()
{
    QFETCH(quint32, requestedSysid);
    VideoTestHelpers::FactRestorer facts;
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->videoSource(), QString(VideoSettings::videoSourceRTSP));
    facts.set(settings->rtspUrl(), QStringLiteral("rtsp://camera/manual"));
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);
    auto* controller = _engine->singletonInstance<COPController*>("QGC", "COPController");
    QVERIFY(controller);
    VehicleRoleController roles;
    controller->initialize(&roles);
    const auto reconnect = [this, requestedSysid]() {
        _mockLink = COPTestHelpers::connectMockLink(requestedSysid);
        if (!_mockLink || !waitForCondition([] { return MultiVehicleManager::instance()->activeVehicle(); },
                                            TestTimeout::longMs(), QStringLiteral("activeVehicle"))) {
            return false;
        }
        _vehicle = MultiVehicleManager::instance()->activeVehicle();
        return waitForInitialConnect();
    };
    QVERIFY(reconnect());
    QVERIFY(vehicle());
    QCOMPARE(vehicle()->id(), requestedSysid);
    controller->selectVehicle(requestedSysid);
    auto* entry = controller->selected();
    QVERIFY(entry);
    QVERIFY(entry->connected());
    QCOMPARE(entry->vehicle(), vehicle());

    QQuickWindow window;
    VideoManager manager;
    const auto cleanupVideo = qScopeGuard([&manager] {
        manager.cleanup();
        VideoManager::instance()->stopVideo();
    });
    auto* primary = new FakeVideoReceiver;
    manager.primaryStream()->setReceiver(primary);
    manager.thermalStream()->setReceiver(new FakeVideoReceiver);
    manager.init(&window);
    entry->setVideoUri(QStringLiteral("rtsp://relay/reconnected"));
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtsp://relay/reconnected"),
                              TestTimeout::shortMs());
    QVERIFY(manager.autoStreamConfigured());
    QPointer<Vehicle> previousVehicle = vehicle();
    _disconnectMockLink();
    QTRY_VERIFY_WITH_TIMEOUT(previousVehicle.isNull(), TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtsp://camera/manual"), TestTimeout::shortMs());
    QVERIFY(!manager.autoStreamConfigured());
    QVERIFY(!entry->connected());
    QVERIFY(!entry->vehicle());
    QVERIFY(reconnect());
    QVERIFY(vehicle());
    QCOMPARE(vehicle()->id(), requestedSysid);
    QCOMPARE(controller->selected(), entry);
    QCOMPARE(entry->vehicle(), vehicle());
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtsp://relay/reconnected"),
                              TestTimeout::shortMs());
    QVERIFY(manager.autoStreamConfigured());
}

void COPVideoPreviewTest::_videoOverrideFollowsActiveVehicle()
{
    VideoTestHelpers::FactRestorer facts;
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->videoSource(), QString(VideoSettings::videoSourceRTSP));
    facts.set(settings->rtspUrl(), QStringLiteral("rtsp://camera/manual"));
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);

    _connectMockLink();
    QVERIFY(vehicle());
    Vehicle* first = vehicle();
    const quint32 secondId = first->id() == 201 ? 202 : 201;
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("COP second camera"));
    config->setSystemId(secondId);
    config->setIncrementVehicleId(false);
    config->setDynamic(true);
    SharedLinkConfigurationPtr sharedConfig = config;
    expectAppMessage(QRegularExpression(QStringLiteral(" - \"Connected to Vehicle %1\"$").arg(secondId)));
    QVERIFY(LinkManager::instance()->createConnectedLink(sharedConfig));
    QPointer<MockLink> secondLink = qobject_cast<MockLink*>(config->link());
    auto* vehicles = MultiVehicleManager::instance();
    const auto disconnectSecond = qScopeGuard([&secondLink, vehicles, secondId] {
        if (secondLink) {
            secondLink->disconnect();
        }
        QVERIFY(QTest::qWaitFor([vehicles, secondId] { return !vehicles->getVehicleById(secondId); },
                                TestTimeout::longMs()));
    });
    QVERIFY(secondLink);
    QTRY_VERIFY_WITH_TIMEOUT(vehicles->getVehicleById(secondId), TestTimeout::longMs());
    Vehicle* second = vehicles->getVehicleById(secondId);
    QVERIFY(second);
    QTRY_VERIFY_WITH_TIMEOUT(second->isInitialConnectComplete(), TestTimeout::longMs());
    verifyExpectedLogMessage();
    vehicles->setActiveVehicle(first);
    QTRY_COMPARE_WITH_TIMEOUT(vehicles->activeVehicle(), first, TestTimeout::mediumMs());

    auto* plugin = QGCCorePlugin::instance();
    auto* controller = _engine->singletonInstance<COPController*>("QGC", "COPController");
    QVERIFY(controller);
    VehicleRoleController roles;
    controller->initialize(&roles);
    controller->selectVehicle(first->id());
    COPVehicle* firstEntry = controller->selected();
    QVERIFY(firstEntry);
    firstEntry->setVideoUri(QStringLiteral("  rtsp://relay/vehicleA  "));
    controller->selectVehicle(secondId);
    COPVehicle* secondEntry = controller->selected();
    QVERIFY(secondEntry);
    secondEntry->setVideoUri(QStringLiteral("rtspt://relay/vehicleB"));
    QCOMPARE(vehicles->activeVehicle(), first);
    QCOMPARE(plugin->primaryVideoUriOverride(first), QStringLiteral("rtsp://relay/vehicleA"));

    QQuickWindow window;
    VideoManager manager;
    const auto cleanupVideo = qScopeGuard([&manager] {
        manager.cleanup();
        VideoManager::instance()->stopVideo();
    });
    auto* primary = new FakeVideoReceiver;
    auto* thermal = new FakeVideoReceiver;
    manager.primaryStream()->setReceiver(primary);
    manager.thermalStream()->setReceiver(thermal);
    manager.init(&window);
    const auto primaryInfo = VideoTestHelpers::makeStreamInfo(VIDEO_STREAM_TYPE_RTSP, "rtsp://camera/advertised");
    const auto thermalInfo = VideoTestHelpers::makeStreamInfo(VIDEO_STREAM_TYPE_RTSP, "rtsp://camera/thermal",
                                                              VIDEO_STREAM_ENCODING_H264, true);
    manager.primaryStream()->setStreamInfo(primaryInfo.get());
    manager.thermalStream()->setStreamInfo(thermalInfo.get());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"));
    QCOMPARE(thermal->config().uri, QStringLiteral("rtsp://camera/thermal"));
    QVERIFY(primary->active());
    QVERIFY(thermal->active());
    QVERIFY(manager.autoStreamConfigured());
    facts.set(settings->videoSource(), QString(VideoSettings::videoDisabled));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(primary->active());
    QVERIFY(manager.hasVideo());
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"));
    facts.set(settings->videoSource(), QString(VideoSettings::videoSourceRTSP));

    QList<QSharedPointer<QSignalSpy>> settingSpies;
    for (Fact* fact : {settings->videoSource(), settings->udpUrl(), settings->rtspUrl(), settings->tcpUrl()}) {
        settingSpies.append(QSharedPointer<QSignalSpy>::create(fact, &Fact::rawValueChanged));
    }
    QSignalSpy assignmentChanged(plugin, &QGCCorePlugin::primaryVideoUriOverrideChanged);
    firstEntry->setVideoUri(QStringLiteral("rtspt://relay/editedA"));
    QCOMPARE(assignmentChanged.count(), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(primary->config().uri, QStringLiteral("rtspt://relay/editedA"));
    QCOMPARE(thermal->config().uri, QStringLiteral("rtsp://camera/thermal"));
    secondEntry->setVideoUri(QStringLiteral("rtsp://relay/inactiveEdit"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(primary->config().uri, QStringLiteral("rtspt://relay/editedA"));
    secondEntry->setVideoUri(QStringLiteral("rtspt://relay/vehicleB"));

    firstEntry->setVideoUri(QStringLiteral(" \n\t "));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://camera/advertised"));
    manager.primaryStream()->setStreamInfo(nullptr);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://camera/manual"));
    QVERIFY(!manager.autoStreamConfigured());

    // Highlight A while B becomes active; neither assignment is an advertisement.
    firstEntry->setVideoUri(QStringLiteral("rtsp://relay/vehicleA"));
    controller->selectVehicle(first->id());
    vehicles->setActiveVehicle(second);
    QTRY_COMPARE_WITH_TIMEOUT(vehicles->activeVehicle(), second, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtspt://relay/vehicleB"), TestTimeout::shortMs());
    QVERIFY(manager.autoStreamConfigured());
    QVERIFY(manager.isStreamSource());
    QVERIFY(manager.hasVideo());
    QVERIFY(!thermal->active());
    vehicles->setActiveVehicle(first);
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"), TestTimeout::mediumMs());

    vehicles->setActiveVehicle(nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!vehicles->activeVehicle(), TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtsp://camera/manual"), TestTimeout::shortMs());
    QVERIFY(plugin->primaryVideoUriOverride(nullptr).isEmpty());
    QVERIFY(!manager.autoStreamConfigured());
    vehicles->setActiveVehicle(first);
    QTRY_COMPARE_WITH_TIMEOUT(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"), TestTimeout::mediumMs());

    bool disconnectedOverrideEmpty = false;
    connect(vehicles, &MultiVehicleManager::vehicleRemoved, &manager, [&](Vehicle* removed) {
        if (removed == second) {
            disconnectedOverrideEmpty = plugin->primaryVideoUriOverride(removed).isEmpty();
        }
    });
    secondLink->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(!vehicles->getVehicleById(secondId), TestTimeout::longMs());
    QVERIFY(disconnectedOverrideEmpty);
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    const int configChanges = primary->configChanges;
    const int restarts = primary->restarts;
    const int activeChanges = primary->activeChanges;
    simulateCommLoss(true);
    QTRY_VERIFY_WITH_TIMEOUT(first->vehicleLinkManager()->communicationLost(), TestTimeout::longMs());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(plugin->primaryVideoUriOverride(first), QStringLiteral("rtsp://relay/vehicleA"));
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"));
    QCOMPARE(primary->configChanges, configChanges);
    QCOMPARE(primary->restarts, restarts);
    QCOMPARE(primary->activeChanges, activeChanges);
    QVERIFY(primary->active());
    QVERIFY(manager.autoStreamConfigured());
    simulateCommLoss(false);
    QTRY_VERIFY_WITH_TIMEOUT(!first->vehicleLinkManager()->communicationLost(), TestTimeout::longMs());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(plugin->primaryVideoUriOverride(first), QStringLiteral("rtsp://relay/vehicleA"));
    QCOMPARE(primary->config().uri, QStringLiteral("rtsp://relay/vehicleA"));
    QCOMPARE(primary->configChanges, configChanges);
    QCOMPARE(primary->restarts, restarts);
    QCOMPARE(primary->activeChanges, activeChanges);
    QVERIFY(primary->active());
    QVERIFY(manager.autoStreamConfigured());
    QCOMPARE(settings->rtspUrl()->rawValue().toString(), QStringLiteral("rtsp://camera/manual"));
    for (const auto& spy : std::as_const(settingSpies)) {
        QCOMPARE(spy->count(), 0);
    }
}

UT_REGISTER_TEST(COPVideoPreviewTest, TestLabel::Integration, TestLabel::Vehicle)
