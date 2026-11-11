#include "LinkPreferenceControllerTest.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QSettings>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "LinkPreferenceController.h"
#include "MavlinkSettings.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QGCCorePlugin.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "SigningController.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VehicleRoleController.h"
#include "VehicleSigningController.h"

void LinkPreferenceControllerTest::init()
{
    CommsTest::init();
    QSettings settingsStore;
    _failbackHold = settingsStore.value(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    settingsStore.remove(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    ignoreLogMessage("Vehicle.StandardModes", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Failed to retrieve available modes")));
    QVERIFY(_directory.isValid());
    QDir directory(_directory.path());
    QVERIFY(directory.removeRecursively());
    QVERIFY(directory.mkpath(QStringLiteral(".")));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
    _roles = new VehicleRoleController(this);
    _preference = _roles->findChild<LinkPreferenceController*>();
    QVERIFY(_preference);
}

void LinkPreferenceControllerTest::cleanup()
{
    delete _roles;
    _roles = nullptr;
    _preference = nullptr;
    disconnectAllLinks();
    _backup.reset();
    _preferred.reset();
    _configs.clear();
    auto* settings = SettingsManager::instance()->appSettings();
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    QSettings settingsStore;
    if (_failbackHold.isValid()) {
        settingsStore.setValue(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"), _failbackHold);
    } else {
        settingsStore.remove(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    }
    CommsTest::cleanup();
}

SharedLinkInterfacePtr LinkPreferenceControllerTest::_startLink(const QString& name, quint32 sysid, bool startArmed)
{
    auto* mockConfig = new MockConfiguration(name);
    SharedLinkConfigurationPtr config(mockConfig);
    mockConfig->setDynamic(true);
    mockConfig->setIncrementVehicleId(false);
    mockConfig->setSystemId(sysid);
    if (startArmed) {
        // ArduPilot skips the download outright, independent of any PX4 parameter cache.
        mockConfig->setFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
        mockConfig->setStartArmed(true);
    }
    _configs.append(config);
    if (!linkManager()->createConnectedLink(config)) {
        return {};
    }
    return linkManager()->sharedLinkInterfacePointerForLink(config->link());
}

bool LinkPreferenceControllerTest::_waitUntilIdle(Vehicle* vehicle)
{
    auto* parameters = vehicle->parameterManager();
    return waitForCondition(
        [vehicle, parameters]() {
            return parameters->parametersReady() && !parameters->pendingWrites() && parameters->loadProgress() <= 0 &&
                   !vehicle->missionManager()->inProgress() && !vehicle->geoFenceManager()->inProgress() &&
                   !vehicle->rallyPointManager()->inProgress();
        },
        TestTimeout::longMs(), QStringLiteral("transfers idle"));
}

Vehicle* LinkPreferenceControllerTest::_startPair(quint32 sysid, bool productionClock)
{
    if (!productionClock) {
        _preference->setTimingForTesting({.initialSettleMs = 3, .baseHoldMs = 15}, 0);
    }
    _backup = _startLink(QStringLiteral("RFD"), sysid);
    if (!_backup) {
        return nullptr;
    }
    auto* vehicle = waitForVehicleConnect(TestTimeout::longMs());
    if (!vehicle || !waitForCondition([vehicle]() { return vehicle->isInitialConnectComplete(); },
                                      TestTimeout::longMs(), QStringLiteral("initial connect"))) {
        return nullptr;
    }
    _preferred = _startLink(QStringLiteral("UDP"), sysid);
    if (!_preferred || !waitForCondition([vehicle]() { return vehicle->vehicleLinkManager()->linkNames().size() == 2; },
                                         TestTimeout::longMs(), QStringLiteral("both links attached"))) {
        return nullptr;
    }
    if (!_waitUntilIdle(vehicle)) {
        return nullptr;
    }
    _roles->addEntry(vehicle->id(), QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    _roles->addLink(0, QStringLiteral("RFD"), QStringLiteral("RFD"));
    _roles->addLink(0, QStringLiteral("UDP"), QStringLiteral("UDP"));
    _roles->setLinkPreferred(0, 1, true);
    return vehicle;
}

void LinkPreferenceControllerTest::_productionClockReturnsAfterHold()
{
    _roles->setFailbackHoldSeconds(5);
    Vehicle* const vehicle = _startPair(1, true);
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    auto* preferred = qobject_cast<MockLink*>(_preferred.get());
    QVERIFY(preferred);
    QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("UDP"), TestTimeout::longMs());
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Switching communication to secondary link")));
    preferred->setCommLost(true);
    QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("RFD"), TestTimeout::longMs());
    verifyExpectedLogMessage();
    QElapsedTimer recovery;
    recovery.start();
    preferred->setCommLost(false);
    const QRegularExpression nearlyDue(QStringLiteral("^Returning to UDP in [12] s if it stays steady\\.$"));
    // A countdown near the deadline is a positive barrier; an early return also ends the wait and fails below.
    QTRY_VERIFY_WITH_TIMEOUT(nearlyDue.match(_roles->linkReturnStatus(vehicle->id())).hasMatch() ||
                                 links->primaryLinkName() == QStringLiteral("UDP"),
                             TestTimeout::longMs());
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QVERIFY(recovery.elapsed() >= 2500);
    QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("UDP"), TestTimeout::longMs());
    QVERIFY(recovery.elapsed() >= 4500);
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("Using UDP, the preferred link."));
}

void LinkPreferenceControllerTest::_armedConnectionCanReturn()
{
    auto* skipWhenArmed = SettingsManager::instance()->mavlinkSettings()->noInitialDownloadWhenArmed();
    const QVariant previousSkipWhenArmed = skipWhenArmed->rawValue();
    const auto restoreSkipWhenArmed =
        qScopeGuard([skipWhenArmed, previousSkipWhenArmed]() { skipWhenArmed->setRawValue(previousSkipWhenArmed); });
    skipWhenArmed->setRawValue(true);
    _preference->setTimingForTesting({.initialSettleMs = 3, .baseHoldMs = 15}, 0);
    _backup = _startLink(QStringLiteral("RFD"), 1, true);
    QVERIFY(_backup);
    Vehicle* const vehicle = waitForVehicleConnect(TestTimeout::longMs());
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->isInitialConnectComplete(), TestTimeout::longMs());
    QVERIFY(vehicle->armed());
    auto* parameters = vehicle->parameterManager();
    QVERIFY(parameters->parameterDownloadSkipped());
    QVERIFY(!parameters->parametersReady());
    _preferred = _startLink(QStringLiteral("UDP"), 1, true);
    QVERIFY(_preferred);
    auto* links = vehicle->vehicleLinkManager();
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
    _roles->addEntry(vehicle->id(), QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    _roles->addLink(0, QStringLiteral("RFD"), QStringLiteral("RFD"));
    _roles->addLink(0, QStringLiteral("UDP"), QStringLiteral("UDP"));
    _roles->setLinkPreferred(0, 1, true);
    _preference->evaluateForTesting(3);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));

    auto* preferred = qobject_cast<MockLink*>(_preferred.get());
    QVERIFY(preferred);
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Switching communication to secondary link")));
    preferred->setCommLost(true);
    QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("RFD"), TestTimeout::longMs());
    verifyExpectedLogMessage();
    _preference->evaluateForTesting(100);
    preferred->setCommLost(false);
    QTRY_VERIFY_WITH_TIMEOUT(links->linkStatuses()
                                 .value(links->linkNames().indexOf(QStringLiteral("UDP")), QStringLiteral("missing"))
                                 .isEmpty(),
                             TestTimeout::longMs());
    _preference->evaluateForTesting(114);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    _preference->evaluateForTesting(115);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("Using UDP, the preferred link."));
    QVERIFY(vehicle->armed());
    QVERIFY(!parameters->parametersReady());
}

