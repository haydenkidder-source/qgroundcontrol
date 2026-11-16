#include "COPSerialNoticesUITest.h"

#include <cstdlib>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "AutoConnectSettings.h"
#include "COPController.h"
#include "COPTestHelpers.h"
#include "Fact.h"
#include "FirmwareUpgradeSettings.h"
#include "LinkManager.h"
#include "MultiVehicleManager.h"
#include "QGCCorePlugin.h"
#include "QGCPalette.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "VehicleLinkManager.h"
#ifndef QGC_NO_SERIAL_LINK
#include "LinkManagerTest.h"
#include "SerialLink.h"
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <fcntl.h>
#include <unistd.h>
#endif

UT_REGISTER_TEST(COPSerialNoticesUITest, TestLabel::Integration, TestLabel::Comms)

#ifndef QGC_NO_SERIAL_LINK
namespace {
QString serialUiRowName(const QString& id)
{
    return QStringLiteral("copSerialNotice_") + id;
}

QString serialUiActionName(const QString& id, const QString& action)
{
    return QStringLiteral("copSerialAction_%1_%2").arg(id, action);
}

QStringList serialUiButtons(QQuickItem* item)
{
    QStringList result;
    if (!item) {
        return result;
    }
    if (item->isVisible() && item->objectName().startsWith(QStringLiteral("copSerialAction_"))) {
        result.append(item->property("text").toString());
    }
    for (auto* child : item->childItems()) {
        result.append(serialUiButtons(child));
    }
    return result;
}
}  // namespace
#endif

void COPSerialNoticesUITest::init()
{
    UnitTest::init();
    QSettings settings;
    for (const auto& key : settings.allKeys()) {
        _settings.insert(key, settings.value(key));
    }
    auto* app = SettingsManager::instance()->appSettings();
    auto* automatic = SettingsManager::instance()->autoConnectSettings();
    const QList<Fact*> facts{app->savePath(),
                             app->audioMuted(),
                             automatic->autoConnectPixhawk(),
                             automatic->autoConnectSiKRadio(),
                             automatic->autoConnectLibrePilot(),
                             SettingsManager::instance()->firmwareUpgradeSettings()->defaultFirmwareType()};
    for (auto* fact : facts) {
        _facts.append({fact, fact->rawValue()});
    }
    QVERIFY(_directory.isValid());
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    app->savePath()->setRawValue(_directory.path());
    app->audioMuted()->setRawValue(true);
    automatic->autoConnectPixhawk()->setRawValue(true);
    automatic->autoConnectSiKRadio()->setRawValue(true);
    automatic->autoConnectLibrePilot()->setRawValue(true);
    settings.remove(QStringLiteral("COP"));
    LinkManager::instance()->init();
    LinkManager::instance()->setConnectionsAllowed();
    QVERIFY(!LinkManager::instance()->connectionsSuspended());
#ifndef QGC_NO_SERIAL_LINK
    _nowUs = 1;
    _previousClock = LinkManagerTest::exchangeSerialNoticeClock(*LinkManager::instance(), [this] { return _nowUs; });
#endif
}

void COPSerialNoticesUITest::cleanup()
{
    auto* manager = LinkManager::instance();
    for (const auto& config : std::as_const(_configs)) {
        manager->disconnectLinkConfiguration(config.get());
        manager->removeConfiguration(config.get());
    }
    // Teardown must settle even after an assertion failed; QSignalSpy::wait cannot do that.
    const bool vehiclesGone =
        QTest::qWaitFor([] { return MultiVehicleManager::instance()->vehicles()->isEmpty(); }, TestTimeout::longMs());
    _configs.clear();
#ifndef QGC_NO_SERIAL_LINK
    _links.clear();
    _claims.clear();
    _ports.clear();
    LinkManagerTest::observeSerialPorts(*manager, {});
    if (_previousClock) {
        (void) LinkManagerTest::exchangeSerialNoticeClock(*manager, std::move(_previousClock));
    }
#endif
    stopUI();
    // Closing MainWindow suspends connections through LinkManager::shutdown().
    manager->setConnectionsAllowed();
    for (const auto& saved : std::as_const(_facts)) {
        saved.first->setRawValue(saved.second);
    }
    _facts.clear();
    QSettings settings;
    settings.clear();
    for (auto it = _settings.cbegin(); it != _settings.cend(); ++it) {
        settings.setValue(it.key(), it.value());
    }
    _settings.clear();
    QmlUITestBase::cleanup();
    QVERIFY(vehiclesGone);
}

