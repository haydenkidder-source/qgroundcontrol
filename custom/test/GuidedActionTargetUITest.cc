#include "GuidedActionTargetUITest.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "LinkManager.h"
#include "MissionManager.h"
#include "MockConfiguration.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

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

MockLink* GuidedActionTargetUITest::_startVehicle(int index, MAV_AUTOPILOT firmware, MAV_TYPE type, bool takeoff)
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
        return [&count](int, int, int command, int, int) {
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
