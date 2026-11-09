#include "LinkPreferenceControllerTest.h"

#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "LinkPreferenceController.h"
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
    _preference->setTimingForTesting({.initialSettleMs = 3, .baseHoldMs = 15}, 0);
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
    CommsTest::cleanup();
}

SharedLinkInterfacePtr LinkPreferenceControllerTest::_startLink(const QString& name, quint32 sysid)
{
    auto* mockConfig = new MockConfiguration(name);
    SharedLinkConfigurationPtr config(mockConfig);
    mockConfig->setDynamic(true);
    mockConfig->setIncrementVehicleId(false);
    mockConfig->setSystemId(sysid);
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

Vehicle* LinkPreferenceControllerTest::_startPair(quint32 sysid)
{
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

void LinkPreferenceControllerTest::_settleFailoverFailbackAndManualChoice_data()
{
    QTest::addColumn<quint32>("sysid");
    QTest::newRow("one") << quint32(1);
    QTest::newRow("uint32-max") << quint32(4294967295U);
}

void LinkPreferenceControllerTest::_settleFailoverFailbackAndManualChoice()
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
    _preference->evaluateForTesting(1000);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(notifications.count(), 2);
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

void LinkPreferenceControllerTest::_repickCurrentPrimary()
{
    Vehicle* const vehicle = _startPair();
    QVERIFY(vehicle);
    auto* links = vehicle->vehicleLinkManager();
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QSignalSpy notifications(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification);
    QSignalSpy primaryChanges(links, &VehicleLinkManager::primaryLinkChanged);
    links->setPrimaryLinkByName(QStringLiteral("RFD"));
    QCOMPARE(primaryChanges.count(), 1);
    _preference->evaluateForTesting(1000);
    QCOMPARE(links->primaryLinkName(), QStringLiteral("RFD"));
    QCOMPARE(primaryChanges.count(), 1);
    QCOMPARE(notifications.count(), 0);
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