COPController* COPSerialNoticesUITest::_controller() const
{
    return _engine ? _engine->singletonInstance<COPController*>("QGC", "COPController") : nullptr;
}

QQuickItem* COPSerialNoticesUITest::_footer() const
{
    return findItem(_rootItem, QStringLiteral("copNotifications"));
}

void COPSerialNoticesUITest::_startFooter()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    QTRY_VERIFY_WITH_TIMEOUT(_footer(), TestTimeout::mediumMs());
    // Loader existence precedes the host context assignment.
    QTRY_COMPARE_WITH_TIMEOUT(_footer()->property("hostWindow").value<QObject*>(), _window, TestTimeout::shortMs());
    QVERIFY(_controller());
}

bool COPSerialNoticesUITest::_clickStable(const QString& name)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (!findVisibleItem(_rootItem, name, TestTimeout::shortMs())) {
        return false;
    }
    return clickButtonScrolled(name, QStringLiteral("copSerialNoticesView"));
}

#ifndef QGC_NO_SERIAL_LINK
QQuickItem* COPSerialNoticesUITest::_liveRow(const QString& id)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return findVisibleItem(_rootItem, serialUiRowName(id), TestTimeout::shortMs());
}

void COPSerialNoticesUITest::_narrowFooter(const QString& id)
{
    QPointer<QQuickItem> row = _liveRow(id);
    QVERIFY(row);
    QPointer<QQuickItem> text = findItem(row, QStringLiteral("copSerialNoticeText_") + id);
    QVERIFY(text);
    QList<QPointer<QQuickItem>> buttons;
    qreal widest = 0;
    for (const auto& action :
         {QStringLiteral("OpenGnssReceiver"), QStringLiteral("OpenCommLinks"), QStringLiteral("Dismiss")}) {
        if (auto* button = findItem(row, serialUiActionName(id, action))) {
            QTRY_VERIFY_WITH_TIMEOUT(button->width() > 0, TestTimeout::shortMs());
            widest = qMax(widest, button->width());
            buttons.append(button);
        }
    }
    QVERIFY(buttons.size() >= 2);
    const int originalLines = text->property("lineCount").toInt();
    const qreal narrowWidth = widest + _footer()->width() - row->width();
    QVERIFY(narrowWidth < _footer()->width());
    auto* loader = _footer()->parentItem();
    QVERIFY(loader);
    // Override the host binding: Loader resizes its item again when wrapped content changes height.
    // Each data row creates a fresh engine, so this width needs no restoration.
    QVERIFY(QQmlProperty(loader, QStringLiteral("width")).write(narrowWidth));
    for (auto* ancestor = text.data(); ancestor; ancestor = ancestor->parentItem()) {
        ancestor->ensurePolished();
    }
    QTRY_COMPARE_WITH_TIMEOUT(_footer()->width(), narrowWidth, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(row && qAbs(row->width() - widest) < 1.0, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(text && text->width() == row->width(), TestTimeout::shortMs());
    auto* flow = buttons.first()->parentItem();
    QVERIFY(flow);
    QTRY_COMPARE_WITH_TIMEOUT(flow->width(), row->width(), TestTimeout::shortMs());
    QVERIFY(QMetaObject::invokeMethod(flow, "forceLayout"));
    row->ensurePolished();
    QTRY_VERIFY_WITH_TIMEOUT(text && text->property("lineCount").toInt() > originalLines, TestTimeout::shortMs());
    for (qsizetype index = 1; index < buttons.size(); ++index) {
        const auto& previous = buttons.at(index - 1);
        const auto& button = buttons.at(index);
        QVERIFY(previous && button);
        QVERIFY(previous->width() + button->width() > row->width());
        QVERIFY(button->mapToScene(QPointF()).y() > previous->mapToScene(QPointF()).y());
    }
    QCOMPARE(loader->width(), narrowWidth);
    QCOMPARE(_footer()->width(), narrowWidth);
}

SharedLinkConfigurationPtr COPSerialNoticesUITest::_configuration(const QString& path)
{
    auto* serial = new SerialConfiguration(QStringLiteral("UI radio %1").arg(_configs.size()));
    serial->setPortName(path.isEmpty() ? QStringLiteral("/test/cop-serial-%1").arg(_configs.size()) : path);
    serial->setAutoConnect(true);
    serial->setAutoConnectStarted(true);
    auto config = LinkManager::instance()->addConfiguration(serial);
    _configs.append(config);
    return config;
}

SerialConnectionNotice* COPSerialNoticesUITest::_raise(const QString& trigger)
{
    auto& manager = *LinkManager::instance();
    auto config = _configuration();
    SerialPortManager::Port port;
    port.systemLocation = static_cast<SerialConfiguration*>(config.get())->portName();
    port.portName = port.systemLocation;
    port.boardName = QStringLiteral("UI device");
    port.description = QStringLiteral("USB telemetry");
    if (trigger == QStringLiteral("class_disabled")) {
        port.boardType = QGCSerialPortInfo::BoardTypePixhawk;
        SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->setRawValue(false);
    } else if (trigger == QStringLiteral("bootloader")) {
        port.boardType = QGCSerialPortInfo::BoardTypePixhawk;
        port.bootloader = true;
    }
    _ports.append(port);
    LinkManagerTest::observeSerialPorts(manager, _ports);
    if (trigger.startsWith(QStringLiteral("owner"))) {
        const auto kind = trigger.contains(QStringLiteral("gnss")) ? SerialPortManager::OwnerKind::GPSReceiver
                                                                   : SerialPortManager::OwnerKind::MAVLink;
        auto claim = SerialPortManager::instance()->reservePort(port.systemLocation, kind, QStringLiteral("UI owner"));
        if (!claim) {
            return nullptr;
        }
        _claims.append(claim);
        if (manager.createConnectedLink(config)) {
            return nullptr;
        }
    } else if (trigger == QStringLiteral("auto_connect_off") || trigger == QStringLiteral("suppressed") ||
               trigger == QStringLiteral("never_started") || trigger == QStringLiteral("connections_suspended") ||
               trigger == QStringLiteral("bootloader") || trigger == QStringLiteral("still_unavailable")) {
        config->setAutoConnect(trigger != QStringLiteral("auto_connect_off"));
        config->setAutoConnectStarted(trigger != QStringLiteral("never_started"));
        config->setSuppressAutoReconnect(trigger == QStringLiteral("suppressed"));
        auto link = LinkManagerTest::_serialNoticeLink(manager, config);
        _links.append(link);
        LinkManagerTest::reportSerialConnected(link.get());
        if (trigger == QStringLiteral("connections_suspended")) {
            manager.setConnectionsSuspended(QStringLiteral("UI firmware update"));
        }
        link->disconnect();
    } else if (trigger != QStringLiteral("class_disabled")) {
        const auto stage = trigger == QStringLiteral("configuration_failed") ? SerialOpenOutcome::Stage::Configuration
                                                                             : SerialOpenOutcome::Stage::Open;
        const auto error = trigger == QStringLiteral("device_not_found") ? QSerialPort::DeviceNotFoundError
                           : trigger == QStringLiteral("access_denied")  ? QSerialPort::PermissionError
                                                                         : QSerialPort::OpenError;
        LinkManagerTest::reportSerialOpenFailure(manager, config.get(),
                                                 {stage, error, QStringLiteral("native <detail> & error"), true});
    }
    if (trigger == QStringLiteral("still_unavailable") || trigger.endsWith(QStringLiteral("escalated"))) {
        _nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US;
        LinkManagerTest::observeSerialPorts(manager, _ports);
    }
    return manager.serialNotices()->isEmpty()
               ? nullptr
               : manager.serialNotices()->value<SerialConnectionNotice*>(manager.serialNotices()->count() - 1);
}
#endif

void COPSerialNoticesUITest::_noticeRows_data()
{
    QTest::addColumn<QString>("trigger");
    QTest::addColumn<QString>("reason");
    QTest::addColumn<QString>("fragment");
    QTest::addColumn<bool>("gnss");
    const auto row = [](const char* trigger, const char* reason, const char* fragment, bool gnss = false) {
        QTest::newRow(trigger) << QString::fromLatin1(trigger) << QString::fromLatin1(reason)
                               << QString::fromLatin1(fragment) << gnss;
    };
    row("auto_connect_off", "auto_connect_off", "Automatic retry for this saved link is off.");
    row("suppressed", "suppressed", "paused or has not been started this session");
    row("never_started", "never_started", "paused or has not been started this session");
    row("connections_suspended", "connections_suspended", "UI firmware update");
    row("class_disabled", "class_disabled", "Automatically connect to a Pixhawk board");
    row("bootloader", "bootloader", "finish starting");
    row("access_denied", "access_denied", "native <detail> & error");
    row("device_not_found", "device_not_found", "which is not detected");
    row("open_failed", "open_failed", "cannot open");
    row("configuration_failed", "configuration_failed", "cannot configure");
    row("owner-mavlink", "reservation_failed", "QGroundControl link 'UI owner' owns it");
    row("owner-gnss", "reservation_failed", "GNSS receiver 'UI owner'", true);
    row("still_unavailable", "still_unavailable", "is still unavailable on");
    row("owner-gnss-escalated", "still_unavailable", "GNSS receiver 'UI owner'", true);
}

void COPSerialNoticesUITest::_noticeRows()
{
#ifdef QGC_NO_SERIAL_LINK
    QSKIP("Serial links are disabled");
#else
    QFETCH(QString, trigger);
    QFETCH(QString, reason);
    QFETCH(QString, fragment);
    QFETCH(bool, gnss);
    // Startup replay must render an incident which predates the QML engine.
    auto* notice = _raise(trigger);
    QVERIFY(notice);
    QCOMPARE(notice->reason(), reason);
    QVERIFY(notice->text().contains(fragment));
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    QTRY_VERIFY_WITH_TIMEOUT(_footer()->height() > 0, TestTimeout::shortMs());
    const QString id = notice->id();
    QVERIFY(verifyText(QStringLiteral("copNotificationHeading"), QStringLiteral("Notifications (1)"), trigger));
    auto* row = _liveRow(id);
    QVERIFY(row);
    auto* text = findVisibleItem(row, QStringLiteral("copSerialNoticeText_") + id);
    QVERIFY(text);
    QCOMPARE(text->property("text").toString(), notice->text());
    QCOMPARE(text->property("textFormat").toInt(), int(Qt::PlainText));
    QTRY_VERIFY_WITH_TIMEOUT(text->property("lineCount").toInt() > 1, TestTimeout::shortMs());
    const QStringList expected = gnss ? QStringList{QStringLiteral("Open GNSS Receiver"),
                                                    QStringLiteral("Open Comm Links"), QStringLiteral("Dismiss")}
                                      : QStringList{QStringLiteral("Open Comm Links"), QStringLiteral("Dismiss")};
    QTRY_COMPARE_WITH_TIMEOUT(serialUiButtons(row), expected, TestTimeout::shortMs());
    _narrowFooter(id);
    QVERIFY(!QTest::currentTestFailed());
    QSignalSpy alerts(_controller(), &COPController::attentionRequested);
    LinkManagerTest::observeSerialPorts(*LinkManager::instance(), _ports);
    QCOMPARE(alerts.count(), 0);
    QVERIFY(notice->active());
    QCOMPARE(notice->reason(), reason);
    row = _liveRow(id);
    QVERIFY(row);
    QTRY_COMPARE_WITH_TIMEOUT(serialUiButtons(row), expected, TestTimeout::shortMs());
    QVERIFY(_clickStable(serialUiActionName(id, QStringLiteral("Dismiss"))));
    QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(id), 0), TestTimeout::shortMs());
    QVERIFY(notice->active());
    QVERIFY(notice->acknowledged());
    QTRY_COMPARE_WITH_TIMEOUT(_footer()->height(), 0.0, TestTimeout::shortMs());
    QCOMPARE(alerts.count(), 0);
#endif
}

