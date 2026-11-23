#include "GuidedActionTargetUITest.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QTimer>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "Fact.h"
#include "GimbalController.h"
#include "GimbalControllerSettings.h"
#include "LinkManager.h"
#include "MAVLinkSigningKeys.h"
#include "MissionManager.h"
#include "MockConfiguration.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VehicleSigningController.h"

UT_REGISTER_TEST(GuidedActionTargetUITest, TestLabel::Integration, TestLabel::Vehicle)

namespace {

QString assumeRoverControlButton()
{
    return QStringLiteral("flyToolStrip_assumeRoverControlButton");
}

QString returnToAutoButton()
{
    return QStringLiteral("flyToolStrip_returnToAutoButton");
}

}  // namespace

void GuidedActionTargetUITest::cleanup()
{
    for (const auto& link : std::as_const(_links)) {
        if (link) {
            link->disconnect();
        }
    }
    (void) waitForCondition([] { return MultiVehicleManager::instance()->vehicles()->count() == 0; },
                            TestTimeout::longMs(), QStringLiteral("all test vehicles removed"));
    _links.clear();
    _configs.clear();
    QmlUITestBase::cleanup();
}

MockLink* GuidedActionTargetUITest::_startVehicle(int index, MAV_AUTOPILOT firmware, MAV_TYPE type, bool takeoff,
                                                  bool gimbal, bool preloadMission)
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

    auto config = std::make_shared<MockConfiguration>(QStringLiteral("Guided target %1").arg(index));
    config->setDynamic(true);
    config->setFirmwareType(firmware);
    config->setVehicleType(type);
    config->setEnableGimbal(gimbal);
    config->setPreloadMission(preloadMission);
    config->setIncrementVehicleId(true);
    SharedLinkConfigurationPtr sharedConfig = config;
    _configs.append(sharedConfig);
    if (!LinkManager::instance()->createConnectedLink(sharedConfig)) {
        return nullptr;
    }
    auto* link = qobject_cast<MockLink*>(config->link());
    if (!link) {
        return nullptr;
    }
    _links.append(link);

    auto* manager = MultiVehicleManager::instance();
    if (!waitForCondition([manager, link] { return manager->getVehicleById(link->vehicleId()) != nullptr; },
                          TestTimeout::longMs(), QStringLiteral("vehicle connected"))) {
        return nullptr;
    }
    Vehicle* const vehicle = manager->getVehicleById(link->vehicleId());
    if (!waitForCondition([vehicle] { return vehicle->isInitialConnectComplete(); }, TestTimeout::longMs(),
                          QStringLiteral("initial connect complete"))) {
        return nullptr;
    }

    if (takeoff) {
        // MockLink arms and climbs above home on takeoff
        vehicle->sendMavCommand(vehicle->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, false /* showError */, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 0.0f, 10.0f /* altitude */);
        if (!waitForCondition([vehicle] { return vehicle->armed() && vehicle->airborne(); }, TestTimeout::longMs(),
                              QStringLiteral("armed and airborne"))) {
            return nullptr;
        }
    }
    return link;
}

bool GuidedActionTargetUITest::_openEmergencyStopConfirmation()
{
    QQuickItem* const indicator =
        findVisibleItem(_rootItem, QStringLiteral("toolbar_mainStatusIndicator"), TestTimeout::mediumMs());
    if (!indicator || !_clickItemAt(indicator, 0.5, 0.5, QStringLiteral("toolbar_mainStatusIndicator"))) {
        return false;
    }
    QQuickItem* const stopButton =
        findVisibleItem(_rootItem, QStringLiteral("mainStatusEmergencyStopButton"), TestTimeout::mediumMs());
    if (!stopButton || !_clickItemAt(stopButton, 0.5, 0.5, QStringLiteral("mainStatusEmergencyStopButton"))) {
        return false;
    }
    return findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), TestTimeout::mediumMs()) != nullptr;
}

QQuickItem* GuidedActionTargetUITest::_openToolStripConfirmation(const QString& toolStripButton, const QString& title)
{
    if (!clickButton(toolStripButton)) {
        return nullptr;
    }
    QQuickItem* const confirmButton =
        findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), TestTimeout::mediumMs());
    if (!confirmButton || confirmButton->property("text").toString() != title) {
        return nullptr;
    }
    return confirmButton;
}