void LinkPreferenceControllerTest::_returnStatusAndNotifications()
{
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    const quint32 sysid = vehicle->id();
    auto* links = vehicle->vehicleLinkManager();
    auto* parameters = vehicle->parameterManager();
    _roles->setLinkPreferred(0, 1, false);
    QCOMPARE(_roles->linkReturnStatus(sysid),
             QStringLiteral("Automatic return is off. Tick Preferred on one link to turn it on."));
    QVERIFY(_roles->linkReturnStatus(123).isEmpty());
    _preference->setTimingForTesting({.initialSettleMs = 2500, .baseHoldMs = 60000}, 0);
    QSignalSpy changes(_roles, &VehicleRoleController::linkReturnStatusChanged);
    QStringList observedTexts;
    QObject observer;
    connect(_roles, &VehicleRoleController::linkReturnStatusChanged, &observer,
            [this, sysid, &observedTexts]() { observedTexts.append(_roles->linkReturnStatus(sysid)); });
    const int revision = _roles->linkReturnStatusRevision();
    _roles->setLinkPreferred(0, 1, true);
    QCOMPARE(_roles->linkReturnStatus(sysid), QStringLiteral("Returning to UDP in 3 s if it stays steady."));
    QCOMPARE(changes.count(), 1);
    QCOMPARE(_roles->linkReturnStatusRevision(), revision + 1);
    _preference->evaluateForTesting(1);
    QCOMPARE(changes.count(), 1);  // Same rounded-up second does not notify.
    _preference->evaluateForTesting(501);
    QCOMPARE(_roles->linkReturnStatus(sysid), QStringLiteral("Returning to UDP in 2 s if it stays steady."));
    QCOMPARE(changes.count(), 2);
    _preference->evaluateForTesting(2499);
    QCOMPARE(_roles->linkReturnStatus(sysid), QStringLiteral("Returning to UDP in 1 s if it stays steady."));
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    parameters->setPendingWritesForTest(true);
    _preference->evaluateForTesting(2500);
    QCOMPARE(_roles->linkReturnStatus(sysid),
             QStringLiteral("Return paused while a parameter, mission, fence, rally or signing transfer is running."));
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    parameters->setPendingWritesForTest(false);
    QTRY_COMPARE_WITH_TIMEOUT(_roles->linkReturnStatus(sysid), QStringLiteral("Using UDP, the preferred link."),
                              TestTimeout::shortMs());
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QVERIFY(observedTexts.contains(QStringLiteral("Returning to UDP...")));
    const int countOnPreferred = changes.count();
    _preference->evaluateForTesting(10000);
    QCOMPARE(changes.count(), countOnPreferred);
    QCOMPARE(_roles->linkReturnStatusRevision(), revision + changes.count());
    disconnectAllLinks();
    QTRY_VERIFY_WITH_TIMEOUT(_roles->linkReturnStatus(sysid).isEmpty(), TestTimeout::longMs());
}

