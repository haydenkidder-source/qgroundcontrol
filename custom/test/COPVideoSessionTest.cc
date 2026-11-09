#include "COPVideoSessionTest.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtQuick/QQuickWindow>

#include "COPVideoSession.h"
#include "SettingsManager.h"
#include "VideoSettings.h"
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
    session.setUri(QStringLiteral("rtsp://camera.local/live"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(created, 1);
    QVERIFY(receiver);
    QCOMPARE(receiver->thread(), QCoreApplication::instance()->thread());
    QCOMPARE(receiver->videoOutput(), &output);
    QVERIFY(receiver->active());
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

UT_REGISTER_TEST(COPVideoSessionTest, TestLabel::Unit)