bool GuidedActionTargetUITest::_confirmationHidden()
{
    return findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), 0) == nullptr;
}

void GuidedActionTargetUITest::_emergencyStopConfirmationStaysWithItsVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();

    // The airborne transition creates QGCPressure, which warns on hosts without a pressure backend
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Failed to connect to pressure backend")));
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Error Initializing Pressure Sensor")));

    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, true);
    QVERIFY(firstLink);
    QPointer<MockLink> secondLink = _startVehicle(1, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, true);
    QVERIFY(secondLink);

    auto* manager = MultiVehicleManager::instance();
    Vehicle* const first = manager->getVehicleById(firstLink->vehicleId());
    Vehicle* const second = manager->getVehicleById(secondLink->vehicleId());
    QVERIFY(first && second && (first != second));

    // Counts the arm/disarm commands each vehicle has answered since the connection was made
    int firstArmDisarmResults = 0;
    int secondArmDisarmResults = 0;
    const auto countArmDisarm = [](int& count) {
        return [&count](quint32, int, int command, int, int) {
            if (command == MAV_CMD_COMPONENT_ARM_DISARM) {
                ++count;
            }
        };
    };
    connect(first, &Vehicle::mavCommandResult, this, countArmDisarm(firstArmDisarmResults));
    connect(second, &Vehicle::mavCommandResult, this, countArmDisarm(secondArmDisarmResults));

    // A confirmation opened for the first vehicle is cancelled when the second becomes active. Even a hold that
    // completes after the switch must not reach the second vehicle.
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::mediumMs());
    QVERIFY(_openEmergencyStopConfirmation());
    manager->setActiveVehicle(second);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == second, TestTimeout::mediumMs());
    QVERIFY_TRUE_WAIT(_confirmationHidden(), TestTimeout::mediumMs());
    QQuickItem* const staleButton = findItem(_rootItem, QStringLiteral("guidedActionConfirmButton"));
    QVERIFY(staleButton);
    QVERIFY(QMetaObject::invokeMethod(staleButton, "activated"));
    QVERIFY(!QTest::qWaitFor([&] { return secondArmDisarmResults > 0; }, TestTimeout::shortMs()));
    QVERIFY(second->armed());
    QVERIFY(first->armed());

    // The same when the vehicle the confirmation was opened for goes away: the surviving vehicle is not stopped
    QVERIFY(_openEmergencyStopConfirmation());
    secondLink->disconnect();
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(_confirmationHidden(), TestTimeout::mediumMs());
    QVERIFY(QMetaObject::invokeMethod(staleButton, "activated"));
    QVERIFY(!QTest::qWaitFor([&] { return firstArmDisarmResults > 0; }, TestTimeout::shortMs()));
    QVERIFY(first->armed());

    // A confirmation that was not interrupted still stops its own vehicle
    QVERIFY(_openEmergencyStopConfirmation());
    QQuickItem* const confirmButton =
        findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), TestTimeout::mediumMs());
    QVERIFY(confirmButton);
    QCOMPARE(confirmButton->property("text").toString(), QStringLiteral("EMERGENCY STOP"));
    QVERIFY(QMetaObject::invokeMethod(confirmButton, "activated"));
    QVERIFY_TRUE_WAIT(!first->armed(), TestTimeout::mediumMs());
}

void GuidedActionTargetUITest::_cancelledConfirmationCannotBeActivated()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();

    // The airborne transition creates QGCPressure, which warns on hosts without a pressure backend
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Failed to connect to pressure backend")));
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Error Initializing Pressure Sensor")));

    QPointer<MockLink> link = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, true);
    QVERIFY(link);
    auto* manager = MultiVehicleManager::instance();
    Vehicle* const vehicle = manager->getVehicleById(link->vehicleId());
    QVERIFY(vehicle);
    manager->setActiveVehicle(vehicle);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == vehicle, TestTimeout::mediumMs());

    // Emergency Stop stops being applicable once the vehicle is disarmed, which cancels its open confirmation.
    // A hold that completes after that must not send the command to the same vehicle either.
    QVERIFY(_openEmergencyStopConfirmation());
    QQuickItem* const staleButton =
        findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), TestTimeout::mediumMs());
    QVERIFY(staleButton);
    vehicle->setArmed(false, false /* showError */);
    QVERIFY_TRUE_WAIT(!vehicle->armed(), TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(_confirmationHidden(), TestTimeout::mediumMs());

    int armDisarmResults = 0;
    const QMetaObject::Connection resultConnection =
        connect(vehicle, &Vehicle::mavCommandResult, this, [&armDisarmResults](quint32, int, int command, int, int) {
            if (command == MAV_CMD_COMPONENT_ARM_DISARM) {
                ++armDisarmResults;
            }
        });
    const auto disconnectGuard = qScopeGuard([resultConnection] { QObject::disconnect(resultConnection); });
    QVERIFY(QMetaObject::invokeMethod(staleButton, "activated"));
    QVERIFY(!QTest::qWaitFor([&] { return armDisarmResults > 0; }, TestTimeout::shortMs()));
}