void COPSerialNoticesUITest::_navigation_data()
{
    QTest::addColumn<QString>("action");
    QTest::addColumn<QString>("page");
    QTest::newRow("comm-links") << QStringLiteral("OpenCommLinks") << QStringLiteral("Comm Links");
    QTest::newRow("gnss-receiver") << QStringLiteral("OpenGnssReceiver") << QStringLiteral("GNSS Receiver");
}

void COPSerialNoticesUITest::_navigation()
{
#ifdef QGC_NO_SERIAL_LINK
    QSKIP("Serial links are disabled");
#else
    QFETCH(QString, action);
    QFETCH(QString, page);
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    auto* notice = _raise(QStringLiteral("owner-gnss"));
    QVERIFY(notice);
    const QString buttonName = serialUiActionName(notice->id(), action);
    // Exercise the real MainWindow guard and its toast, not a host-window mock.
    QObject* globals = nullptr;
    for (auto* child : _window->findChildren<QObject*>()) {
        if (child->metaObject()->indexOfProperty("navigationBlockedReason") >= 0) {
            globals = child;
            break;
        }
    }
    QVERIFY(globals);
    const QQmlProperty block(globals, QStringLiteral("navigationBlockedReason"));
    QVERIFY(block.isValid());
    const QVariant previousBlock = block.read();
    const auto restoreBlock = qScopeGuard([&] { (void) block.write(previousBlock); });
    QVERIFY(block.write(QStringLiteral("Serial UI navigation blocked")));
    QVERIFY(_clickStable(buttonName));
    auto* drawer = findItem(_rootItem, QStringLiteral("mainView_toolDrawer"));
    QVERIFY(drawer);
    QVERIFY(!drawer->isVisible());
    const auto blockedToastVisible = [this] {
        for (auto* child : _window->findChildren<QObject*>()) {
            if (child->property("text").toString() == QStringLiteral("Serial UI navigation blocked") &&
                child->property("visible").toBool()) {
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(blockedToastVisible(), TestTimeout::shortMs());
    QVERIFY(!findVisibleItem(_rootItem, QStringLiteral("settings_rightPanel"), 0));
    QVERIFY(notice->active() && !notice->acknowledged());
    QVERIFY(block.write(QString()));
    QVERIFY(_clickStable(buttonName));
    auto* panel = findVisibleItem(_rootItem, QStringLiteral("settings_rightPanel"), TestTimeout::mediumMs());
    QVERIFY(panel);
    const auto expectedSource = page == QStringLiteral("Comm Links") ? QStringLiteral("CommLinksSettings.qml")
                                                                     : QStringLiteral("GNSSReceiverSettings.qml");
    QTRY_VERIFY_WITH_TIMEOUT(panel->property("source").toUrl().path().endsWith(expectedSource),
                             TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(panel->property("item").value<QObject*>(), TestTimeout::mediumMs());
    QVERIFY(notice->active() && !notice->acknowledged());
    QVERIFY(findVisibleItem(_rootItem, serialUiRowName(notice->id()), 0));
#endif
}

void COPSerialNoticesUITest::_attentionAndHistory()
{
#ifdef QGC_NO_SERIAL_LINK
    QSKIP("Serial links are disabled");
#else
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    auto* controller = _controller();
    auto* footer = _footer();
    auto* timer = footer->findChild<QObject*>(QStringLiteral("copNotificationMinimizeTimer"));
    QVERIFY(timer);
    QSignalSpy alerts(controller, &COPController::attentionRequested);
    controller->notify(QStringLiteral("ordinary notification"));
    QCOMPARE(alerts.count(), 1);
    QVERIFY(clickButton(QStringLiteral("copNotificationAcknowledge")));
    QVERIFY(timer->property("running").toBool());
    // Trigger the timeout handler deterministically, including the pending-timeout race.
    QVERIFY(QMetaObject::invokeMethod(timer, "triggered"));
    QTRY_COMPARE_WITH_TIMEOUT(footer->height(), 0.0, TestTimeout::shortMs());
    auto* notice = _raise(QStringLiteral("access_denied"));
    QVERIFY(notice);
    const QString id = notice->id();
    QTRY_VERIFY_WITH_TIMEOUT(footer->height() > 0, TestTimeout::shortMs());
    QCOMPARE(alerts.count(), 2);
    QGCPalette palette;
    QCOMPARE(QQmlProperty(footer, QStringLiteral("border.color")).read().value<QColor>(), palette.colorOrange());
    QVERIFY(!timer->property("running").toBool());
    for (int tick = 0; tick < 4; ++tick) {
        LinkManagerTest::reportSerialOpenFailure(*LinkManager::instance(), _configs.last().get(),
                                                 {SerialOpenOutcome::Stage::Open, QSerialPort::PermissionError,
                                                  QStringLiteral("changed native error %1").arg(tick), true});
        LinkManagerTest::observeSerialPorts(*LinkManager::instance(), _ports);
    }
    QCOMPARE(alerts.count(), 2);
    QVERIFY(QMetaObject::invokeMethod(timer, "triggered"));
    QVERIFY(footer->height() > 0);
    // Bounded ordinary history cannot evict or acknowledge an active serial blocker.
    for (int index = 0; index < 1005; ++index) {
        controller->notify(QStringLiteral("history overflow %1").arg(index));
    }
    QCOMPARE(controller->messages().size(), 1000);
    QCOMPARE(controller->droppedMessages(), quint64(5));
    QVERIFY(clickButton(QStringLiteral("copNotificationAcknowledge")));
    QCOMPARE(controller->messages().size(), 200);
    QVERIFY(!notice->acknowledged());
    QVERIFY(!timer->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(timer, "triggered"));
    QVERIFY(footer->height() > 0);
    QVERIFY(findVisibleItem(_rootItem, serialUiRowName(id), 0));
    const qsizetype beforeEscalation = alerts.count();
    _nowUs += LinkManager::SERIAL_UNAVAILABLE_DELAY_US;
    LinkManagerTest::observeSerialPorts(*LinkManager::instance(), _ports);
    QCOMPARE(alerts.count(), beforeEscalation + 1);
    QCOMPARE(notice->reason(), QStringLiteral("still_unavailable"));
    LinkManagerTest::observeSerialPorts(*LinkManager::instance(), _ports);
    QCOMPARE(alerts.count(), beforeEscalation + 1);
    QVERIFY(_clickStable(serialUiActionName(id, QStringLiteral("Dismiss"))));
    QVERIFY(notice->acknowledged());
    LinkManagerTest::reportSerialOpenFailure(
        *LinkManager::instance(), _configs.first().get(),
        {SerialOpenOutcome::Stage::Open, QSerialPort::DeviceNotFoundError, QStringLiteral("different blocker"), true});
    QCOMPARE(alerts.count(), beforeEscalation + 2);
    QVERIFY(!notice->acknowledged());
    QVERIFY(_liveRow(id));
    auto* second = _raise(QStringLiteral("open_failed"));
    QVERIFY(second);
    QVERIFY(second->id() != id);
    QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, serialUiRowName(second->id()), 0), TestTimeout::shortMs());
    QVERIFY(findVisibleItem(_rootItem, serialUiRowName(id), 0));
    QVERIFY(findVisibleItemScrolled(serialUiRowName(second->id()), QStringLiteral("copSerialNoticesView")));
    const qsizetype beforeResolution = alerts.count();
    auto config = _configs.first();
    auto restored = LinkManagerTest::_serialNoticeLink(*LinkManager::instance(), config);
    _links.append(restored);
    LinkManagerTest::reportSerialConnected(restored.get());
    QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(id), 0), TestTimeout::shortMs());
    QVERIFY(!notice->active());
    QVERIFY(notice->text().contains(QStringLiteral("serial connection restored")));
    QCOMPARE(alerts.count(), beforeResolution);
    QVERIFY(findVisibleItem(_rootItem, serialUiRowName(second->id()), 0));
    LinkManager::instance()->disconnectLinkConfiguration(_configs.last().get());
    QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(second->id()), 0), TestTimeout::shortMs());
    QCOMPARE(alerts.count(), beforeResolution);
    QVERIFY(timer->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(timer, "triggered"));
    QTRY_COMPARE_WITH_TIMEOUT(footer->height(), 0.0, TestTimeout::shortMs());