void LinkPreferenceControllerTest::_returnStatusText_data()
{
    QTest::addColumn<int>("gate");
    QTest::addColumn<qint64>("remainingMs");
    QTest::addColumn<bool>("extendedHold");
    QTest::addColumn<QString>("primaryName");
    QTest::addColumn<QString>("expectedText");
    QTest::newRow("usual-wait") << int(LinkPreferencePolicy::Gate::Waiting) << qint64(117001) << false
                                << QStringLiteral("RFD")
                                << QStringLiteral("Returning to UDP in 118 s if it stays steady.");
    QTest::newRow("extended-wait") << int(LinkPreferencePolicy::Gate::Waiting) << qint64(117001) << true
                                   << QStringLiteral("RFD")
                                   << QStringLiteral(
                                          "Returning to UDP in 118 s if it stays steady (longer than usual because it "
                                          "dropped again soon after the last return).");
    QTest::newRow("minimum-wait") << int(LinkPreferencePolicy::Gate::Waiting) << qint64(0) << false
                                  << QStringLiteral("RFD")
                                  << QStringLiteral("Returning to UDP in 1 s if it stays steady.");
    QTest::newRow("down-with-primary") << int(LinkPreferencePolicy::Gate::PreferredDown) << qint64(0) << false
                                       << QStringLiteral("RFD")
                                       << QStringLiteral("UDP has no communication. Staying on RFD.");
    QTest::newRow("down-without-primary") << int(LinkPreferencePolicy::Gate::PreferredDown) << qint64(0) << false
                                          << QString() << QStringLiteral("UDP has no communication.");
}