void GuidedActionTargetUITest::_planRemovalConfirmationStaysWithItsVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();

    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false);
    QVERIFY(firstLink);
    QPointer<MockLink> secondLink = _startVehicle(1, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false);
    QVERIFY(secondLink);

    auto* manager = MultiVehicleManager::instance();
    Vehicle* const first = manager->getVehicleById(firstLink->vehicleId());
    Vehicle* const second = manager->getVehicleById(secondLink->vehicleId());
    QVERIFY(first && second && (first != second));

    int firstRemovals = 0;
    int secondRemovals = 0;
    connect(first->missionManager(), &MissionManager::removeAllComplete, this, [&](bool) { ++firstRemovals; });
    connect(second->missionManager(), &MissionManager::removeAllComplete, this, [&](bool) { ++secondRemovals; });

    const auto openRemovalConfirmation = [this] {
        QQuickItem* const clearButton =
            findVisibleItem(_rootItem, QStringLiteral("planToolbar_clearButton"), TestTimeout::mediumMs());
        return clearButton &&
               QTest::qWaitFor([clearButton] { return clearButton->isEnabled(); }, TestTimeout::longMs()) &&
               _clickItemAt(clearButton, 0.5, 0.5, QStringLiteral("planToolbar_clearButton")) &&
               waitForDialog(QStringLiteral("Clear"));
    };

    // A removal confirmed after another vehicle became active removes nothing, and says so
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::mediumMs());
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewPlan")));
    QVERIFY(openRemovalConfirmation());
    manager->setActiveVehicle(second);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == second, TestTimeout::mediumMs());
    QVERIFY(acceptDialog());
    QVERIFY(waitForDialog(QStringLiteral("Plan not removed")));
    QVERIFY(acceptDialog());
    QVERIFY(!QTest::qWaitFor([&] { return (firstRemovals + secondRemovals) > 0; }, TestTimeout::shortMs()));

    // A removal confirmed for the vehicle that is still active goes through to that vehicle only
    QVERIFY(openRemovalConfirmation());
    QVERIFY(acceptDialog());
    QVERIFY_TRUE_WAIT(secondRemovals > 0, TestTimeout::mediumMs());
    QCOMPARE(firstRemovals, 0);
}