#endif
}

void COPSerialNoticesUITest::_discoveryLifecycle()
{
#ifdef QGC_NO_SERIAL_LINK
    QSKIP("Serial links are disabled");
#else
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    QSignalSpy alerts(_controller(), &COPController::attentionRequested);
    auto* notice = _raise(QStringLiteral("class_disabled"));
    QVERIFY(notice);
    const QString id = notice->id();
    QVERIFY(_liveRow(id));
    QCOMPARE(alerts.count(), 1);
    QPointer<QQuickItem> loader = _footer()->parentItem();
    QVERIFY(loader && loader->property("active").isValid());
    QVERIFY(loader->setProperty("active", false));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTRY_VERIFY_WITH_TIMEOUT(!_footer(), TestTimeout::shortMs());
    QVERIFY(loader->setProperty("active", true));
    QTRY_VERIFY_WITH_TIMEOUT(_footer() && _footer()->property("hostWindow").value<QObject*>() == _window,
                             TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(_footer()->height() > 0, TestTimeout::shortMs());
    QVERIFY(_liveRow(id));
    QCOMPARE(alerts.count(), 1);
    SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->setRawValue(true);
    LinkManagerTest::observeSerialPorts(*LinkManager::instance(), _ports);
    QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(id), 0), TestTimeout::shortMs());
    QVERIFY(!notice->active());
    QCOMPARE(alerts.count(), 1);
    SettingsManager::instance()->autoConnectSettings()->autoConnectPixhawk()->setRawValue(false);
    LinkManagerTest::observeSerialPorts(*LinkManager::instance(), _ports);
    QVERIFY(_liveRow(id));
    QCOMPARE(alerts.count(), 2);
    LinkManagerTest::observeSerialPorts(*LinkManager::instance(), {});
    QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(id), 0), TestTimeout::shortMs());
    QCOMPARE(alerts.count(), 2);