void LinkPreferenceControllerTest::_returnStatusText()
{
    QFETCH(int, gate);
    QFETCH(qint64, remainingMs);
    QFETCH(bool, extendedHold);
    QFETCH(QString, primaryName);
    QFETCH(QString, expectedText);
    LinkPreferencePolicy::Input state;
    state.preferredName = QStringLiteral("UDP");
    state.primaryName = primaryName;
    const LinkPreferencePolicy::Status status{
        .gate = LinkPreferencePolicy::Gate(gate), .remainingMs = remainingMs, .extendedHold = extendedHold};
    QCOMPARE(_preference->_statusText(state, status), expectedText);
}

void LinkPreferenceControllerTest::_statusObserverCanPauseReturn()
{
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    auto* parameters = vehicle->parameterManager();
    bool pauseRequested = false;
    QObject observer;
    connect(
        _roles, &VehicleRoleController::linkReturnStatusChanged, &observer,
        [this, vehicle, parameters, &pauseRequested]() {
            if (!pauseRequested && _roles->linkReturnStatus(vehicle->id()) == QStringLiteral("Returning to UDP...")) {
                pauseRequested = true;
                parameters->setPendingWritesForTest(true);
            }
        });
    _preference->evaluateForTesting(3);
    QVERIFY(pauseRequested);
    QCOMPARE(vehicle->vehicleLinkManager()->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()),
             QStringLiteral("Return paused while a parameter, mission, fence, rally or signing transfer is running."));
    parameters->setPendingWritesForTest(false);
    QTRY_COMPARE_WITH_TIMEOUT(vehicle->vehicleLinkManager()->primaryLinkName(), QStringLiteral("UDP"),
                              TestTimeout::shortMs());
}

void LinkPreferenceControllerTest::_settleFailoverFailbackAndSecondaryChoice_data()
{
    QTest::addColumn<quint32>("sysid");
    QTest::newRow("one") << quint32(1);
    QTest::newRow("uint32-max") << quint32(4294967295U);
}

void LinkPreferenceControllerTest::_settleFailoverFailbackAndSecondaryChoice()
{
    QFETCH(quint32, sysid);
    Vehicle* const vehicle = _startPair(sysid);
    QVERIFY(vehicle);
    QCOMPARE(vehicle->id(), sysid);
    auto* links = vehicle->vehicleLinkManager();
    QCOMPARE(MultiVehicleManager::instance()->vehicles()->count(), 1);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QSignalSpy notifications(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification);
    _preference->evaluateForTesting(2);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    _preference->evaluateForTesting(3);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.at(0).at(0).toString(), QStringLiteral("Scout: communication back on UDP"));

    auto* preferred = qobject_cast<MockLink*>(_preferred.get());
    QVERIFY(preferred);
    expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression(QStringLiteral("Switching communication to secondary link")));
    preferred->setCommLost(true);
    QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("RFD"), TestTimeout::longMs());
    verifyExpectedLogMessage();
    _preference->evaluateForTesting(100);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(notifications.count(), 1);
    preferred->setCommLost(false);
    const auto healthy = [links]() {
        const auto index = links->linkNames().indexOf(QStringLiteral("UDP"));
        return index >= 0 && links->linkStatuses().value(index, QStringLiteral("missing")).isEmpty();
    };
    QTRY_VERIFY_WITH_TIMEOUT(healthy(), TestTimeout::longMs());
    _preference->evaluateForTesting(114);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    _preference->evaluateForTesting(115);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(notifications.count(), 2);
    links->setPrimaryLinkByName(QStringLiteral("RFD"));
    QCOMPARE(_roles->linkReturnStatus(sysid), QStringLiteral("Returning to UDP in 1 s if it stays steady."));
    _preference->evaluateForTesting(129);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(notifications.count(), 2);
    _preference->evaluateForTesting(130);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(notifications.count(), 3);
    QCOMPARE(notifications.at(2).at(0).toString(), QStringLiteral("Scout: communication back on UDP"));
}