void GuidedActionTargetUITest::_missionCompleteDialogStaysWithItsVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();

    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Failed to connect to pressure backend")));
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Error Initializing Pressure Sensor")));
    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, true, false, true);
    QVERIFY(firstLink);
    QPointer<MockLink> secondLink = _startVehicle(1, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false);
    QVERIFY(secondLink);

    auto* manager = MultiVehicleManager::instance();
    Vehicle* const first = manager->getVehicleById(firstLink->vehicleId());
    Vehicle* const second = manager->getVehicleById(secondLink->vehicleId());
    QVERIFY(first && second && first != second);
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(first->initialPlanRequestComplete(), TestTimeout::longMs());
    first->setFlightMode(first->missionFlightMode());
    QTRY_COMPARE_WITH_TIMEOUT(first->flightMode(), first->missionFlightMode(), TestTimeout::longMs());
    first->setArmed(false, false);
    QVERIFY_TRUE_WAIT(!first->armed(), TestTimeout::longMs());

    QQuickItem* const factory = findItem(_rootItem, QStringLiteral("missionCompleteDialogFactory"));
    QVERIFY(factory);
    const auto openDialog = [factory, first] {
        QVariantMap properties;
        properties.insert(QStringLiteral("activeVehicleCopy"), QVariant::fromValue(first));
        QVariant result;
        return QMetaObject::invokeMethod(factory, "open", Q_RETURN_ARG(QVariant, result),
                                         Q_ARG(QVariant, QVariant(properties)));
    };

    firstLink->clearReceivedMavlinkMessageCounts();
    QVERIFY(waitForDialog(QStringLiteral("Flight Plan complete"), TestTimeout::longMs()));
    QVERIFY(clickButton(QStringLiteral("missionCompleteRemoveButton")));
    QVERIFY_TRUE_WAIT(firstLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_CLEAR_ALL) > 0,
                      TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(!first->missionManager()->inProgress(), TestTimeout::longMs());

    QVERIFY(openDialog());
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("missionCompleteRemoveButton"), TestTimeout::longMs()));
    secondLink->clearReceivedMavlinkMessageCounts();
    manager->setActiveVehicle(second);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == second, TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(!dialogVisible(QStringLiteral("Flight Plan complete")), TestTimeout::longMs());
    QVERIFY(
        !QTest::qWaitFor([&] { return secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_CLEAR_ALL) > 0; },
                         TestTimeout::shortMs()));
    QCOMPARE(secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_MISSION_CLEAR_ALL), 0);
}

void GuidedActionTargetUITest::_gimbalDragStaysWithItsVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    auto* settings = SettingsManager::instance()->gimbalControllerSettings();
    const QVariant oldEnabled = settings->enableOnScreenControl()->rawValue();
    const QVariant oldDrag = settings->clickAndDrag()->rawValue();
    const auto restoreSettings = qScopeGuard([settings, oldEnabled, oldDrag] {
        settings->enableOnScreenControl()->setRawValue(oldEnabled);
        settings->clickAndDrag()->setRawValue(oldDrag);
    });
    settings->enableOnScreenControl()->setRawValue(true);
    settings->clickAndDrag()->setRawValue(true);

    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false, true);
    QVERIFY(firstLink);
    QPointer<MockLink> secondLink = _startVehicle(1, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false, true);
    QVERIFY(secondLink);
    auto* manager = MultiVehicleManager::instance();
    Vehicle* const first = manager->getVehicleById(firstLink->vehicleId());
    Vehicle* const second = manager->getVehicleById(secondLink->vehicleId());
    QVERIFY(first && second && first != second);
    QVERIFY_TRUE_WAIT(first->gimbalController()->activeGimbal() && second->gimbalController()->activeGimbal(),
                      TestTimeout::longMs());
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::longMs());

    QQuickItem* const controller = findItem(_rootItem, QStringLiteral("onScreenGimbalController"));
    QVERIFY(controller);
    QObject* const timer = controller->findChild<QObject*>(QStringLiteral("gimbalSendRateTimer"));
    QVERIFY(timer);
    const auto drag = [controller] {
        return QMetaObject::invokeMethod(controller, "mouseDragStart", Q_ARG(QVariant, QVariant(20)),
                                         Q_ARG(QVariant, QVariant(20))) &&
               QMetaObject::invokeMethod(controller, "mouseDragPositionChanged", Q_ARG(QVariant, QVariant(40)),
                                         Q_ARG(QVariant, QVariant(40)));
    };
    firstLink->clearReceivedMavCommandCounts();
    QVERIFY(drag());
    QVERIFY_TRUE_WAIT(firstLink->receivedMavCommandCount(MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW) > 0,
                      TestTimeout::longMs());
    QVERIFY(timer->property("running").toBool());
    secondLink->clearReceivedMavCommandCounts();
    firstLink->disconnect();
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == second, TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(!timer->property("running").toBool(), TestTimeout::longMs());
    QVERIFY(
        !QTest::qWaitFor([&] { return secondLink->receivedMavCommandCount(MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW) > 0; },
                         TestTimeout::shortMs()));
    QCOMPARE(secondLink->receivedMavCommandCount(MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW), 0);
    QVERIFY(drag());
    QVERIFY_TRUE_WAIT(secondLink->receivedMavCommandCount(MAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW) > 0,
                      TestTimeout::longMs());
    QVERIFY(QMetaObject::invokeMethod(controller, "mouseDragEnd"));
}