#endif
}

void COPSerialNoticesUITest::_automaticOpen_data()
{
    QTest::addColumn<bool>("occupied");
    QTest::newRow("real-permission-error") << true;
    QTest::newRow("real-missing-device") << false;
}

void COPSerialNoticesUITest::_automaticOpen()
{
#if !defined(QGC_NO_SERIAL_LINK) && defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QFETCH(bool, occupied);
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    QVERIFY(master >= 0);
    const auto closeMaster = qScopeGuard([master] { ::close(master); });
    QCOMPARE(::grantpt(master), 0);
    QCOMPARE(::unlockpt(master), 0);
    const char* slave = ::ptsname(master);
    QVERIFY(slave);
    QSerialPort blocker(QString::fromLocal8Bit(slave));
    if (occupied) {
        QVERIFY(blocker.open(QIODevice::ReadWrite));
    }
    auto config = _configuration(occupied ? blocker.portName() : _directory.filePath(QStringLiteral("absent-port")));
    QSignalSpy alerts(_controller(), &COPController::attentionRequested);
    QVERIFY(LinkManager::instance()->createConnectedLink(config));
    QTRY_COMPARE_WITH_TIMEOUT(LinkManager::instance()->serialNotices()->count(), 1, TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(!config->link(), TestTimeout::mediumMs());
    auto* notice = LinkManager::instance()->serialNotices()->value<SerialConnectionNotice*>(0);
    QCOMPARE(notice->reason(), occupied ? QStringLiteral("access_denied") : QStringLiteral("device_not_found"));
    const QString id = notice->id();
    QVERIFY(findVisibleItem(_rootItem, serialUiRowName(id)));
    QCOMPARE(alerts.count(), 1);
    if (occupied) {
        blocker.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(LinkManager::instance()->createConnectedLink(config));
        QTRY_VERIFY_WITH_TIMEOUT(config->link() && config->link()->isConnected(), TestTimeout::mediumMs());
        QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(id), 0), TestTimeout::shortMs());
        QCOMPARE(alerts.count(), 1);
        LinkManager::instance()->disconnectLinkConfiguration(config.get());
        QTRY_VERIFY_WITH_TIMEOUT(!config->link(), TestTimeout::shortMs());
    }