void LinkPreferenceControllerTest::_lostAndOtherVehicleLinksAreNeverSelected()
{
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    auto* preferred = qobject_cast<MockLink*>(_preferred.get());
    QVERIFY(preferred);
    preferred->setCommLost(true);
    QTRY_VERIFY_WITH_TIMEOUT(!links->linkStatuses().value(links->linkNames().indexOf(QStringLiteral("UDP"))).isEmpty(),
                             TestTimeout::longMs());
    QSignalSpy primaryChanges(links, &VehicleLinkManager::primaryLinkChanged);
    _preference->evaluateForTesting(1000);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(primaryChanges.count(), 0);
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("UDP has no communication. Staying on RFD."));

    // A preference must belong to this vehicle even when another vehicle is active.
    expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle")));
    const auto otherLink = _startLink(QStringLiteral("Other UDP"), 1);
    QVERIFY(otherLink);
    auto* otherMock = qobject_cast<MockLink*>(otherLink.get());
    QVERIFY(otherMock);
    auto* manager = MultiVehicleManager::instance();
    QTRY_VERIFY_WITH_TIMEOUT(manager->getVehicleById(otherMock->vehicleId()), TestTimeout::longMs());
    Vehicle* const other = manager->getVehicleById(otherMock->vehicleId());
    QVERIFY(other && other != vehicle);
    verifyExpectedLogMessage();
    QTRY_VERIFY_WITH_TIMEOUT(other->isInitialConnectComplete(), TestTimeout::longMs());
    const QString otherPrimary = other->vehicleLinkManager()->primaryLinkName();
    manager->setActiveVehicle(other);
    _roles->setLinkConfigName(0, 1, QStringLiteral("Other UDP"));
    _roles->setLinkPreferred(0, 1, true);
    _preference->evaluateForTesting(2000);
    _preference->evaluateForTesting(3000);
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()),
             QStringLiteral("Other UDP is not connected to this vehicle right now."));
    QCOMPARE(primaryChanges.count(), 0);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(other->vehicleLinkManager()->primaryLinkName(), otherPrimary);
    _roles->setLinkConfigName(0, 1, QStringLiteral("UDP"));
    _roles->setLinkPreferred(0, 1, true);
    preferred->setCommLost(false);
    QTRY_VERIFY_WITH_TIMEOUT(links->linkStatuses()
                                 .value(links->linkNames().indexOf(QStringLiteral("UDP")), QStringLiteral("missing"))
                                 .isEmpty(),
                             TestTimeout::longMs());
    _preference->evaluateForTesting(3015);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(other->vehicleLinkManager()->primaryLinkName(), otherPrimary);

    disconnectAllLinks();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 0, TestTimeout::longMs());
    _preference->evaluateForTesting(10000);
}

void LinkPreferenceControllerTest::_healthyPreferredDepartureReturnsAfterWait()
{
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    _preference->setTimingForTesting(LinkPreferencePolicy::Timing{}, 0);
    _preference->evaluateForTesting(6000);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    _preference->evaluateForTesting(16000);
    QSignalSpy notifications(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification);
    links->setPrimaryLinkByName(QStringLiteral("RFD"));
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("Returning to UDP in 60 s if it stays steady."));
    _preference->evaluateForTesting(16000);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(notifications.count(), 0);
    _preference->evaluateForTesting(17001);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("Returning to UDP in 59 s if it stays steady."));
    _preference->evaluateForTesting(75999);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("Returning to UDP in 1 s if it stays steady."));
    QCOMPARE(notifications.count(), 0);
    _preference->evaluateForTesting(76000);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()), QStringLiteral("Using UDP, the preferred link."));
    QCOMPARE(notifications.count(), 1);
    QCOMPARE(notifications.at(0).at(0).toString(), QStringLiteral("Scout: communication back on UDP"));
    _preference->evaluateForTesting(76001);
    QCOMPARE(notifications.count(), 1);
}