void GuidedActionTargetUITest::_signingKeyConfirmationStaysWithItsVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false);
    QVERIFY(firstLink);
    QPointer<MockLink> secondLink = _startVehicle(1, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false);
    QVERIFY(secondLink);
    auto* manager = MultiVehicleManager::instance();
    Vehicle* const first = manager->getVehicleById(firstLink->vehicleId());
    Vehicle* const second = manager->getVehicleById(secondLink->vehicleId());
    QVERIFY(first && second && first != second);

    auto* keys = MAVLinkSigningKeys::instance();
    const QString keyName = QStringLiteral("target-test-key");
    QVERIFY(keys->addRawKey(keyName, QString(64, QLatin1Char('1'))));
    const auto removeKey = qScopeGuard([keys, keyName] { keys->removeKey(keyName); });
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::longMs());
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewSettings")));
    QQuickItem* const pageButton = findVisibleItem(_rootItem, QStringLiteral("settingsButton_Telemetry"));
    QVERIFY(pageButton);
    QVERIFY(scrollIntoView(pageButton, QStringLiteral("settings_buttonList")));
    QVERIFY(_clickItemAt(pageButton, 0.5, 0.5, QStringLiteral("settingsButton_Telemetry")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("settingsPage_Telemetry"), TestTimeout::longMs()));
    QQuickItem* enable = findItem(_rootItem, QStringLiteral("signingKeyEnable_") + keyName);
    QVERIFY(enable);
    QVERIFY(scrollIntoView(enable, QStringLiteral("settingsPageFlickable")));
    QVERIFY(_clickItemAt(enable, 0.5, 0.5, QStringLiteral("signing key enable")));
    QVERIFY(waitForDialog(QStringLiteral("Send Signing Key"), TestTimeout::longMs()));
    manager->setActiveVehicle(second);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == second, TestTimeout::longMs());
    secondLink->clearReceivedMavlinkMessageCounts();
    QVERIFY(acceptDialog());
    QVERIFY(!QTest::qWaitFor([&] { return secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_SETUP_SIGNING) > 0; },
                             TestTimeout::shortMs()));
    QVERIFY(waitForDialog(QStringLiteral("Send Signing Key"), TestTimeout::longMs()));
    QVERIFY(acceptDialog());
    QCOMPARE(secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_SETUP_SIGNING), 0);

    enable = findItem(_rootItem, QStringLiteral("signingKeyEnable_") + keyName);
    QVERIFY(enable);
    QVERIFY(scrollIntoView(enable, QStringLiteral("settingsPageFlickable")));
    QVERIFY(_clickItemAt(enable, 0.5, 0.5, QStringLiteral("signing key enable")));
    QVERIFY(waitForDialog(QStringLiteral("Send Signing Key"), TestTimeout::longMs()));
    QVERIFY(acceptDialog());
    QVERIFY_TRUE_WAIT(secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_SETUP_SIGNING) > 0, TestTimeout::longMs());
    secondLink->disconnect();
    QVERIFY_TRUE_WAIT(manager->getVehicleById(secondLink ? secondLink->vehicleId() : -1) == nullptr,
                      TestTimeout::longMs());
}