#else
    QSKIP("Requires serial support and a Linux pseudo-terminal");
#endif
}

void COPSerialNoticesUITest::_serialVehicleLoss()
{
#ifdef QGC_NO_SERIAL_LINK
    QSKIP("Serial links are disabled");
#else
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    Vehicle* vehicle = nullptr;
    const auto mock = connectMockLinkAndWaitReady(
        [this] {
            auto* created = COPTestHelpers::connectMockLink(216);
            if (created) {
                _configs.append(created->linkConfiguration());
            }
            return created;
        },
        vehicle);
    QVERIFY(mock && vehicle);
    auto* controller = _controller();
    controller->selectVehicle(vehicle->id());
    QVERIFY(controller->selected());
    auto* vehicleLinks = vehicle->vehicleLinkManager();
    vehicleLinks->setCommunicationLostEnabled(false);
    QSignalSpy alerts(controller, &COPController::attentionRequested);
    emit vehicleLinks->communicationLostChanged(true);
    const QString radioLoss = controller->messages().first().toMap().value(QStringLiteral("text")).toString();
    QCOMPARE(radioLoss, QStringLiteral("Lost contact with %1. The map shows where it was last seen. It will reconnect "
                                       "on its own when the radio link returns.")
                            .arg(controller->selected()->label()));
    QCOMPARE(alerts.count(), 1);
    emit vehicleLinks->communicationLostChanged(false);
    auto config = _configuration();
    config->setAutoConnect(false);
    auto serial = LinkManagerTest::_serialNoticeLink(*LinkManager::instance(), config, true);
    QVERIFY(serial);
    _links.append(serial);
    LinkManagerTest::reportSerialConnected(serial.get());
    mavlink_message_t heartbeat{};
    heartbeat.msgid = MAVLINK_MSG_ID_HEARTBEAT;
    // Deliver loss after the S2 observer, before VehicleLinkManager removes its final transport.
    connect(serial.get(), &LinkInterface::disconnected, vehicleLinks,
            [vehicleLinks] { emit vehicleLinks->communicationLostChanged(true); });
    vehicleLinks->mavlinkMessageReceived(serial.get(), heartbeat);
    vehicleLinks->setPrimaryLinkByName(config->name());
    QVERIFY(controller->selected()->serialTransport());
    mock->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(!mock || !mock->isConnected(), TestTimeout::shortMs());
    alerts.clear();
    const qsizetype beforeLoss = controller->messages().size();
    // Heartbeat loss with an open port must not promise unconditional serial reconnection.
    emit vehicleLinks->communicationLostChanged(true);
    QCOMPARE(alerts.count(), 1);
    const QString loss = controller->messages().first().toMap().value(QStringLiteral("text")).toString();
    QVERIFY(loss.contains(QStringLiteral("The serial port is open")));
    QVERIFY(!loss.contains(QStringLiteral("reconnect on its own")));
    emit vehicleLinks->communicationLostChanged(false);
    alerts.clear();
    serial->disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->selected()->vehicle(), TestTimeout::mediumMs());
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(controller->messages().size(), beforeLoss + 2);
    const QString disconnected = controller->messages().first().toMap().value(QStringLiteral("text")).toString();
    QVERIFY(disconnected.contains(QStringLiteral("last known position and mode, not live data")));
    auto* notice = LinkManager::instance()->serialNotices()->value<SerialConnectionNotice*>(0);
    QVERIFY(notice);
    QCOMPARE(notice->reason(), QStringLiteral("auto_connect_off"));
    QVERIFY(findVisibleItem(_rootItem, serialUiRowName(notice->id())));