void LinkPreferenceControllerTest::_repickCurrentPrimary_data()
{
    QTest::addColumn<QString>("phase");
    for (const char* name : {"initial-settle", "outage", "recovery"}) {
        QTest::newRow(name) << QString::fromLatin1(name);
    }
}

void LinkPreferenceControllerTest::_repickCurrentPrimary()
{
    QFETCH(QString, phase);
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    qint64 deadline = 3;
    if (phase != QStringLiteral("initial-settle")) {
        _preference->evaluateForTesting(3);
        auto* preferred = qobject_cast<MockLink*>(_preferred.get());
        QVERIFY(preferred);
        expectLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                         QRegularExpression(QStringLiteral("Switching communication to secondary link")));
        preferred->setCommLost(true);
        QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("RFD"), TestTimeout::longMs());
        verifyExpectedLogMessage();
        _preference->evaluateForTesting(100);
        if (phase == QStringLiteral("outage")) {
            links->setPrimaryLinkByName(QStringLiteral("RFD"));
        }
        preferred->setCommLost(false);
        QTRY_VERIFY_WITH_TIMEOUT(
            links->linkStatuses()
                .value(links->linkNames().indexOf(QStringLiteral("UDP")), QStringLiteral("missing"))
                .isEmpty(),
            TestTimeout::longMs());
        _preference->evaluateForTesting(105);
        deadline = 115;
    }
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QSignalSpy notifications(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification);
    QSignalSpy primaryChanges(links, &VehicleLinkManager::primaryLinkChanged);
    links->setPrimaryLinkByName(QStringLiteral("RFD"));
    QCOMPARE(primaryChanges.count(), 1);
    _preference->evaluateForTesting(deadline - 1);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(primaryChanges.count(), 1);
    QCOMPARE(notifications.count(), 0);
    _preference->evaluateForTesting(deadline);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(primaryChanges.count(), 2);
    QCOMPARE(notifications.count(), 1);
    _preference->evaluateForTesting(1000);
    QCOMPARE(primaryChanges.count(), 2);
    QCOMPARE(notifications.count(), 1);
}

void LinkPreferenceControllerTest::_reusedSysidStartsWithFreshHistory()
{
    QPointer<Vehicle> removed = _startPair(4294967295U);
    QVERIFY(removed);
    const quint32 sysid = removed->id();
    QSignalSpy notifications(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification);
    _preference->evaluateForTesting(2);
    QCOMPARE(removed->vehicleLinkManager()->primaryLinkName(), QStringLiteral("RFD"));
    // Preference changes have queued an evaluation; removing the Vehicle must cancel it.
    disconnectAllLinks();
    QTRY_VERIFY_WITH_TIMEOUT(!removed, TestTimeout::longMs());
    _backup.reset();
    _preferred.reset();
    _preference->evaluateForTesting(1000);
    QCOMPARE(notifications.count(), 0);

    _backup = _startLink(QStringLiteral("RFD"), sysid);
    QVERIFY(_backup);
    Vehicle* const vehicle = waitForVehicleConnect(TestTimeout::longMs());
    QVERIFY(vehicle);
    QCOMPARE(vehicle->id(), sysid);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->isInitialConnectComplete(), TestTimeout::longMs());
    _preferred = _startLink(QStringLiteral("UDP"), sysid);
    QVERIFY(_preferred);
    auto* links = vehicle->vehicleLinkManager();
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
    QVERIFY(_waitUntilIdle(vehicle));
    _preference->evaluateForTesting(1002);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(notifications.count(), 0);
    _preference->evaluateForTesting(1003);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    QCOMPARE(notifications.count(), 1);
}

void LinkPreferenceControllerTest::_physicalDisconnectWaitsForBackoff()
{
    Vehicle* const vehicle = _startPair(4294967295U);
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    _preference->evaluateForTesting(3);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    _preferred->disconnect();
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 1, TestTimeout::longMs());
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    _preference->evaluateForTesting(100);
    _preferred = _startLink(QStringLiteral("UDP"), vehicle->id());
    QVERIFY(_preferred);
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
    _preference->evaluateForTesting(114);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    _preference->evaluateForTesting(115);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
    _preferred->disconnect();
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 1, TestTimeout::longMs());
    _preferred = _startLink(QStringLiteral("UDP"), vehicle->id());
    QVERIFY(_preferred);
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
    _preference->evaluateForTesting(144);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    _preference->evaluateForTesting(145);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
}

