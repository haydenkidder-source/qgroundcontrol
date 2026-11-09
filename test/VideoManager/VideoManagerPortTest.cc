#include "VideoManagerPortTest.h"

#include <memory>

#include <QtCore/QPromise>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlApplicationEngine>

#include "AppSettings.h"
#include "COPController.h"
#include "GStreamer/GStreamerTestHelpers.h"
#include "QGCCorePlugin.h"
#include "SettingsManager.h"
#include "VehicleRoleController.h"
#include "VideoManager.h"
#include "VideoSettings.h"
#include "VideoStreamController.h"
#include "VideoTestHelpers.h"

void VideoManagerPortTest::init()
{
    UnitTest::init();
#ifdef QGC_GST_STREAMING
    // Same uninitialized-registry fixture as VideoManagerInitTest; no backend or GPU is started.
    ignoreLogMessage("Video.GStreamer.GStreamerLogging", QtCriticalMsg, GstTest::glibTypeRegistrationCriticals());
#endif
}

void VideoManagerPortTest::_gateAtInitAndActivation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    VideoTestHelpers::FactRestorer facts;
    facts.set(SettingsManager::instance()->appSettings()->savePath(), directory.path());
    auto* settings = SettingsManager::instance()->videoSettings();
    facts.set(settings->videoSource(), QString(VideoSettings::videoSourceUDPH264));
    facts.set(settings->udpUrl(), QStringLiteral("0.0.0.0:5600"));
    facts.set(settings->streamEnabled(), true);
    facts.set(settings->disableWhenDisarmed(), false);

    auto* plugin = QGCCorePlugin::instance();
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    auto* controller = engine->singletonInstance<COPController*>("QGC", "COPController");
    QVERIFY(controller);
    VehicleRoleController roles;
    roles.addEntry(201, QStringLiteral("Rover"), QStringLiteral("Video test"), 0);
    controller->initialize(&roles);
    controller->selectVehicle(0);
    QVERIFY(!plugin->startStandardVideoReceivers());

    VideoManager manager;
    int created = 0;
    manager._receiverFactory = [&created](QObject* owner) {
        ++created;
        return new FakeVideoReceiver(owner);
    };
    manager._initState = VideoManager::InitState::Pending;
    manager._onMainWindowReady();
    manager._onBackendInitComplete(true);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 0);
    QVERIFY(!manager.primaryStream()->receiver());

    // A selection after initialization can create and activate the standard streams.
    controller->selectVehicle(201);
    QVERIFY(plugin->startStandardVideoReceivers());
    manager.startVideo();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 2);
    auto* primary = qobject_cast<FakeVideoReceiver*>(manager.primaryStream()->receiver());
    QVERIFY(primary && primary->active());

    // A settings apply queued while allowed cannot start the stream once COP owns the endpoint.
    primary->setState(VideoReceiver::State::Reconnecting);
    controller->selectVehicle(0);
    facts.set(settings->lowLatencyMode(), !settings->lowLatencyMode()->rawValue().toBool());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(!primary->active());
    QVERIFY(!manager.thermalStream()->active());
    QCOMPARE(created, 2);

    // Neither can an activation request.
    manager.startVideo();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(!primary->active());
    QCOMPARE(created, 2);

    controller->selectVehicle(201);
    manager.startVideo();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(primary->active());
    QCOMPARE(created, 2);
    manager.cleanup();
    VideoManager::instance()->stopVideo();
}

void VideoManagerPortTest::_whenReadyDeliversQueuedOnceOnTheGuiThread()
{
    VideoManager manager;
    manager._initState = VideoManager::InitState::Pending;
    QPromise<bool> backend;
    backend.start();
    manager._watchBackendInit(backend.future());

    int delivered = 0;
    manager.whenVideoBackendReady(this, [&delivered](bool ready) { delivered += ready ? 1 : 100; });
    QCOMPARE(delivered, 0);

    backend.addResult(true);
    backend.finish();
    QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, TestTimeout::shortMs());

    // Once initialized, a request is still delivered from the event loop and never inline.
    int late = 0;
    manager.whenVideoBackendReady(this, [&late](bool) { ++late; });
    QCOMPARE(late, 0);
    QTRY_COMPARE_WITH_TIMEOUT(late, 1, TestTimeout::shortMs());
    QCOMPARE(delivered, 1);
    manager.cleanup();
}

void VideoManagerPortTest::_whenReadyReportsFailureAndHonoursItsContext()
{
    VideoManager manager;
    manager._initState = VideoManager::InitState::Pending;

    int cancelled = 0;
    auto context = std::make_unique<QObject>();
    manager.whenVideoBackendReady(context.get(), [&cancelled](bool) { ++cancelled; });
    int delivered = 0;
    bool ready = true;
    manager.whenVideoBackendReady(this, [&delivered, &ready](bool success) {
        ++delivered;
        ready = success;
    });
    // Destroying the context cancels its delivery without affecting other waiters.
    context.reset();

    expectLogMessage("Video.VideoManager", QtCriticalMsg,
                     QRegularExpression(QStringLiteral("video initialization failed")));
    manager._onBackendInitComplete(false);
    QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, TestTimeout::shortMs());
    verifyExpectedLogMessage();
    QVERIFY(!ready);
    QCOMPARE(cancelled, 0);
    manager.cleanup();
}

UT_REGISTER_TEST(VideoManagerPortTest, TestLabel::Unit)