#endif
}

void COPSerialNoticesUITest::_stockFooterHook()
{
    QGCCorePlugin stock;
    QVERIFY(stock.notificationFooter().isEmpty());
}

void COPSerialNoticesUITest::_dismissOneOfTwo()
{
#ifdef QGC_NO_SERIAL_LINK
    QSKIP("Serial links are disabled");
#else
    _startFooter();
    QVERIFY(!QTest::currentTestFailed());
    auto* first = _raise(QStringLiteral("access_denied"));
    QVERIFY(first);
    auto* second = _raise(QStringLiteral("open_failed"));
    QVERIFY(second);
    QCOMPARE(_configs.size(), 2);
    const QString firstId = first->id();
    const QString secondId = second->id();
    QVERIFY(firstId != secondId);
    QVERIFY(_liveRow(firstId));
    QVERIFY(_liveRow(secondId));
    QVERIFY(verifyText(QStringLiteral("copNotificationHeading"), QStringLiteral("Notifications (2)"),
                       QStringLiteral("dismiss one of two")));
    QTRY_VERIFY_WITH_TIMEOUT(_footer()->height() > 0, TestTimeout::shortMs());
    QSignalSpy alerts(_controller(), &COPController::attentionRequested);

    QVERIFY(_clickStable(serialUiActionName(firstId, QStringLiteral("Dismiss"))));
    QVERIFY(first->active() && first->acknowledged());
    QVERIFY(second->active() && !second->acknowledged());
    QVERIFY(verifyText(QStringLiteral("copNotificationHeading"), QStringLiteral("Notifications (1)"),
                       QStringLiteral("dismiss one of two")));
    // Acknowledgement can rebuild delegates; flush deferred deletion and resolve the live rows again.
    QPointer<QQuickItem> remainingRow = _liveRow(secondId);
    QVERIFY(remainingRow);
    const QStringList expected{QStringLiteral("Open Comm Links"), QStringLiteral("Dismiss")};
    QTRY_VERIFY_WITH_TIMEOUT(remainingRow && serialUiButtons(remainingRow) == expected, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, serialUiRowName(firstId), 0), TestTimeout::shortMs());
    QVERIFY(_footer()->height() > 0);
    QCOMPARE(alerts.count(), 0);

    QVERIFY(_clickStable(serialUiActionName(secondId, QStringLiteral("Dismiss"))));
    QVERIFY(second->active() && second->acknowledged());
    QTRY_COMPARE_WITH_TIMEOUT(_footer()->height(), 0.0, TestTimeout::shortMs());
    QCOMPARE(alerts.count(), 0);
#endif
}