void LinkPreferenceControllerTest::_transferBusyAtDeadline_data()
{
    QTest::addColumn<int>("transfer");
    QTest::newRow("parameter-write") << -1;
    QTest::newRow("mission-read") << 0;
    QTest::newRow("fence-read") << 1;
    QTest::newRow("rally-read") << 2;
}

void LinkPreferenceControllerTest::_transferBusyAtDeadline()
{
    QFETCH(int, transfer);
    Vehicle* const vehicle = _startPair(4294967295U);
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    auto* parameters = vehicle->parameterManager();
    PlanManager* plans[] = {vehicle->missionManager(), vehicle->geoFenceManager(), vehicle->rallyPointManager()};
    if (transfer < 0) {
        parameters->setPendingWritesForTest(true);
    } else {
        plans[transfer]->loadFromVehicle();
        QVERIFY(plans[transfer]->inProgress());
    }
    _preference->evaluateForTesting(3);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()),
             QStringLiteral("Return paused while a parameter, mission, fence, rally or signing transfer is running."));
    if (transfer < 0) {
        parameters->setPendingWritesForTest(false);
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(!plans[transfer]->inProgress(), TestTimeout::longMs());
    }
    _preference->evaluateForTesting(4);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("UDP"));
}

void LinkPreferenceControllerTest::_signingPendingAtDeadline_data()
{
    QTest::addColumn<bool>("disabling");
    QTest::newRow("enable-pending") << false;
    QTest::newRow("disable-pending") << true;
}

void LinkPreferenceControllerTest::_signingPendingAtDeadline()
{
    QFETCH(bool, disabling);
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    auto* signing = _backup->signing();
    auto* facade = vehicle->signingController();
    QVERIFY(signing && facade);
    MAVLinkSigning::SigningKey key{};
    key.fill(0x42);
    _preference->evaluateForTesting(2);
    // Use a reserved target without transmitting SETUP_SIGNING; background MockLink heartbeats cannot confirm it.
    constexpr quint32 signingTargetSysid = 0;
    if (disabling) {
        const QByteArrayView keyView(reinterpret_cast<const char*>(key.data()), key.size());
        QVERIFY(signing->initSigningImmediate(keyView, MAVLinkSigning::UnsignedAcceptancePolicy::Strict));
        QVERIFY(!signing->tryBeginDisable(signingTargetSysid));
    } else {
        QVERIFY(!signing->tryBeginEnable(signingTargetSysid, QString(), key));
    }
    QVERIFY(facade->signingStatus().pending());
    _preference->evaluateForTesting(3);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QVERIFY(facade->signingStatus().pending());
    QCOMPARE(_roles->linkReturnStatus(vehicle->id()),
             QStringLiteral("Return paused while a parameter, mission, fence, rally or signing transfer is running."));
    QSignalSpy statusChanges(facade, &VehicleSigningController::signingStatusChanged);
    expectLogMessage("MAVLink.SigningController", QtWarningMsg,
                     QRegularExpression(disabling ? QStringLiteral("Signing disable not confirmed")
                                                  : QStringLiteral("Signing operation cancelled")));
    signing->cancelPending();
    verifyExpectedLogMessage();
    QVERIFY(!facade->signingStatus().pending());
    QVERIFY(!statusChanges.isEmpty());
    QVERIFY(signing->clearSigning());
    // Completion must trigger a queued reevaluation at the same deadline, without advancing the test clock.
    QTRY_COMPARE_WITH_TIMEOUT(links->primaryLinkName(), QStringLiteral("UDP"), TestTimeout::shortMs());
}

UT_REGISTER_TEST(LinkPreferenceControllerTest, TestLabel::Integration, TestLabel::Vehicle)