void GuidedActionTargetUITest::_signingKeyConfirmationStaysWithItsLink()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_PX4, MAV_TYPE_QUADROTOR, false);
    QVERIFY(firstLink);
    auto* manager = MultiVehicleManager::instance();
    Vehicle* const vehicle = manager->getVehicleById(firstLink->vehicleId());
    QVERIFY(vehicle);

    const QString secondName = QStringLiteral("Guided target second link");
    auto config = std::make_shared<MockConfiguration>(secondName);
    config->setDynamic(true);
    config->setFirmwareType(MAV_AUTOPILOT_PX4);
    config->setVehicleType(MAV_TYPE_QUADROTOR);
    config->setIncrementVehicleId(false);
    config->setSystemId(firstLink->vehicleId());
    SharedLinkConfigurationPtr sharedConfig = config;
    _configs.append(sharedConfig);
    QVERIFY(LinkManager::instance()->createConnectedLink(sharedConfig));
    QPointer<MockLink> secondLink = qobject_cast<MockLink*>(config->link());
    QVERIFY(secondLink);
    _links.append(secondLink);
    auto* links = vehicle->vehicleLinkManager();
    QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
    links->setPrimaryLinkByName(QStringLiteral("Guided target 0"));
    QCOMPARE(links->primaryLinkName(), QStringLiteral("Guided target 0"));

    auto* keys = MAVLinkSigningKeys::instance();
    const QString keyName = QStringLiteral("target-link-test-key");
    QVERIFY(keys->addRawKey(keyName, QString(64, QLatin1Char('1'))));
    const auto removeKey = qScopeGuard([keys, keyName] { keys->removeKey(keyName); });
    manager->setActiveVehicle(vehicle);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == vehicle, TestTimeout::longMs());
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewSettings")));
    QQuickItem* const pageButton = findVisibleItem(_rootItem, QStringLiteral("settingsButton_Telemetry"));
    QVERIFY(pageButton);
    QVERIFY(scrollIntoView(pageButton, QStringLiteral("settings_buttonList")));
    QVERIFY(_clickItemAt(pageButton, 0.5, 0.5, QStringLiteral("settingsButton_Telemetry")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("settingsPage_Telemetry"), TestTimeout::longMs()));
    const auto openConfirmation = [this, &keyName] {
        QQuickItem* const enable = findItem(_rootItem, QStringLiteral("signingKeyEnable_") + keyName);
        return enable && scrollIntoView(enable, QStringLiteral("settingsPageFlickable")) &&
               _clickItemAt(enable, 0.5, 0.5, QStringLiteral("signing key enable")) &&
               waitForDialog(QStringLiteral("Send Signing Key"), TestTimeout::longMs());
    };
    QVERIFY(openConfirmation());
    links->setPrimaryLinkByName(secondName);
    QCOMPARE(links->primaryLinkName(), secondName);
    firstLink->clearReceivedMavlinkMessageCounts();
    secondLink->clearReceivedMavlinkMessageCounts();
    QVERIFY(acceptDialog());
    QVERIFY(!QTest::qWaitFor(
        [&] {
            return firstLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_SETUP_SIGNING) > 0 ||
                   secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_SETUP_SIGNING) > 0;
        },
        TestTimeout::shortMs()));
    QVERIFY(waitForDialog(QStringLiteral("Send Signing Key"), TestTimeout::longMs()));
    const auto containsNotice = [](const auto& self, QQuickItem* item) -> bool {
        if (!item || !item->isVisible()) {
            return false;
        }
        if (item->property("text").toString() ==
            QStringLiteral("The active vehicle or its link changed. The signing key was not sent.")) {
            return true;
        }
        for (QQuickItem* child : item->childItems()) {
            if (self(self, child)) {
                return true;
            }
        }
        return false;
    };
    QVERIFY(containsNotice(containsNotice, _rootItem));
    QVERIFY(acceptDialog());
    QVERIFY(openConfirmation());
    QVERIFY(acceptDialog());
    QVERIFY_TRUE_WAIT(secondLink->receivedMavlinkMessageCount(MAVLINK_MSG_ID_SETUP_SIGNING) > 0, TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(vehicle->signingController()->signingStatus().enabled, TestTimeout::longMs());
    const int vehicleId = vehicle->id();
    firstLink->disconnect();
    secondLink->disconnect();
    QVERIFY_TRUE_WAIT(manager->getVehicleById(vehicleId) == nullptr, TestTimeout::longMs());
}

void GuidedActionTargetUITest::_roverTakeoverAndReturnToAuto()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();

    QPointer<MockLink> roverLink = _startVehicle(0, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER, false);
    QVERIFY(roverLink);
    auto* manager = MultiVehicleManager::instance();
    Vehicle* const rover = manager->getVehicleById(roverLink->vehicleId());
    QVERIFY(rover && rover->rover());
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == rover, TestTimeout::mediumMs());

    // The operator can only take over from a mode other than Manual, so start in Auto
    rover->setFlightMode(QStringLiteral("Auto"));
    QTRY_COMPARE_WITH_TIMEOUT(rover->flightMode(), QStringLiteral("Auto"), TestTimeout::longMs());
    QVERIFY(findVisibleItem(_rootItem, assumeRoverControlButton(), TestTimeout::mediumMs()));
    QVERIFY(findVisibleItem(_rootItem, returnToAutoButton(), 0) == nullptr);

    QQuickItem* confirmButton =
        _openToolStripConfirmation(assumeRoverControlButton(), QStringLiteral("Assume Rover Control"));
    QVERIFY(confirmButton);
    QVERIFY(QMetaObject::invokeMethod(confirmButton, "activated"));
    QTRY_COMPARE_WITH_TIMEOUT(rover->flightMode(), QStringLiteral("Manual"), TestTimeout::longMs());

    // A confirmed switch raises no warning, and the strip now offers the way back
    QVERIFY(findVisibleItem(_rootItem, returnToAutoButton(), TestTimeout::mediumMs()));
    QVERIFY(findVisibleItem(_rootItem, assumeRoverControlButton(), 0) == nullptr);
    QVERIFY(!QTest::qWaitFor([this] { return dialogVisible(QStringLiteral("Assume Rover Control")); },
                             TestTimeout::mediumMs()));

    confirmButton = _openToolStripConfirmation(returnToAutoButton(), QStringLiteral("Return to Auto"));
    QVERIFY(confirmButton);
    QVERIFY(QMetaObject::invokeMethod(confirmButton, "activated"));
    QTRY_COMPARE_WITH_TIMEOUT(rover->flightMode(), QStringLiteral("Auto"), TestTimeout::longMs());
    QVERIFY(findVisibleItem(_rootItem, assumeRoverControlButton(), TestTimeout::mediumMs()));
    QVERIFY(
        !QTest::qWaitFor([this] { return dialogVisible(QStringLiteral("Return to Auto")); }, TestTimeout::mediumMs()));
}

void GuidedActionTargetUITest::_roverTakeoverConfirmationStaysWithRover()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    LinkManager::instance()->setConnectionsAllowed();

    QPointer<MockLink> firstLink = _startVehicle(0, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER, false);
    QVERIFY(firstLink);
    QPointer<MockLink> secondLink = _startVehicle(1, MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_GROUND_ROVER, false);
    QVERIFY(secondLink);

    auto* manager = MultiVehicleManager::instance();
    Vehicle* const first = manager->getVehicleById(firstLink->vehicleId());
    Vehicle* const second = manager->getVehicleById(secondLink->vehicleId());
    QVERIFY(first && second && (first != second) && first->rover() && second->rover());

    for (Vehicle* const rover : {first, second}) {
        manager->setActiveVehicle(rover);
        QVERIFY_TRUE_WAIT(manager->activeVehicle() == rover, TestTimeout::mediumMs());
        rover->setFlightMode(QStringLiteral("Auto"));
        QTRY_COMPARE_WITH_TIMEOUT(rover->flightMode(), QStringLiteral("Auto"), TestTimeout::longMs());
    }

    // Switching rovers while the takeover is being confirmed cancels it. A hold that still completes afterwards
    // must not put either rover into Manual.
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::mediumMs());
    QQuickItem* const confirmButton =
        _openToolStripConfirmation(assumeRoverControlButton(), QStringLiteral("Assume Rover Control"));
    QVERIFY(confirmButton);
    manager->setActiveVehicle(second);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == second, TestTimeout::mediumMs());
    QVERIFY_TRUE_WAIT(_confirmationHidden(), TestTimeout::mediumMs());
    QVERIFY(QMetaObject::invokeMethod(confirmButton, "activated"));
    QVERIFY(!QTest::qWaitFor(
        [&] {
            return (first->flightMode() != QStringLiteral("Auto")) || (second->flightMode() != QStringLiteral("Auto"));
        },
        TestTimeout::shortMs()));

    // Back on the first rover, a fresh confirmation still takes over that rover only
    manager->setActiveVehicle(first);
    QVERIFY_TRUE_WAIT(manager->activeVehicle() == first, TestTimeout::mediumMs());
    QQuickItem* const freshButton =
        _openToolStripConfirmation(assumeRoverControlButton(), QStringLiteral("Assume Rover Control"));
    QVERIFY(freshButton);
    QVERIFY(QMetaObject::invokeMethod(freshButton, "activated"));
    QTRY_COMPARE_WITH_TIMEOUT(first->flightMode(), QStringLiteral("Manual"), TestTimeout::longMs());
    QCOMPARE(second->flightMode(), QStringLiteral("Auto"));
}
