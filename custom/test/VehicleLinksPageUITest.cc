#include "VehicleLinksPageUITest.h"

#include <memory>

#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/qscopeguard.h>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "AppSettings.h"
#include "COPController.h"
#include "LinkManager.h"
#include "LinkPreferenceController.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VehicleRoleController.h"
#include "VehicleSystemIdTestData.h"

UT_REGISTER_TEST(VehicleLinksPageUITest, TestLabel::Integration, TestLabel::Vehicle)

void VehicleLinksPageUITest::init()
{
    UnitTest::init();
    _failbackHold = QSettings().value(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    QVERIFY(_directory.isValid());
    QSettings().remove(QStringLiteral("COP"));
    // Vehicles and links persist under savePath; every test starts from an empty directory, before the UI loads them
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
}

void VehicleLinksPageUITest::cleanup()
{
    SettingsManager::instance()->appSettings()->savePath()->setRawValue(_savePath);
    QSettings settingsStore;
    if (_failbackHold.isValid()) {
        settingsStore.setValue(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"), _failbackHold);
    } else {
        settingsStore.remove(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    }
    QmlUITestBase::cleanup();
}

VehicleRoleController* VehicleLinksPageUITest::_roles() const
{
    return _engine ? _engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController") : nullptr;
}

QQuickItem* VehicleLinksPageUITest::_requireItem(const QString& objectName) const
{
    QQuickItem* const item = findItem(_rootItem, objectName);
    if (!item) {
        qWarning() << "Vehicle Links page has no item named" << objectName;
    }
    return item;
}

bool VehicleLinksPageUITest::_openPage()
{
    _window->resize(1280, 1000);
    return clickToolSelectDropdownButton(QStringLiteral("toolbar_viewAnalyze")) &&
           clickButton(QStringLiteral("analyzeButton_Vehicle Links")) &&
           findVisibleItem(_rootItem, QStringLiteral("vehicleLinksFlickable"), TestTimeout::mediumMs()) != nullptr;
}

void VehicleLinksPageUITest::_emptyPageSaysWhatToDo()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_openPage());

    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("noVehiclesConnectedHint"), TestTimeout::mediumMs()));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("noVehiclesSetUpHint"), TestTimeout::mediumMs()));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("newVehicleAdd"), TestTimeout::mediumMs()));
    QQuickItem* const toSetUp = _requireItem(QStringLiteral("vehiclesToSetUp"));
    QVERIFY(toSetUp);
    QVERIFY(!toSetUp->isVisible());
}

void VehicleLinksPageUITest::_vehicleAddedByHandGetsACard_data()
{
    VehicleSystemIdTestData::addValidRows();
}

void VehicleLinksPageUITest::_vehicleAddedByHandGetsACard()
{
    QFETCH(quint32, sysid);
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    QVERIFY(_openPage());

    QQuickItem* const sysidField = _requireItem(QStringLiteral("newVehicleSysid"));
    QQuickItem* const roleCombo = _requireItem(QStringLiteral("newVehicleRole"));
    QQuickItem* const nicknameField = _requireItem(QStringLiteral("newVehicleNickname"));
    QQuickItem* const addButton = _requireItem(QStringLiteral("newVehicleAdd"));
    QVERIFY(sysidField && roleCombo && nicknameField && addButton);

    // Not addable until it has a valid system ID
    QVERIFY(!addButton->isEnabled());
    QVERIFY(sysidField->setProperty("text", QString::number(sysid)));
    QVERIFY(roleCombo->setProperty("currentIndex", roles->availableRoles().indexOf(QStringLiteral("Rover"))));
    QVERIFY(nicknameField->setProperty("text", QStringLiteral("Scout")));
    QTRY_VERIFY_WITH_TIMEOUT(addButton->isEnabled(), TestTimeout::shortMs());
    QVERIFY(clickButton(QStringLiteral("newVehicleAdd")));

    QTRY_COMPARE_WITH_TIMEOUT(roles->roleForSysid(sysid), QStringLiteral("Rover"), TestTimeout::mediumMs());
    QCOMPARE(roles->nameForSysid(sysid), QStringLiteral("Scout"));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_%1").arg(sysid), TestTimeout::mediumMs()));
    QCOMPARE(_requireItem(QStringLiteral("vehicleNickname_%1").arg(sysid))->property("text").toString(),
             QStringLiteral("Scout"));
    QQuickItem* const hint = _requireItem(QStringLiteral("noVehiclesSetUpHint"));
    QVERIFY(hint);
    QTRY_VERIFY_WITH_TIMEOUT(!hint->isVisible(), TestTimeout::shortMs());
    // Ready for the next one
    QCOMPARE(sysidField->property("text").toString(), QString());
    QCOMPARE(nicknameField->property("text").toString(), QString());
}

void VehicleLinksPageUITest::_invalidSysidField_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("empty") << QString();
    QTest::newRow("zero") << QStringLiteral("0");
    QTest::newRow("negative") << QStringLiteral("-1");
    QTest::newRow("fraction") << QStringLiteral("1.5");
    QTest::newRow("overflow") << QStringLiteral("4294967296");
    QTest::newRow("text") << QStringLiteral("invalid");
    QTest::newRow("text-suffix") << QStringLiteral("255abc");
    QTest::newRow("exponent") << QStringLiteral("1e2");
    QTest::newRow("group-separator") << QStringLiteral("1,257");
}

void VehicleLinksPageUITest::_invalidSysidField()
{
    QFETCH(QString, text);
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_openPage());
    QQuickItem* const sysidField = _requireItem(QStringLiteral("newVehicleSysid"));
    QQuickItem* const addButton = _requireItem(QStringLiteral("newVehicleAdd"));
    QVERIFY(sysidField && addButton);
    // Start from an enabled button to catch a stale binding when the next input is invalid.
    QVERIFY(sysidField->setProperty("text", QStringLiteral("255")));
    QTRY_VERIFY_WITH_TIMEOUT(addButton->isEnabled(), TestTimeout::shortMs());
    QVERIFY(sysidField->setProperty("text", text));
    QTRY_VERIFY_WITH_TIMEOUT(!addButton->isEnabled(), TestTimeout::shortMs());
    QVERIFY(!sysidField->property("validSysid").toBool());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    QCOMPARE(roles->roleEntries()->count(), 0);
}

void VehicleLinksPageUITest::_connectedVehicleIsOfferedForOneClickSetup_data()
{
    QTest::addColumn<quint32>("requestedSysid");
    QTest::newRow("legacy") << quint32(128);
    QTest::newRow("wide") << quint32(4294967295U);
}

void VehicleLinksPageUITest::_connectedVehicleIsOfferedForOneClickSetup()
{
    QFETCH(quint32, requestedSysid);
    auto configuration = std::make_shared<MockConfiguration>(QStringLiteral("Vehicle Links setup"));
    configuration->setDynamic(true);
    configuration->setFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    configuration->setVehicleType(MAV_TYPE_QUADROTOR);
    configuration->setSystemId(requestedSysid);
    runWithMockLink(
        [configuration]() -> MockLink* {
            SharedLinkConfigurationPtr sharedConfiguration = configuration;
            if (!LinkManager::instance()->createConnectedLink(sharedConfiguration)) {
                return nullptr;
            }
            return qobject_cast<MockLink*>(configuration->link());
        },
        [this, requestedSysid](QPointer<MockLink> /*mockLink*/, Vehicle* vehicle) {
            VehicleRoleController* const roles = _roles();
            QVERIFY(roles);
            QVERIFY(vehicle);
            const quint32 sysid = vehicle->id();
            QCOMPARE(sysid, requestedSysid);
            QCOMPARE(MultiVehicleManager::instance()->getVehicleById(sysid), vehicle);
            auto* cop = _engine->singletonInstance<COPController*>("QGC", "COPController");
            QVERIFY(cop);
            cop->initialize(roles);
            cop->selectVehicle(0);
            QVERIFY(clickButton(QStringLiteral("copVehicleTab_%1").arg(sysid)));
            QTRY_COMPARE_WITH_TIMEOUT(cop->selectedSysid(), sysid, TestTimeout::shortMs());
            QVERIFY(cop->selected());
            QCOMPARE(cop->selected()->sysid(), sysid);
            QCOMPARE(cop->selected()->vehicle(), vehicle);
            QVERIFY(cop->selected()->connected());
            QVERIFY(_openPage());
            QVERIFY(findVisibleItem(_rootItem, QStringLiteral("setUpRow_%1").arg(sysid), TestTimeout::mediumMs()));
            // The type comes from the vehicle itself
            QQuickItem* const roleCombo = _requireItem(QStringLiteral("setUpRole_%1").arg(sysid));
            QVERIFY(roleCombo);
            QCOMPARE(roleCombo->property("currentText").toString(), QStringLiteral("Copter"));
            QQuickItem* const nicknameField = _requireItem(QStringLiteral("setUpNickname_%1").arg(sysid));
            QVERIFY(nicknameField);
            QVERIFY(nicknameField->setProperty("text", QStringLiteral("Mapper")));
            QVERIFY(clickButton(QStringLiteral("setUpAdd_%1").arg(sysid)));

            QTRY_COMPARE_WITH_TIMEOUT(roles->roleForSysid(sysid), QStringLiteral("Copter"), TestTimeout::mediumMs());
            QCOMPARE(roles->nameForSysid(sysid), QStringLiteral("Mapper"));
            QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_%1").arg(sysid), TestTimeout::mediumMs()));
            // Set up now, so it is no longer offered
            QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("setUpRow_%1").arg(sysid), 0) == nullptr,
                                     TestTimeout::mediumMs());
        });
}

void VehicleLinksPageUITest::_nicknameAlreadyInUseBlocksAdd()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    QVERIFY(_openPage());

    QQuickItem* const sysidField = _requireItem(QStringLiteral("newVehicleSysid"));
    QQuickItem* const nicknameField = _requireItem(QStringLiteral("newVehicleNickname"));
    QQuickItem* const addButton = _requireItem(QStringLiteral("newVehicleAdd"));
    QVERIFY(sysidField && nicknameField && addButton);

    // Case and surrounding spaces do not make a nickname different
    QVERIFY(sysidField->setProperty("text", QStringLiteral("212")));
    QVERIFY(nicknameField->setProperty("text", QStringLiteral("  scout ")));
    QTRY_VERIFY_WITH_TIMEOUT(nicknameField->property("taken").toBool(), TestTimeout::shortMs());
    QVERIFY(!addButton->isEnabled());

    QVERIFY(nicknameField->setProperty("text", QStringLiteral("Sentinel")));
    QTRY_VERIFY_WITH_TIMEOUT(addButton->isEnabled(), TestTimeout::shortMs());
    QVERIFY(clickButton(QStringLiteral("newVehicleAdd")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->nameForSysid(212), QStringLiteral("Sentinel"), TestTimeout::mediumMs());
    QCOMPARE(roles->nameForSysid(211), QStringLiteral("Scout"));
}

void VehicleLinksPageUITest::_linksBelongToTheirVehicle()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    roles->addEntry(1, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    roles->addEntry(257, QStringLiteral("Plane"), QStringLiteral("Sentinel"), 0);
    QVERIFY(_openPage());
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_1"), TestTimeout::mediumMs()));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_257"), TestTimeout::mediumMs()));

    QQuickItem* const linkName = _requireItem(QStringLiteral("vehicleNewLinkName_257"));
    QVERIFY(linkName);
    QVERIFY(linkName->setProperty("text", QStringLiteral("Microhard")));
    QVERIFY(clickButton(QStringLiteral("vehicleAddLink_257")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->linksForSysid(257)->count(), 1, TestTimeout::mediumMs());
    QCOMPARE(roles->linksForSysid(1)->count(), 0);
    QCOMPARE(linkName->property("text").toString(), QString());

    QVERIFY(clickButton(QStringLiteral("vehicleRemoveLink_257_0")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->linksForSysid(257)->count(), 0, TestTimeout::mediumMs());

    // Removing a vehicle takes its card with it and leaves the other alone
    QVERIFY(clickButton(QStringLiteral("vehicleRemove_257")));
    QTRY_VERIFY_WITH_TIMEOUT(roles->roleForSysid(257).isEmpty(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_257"), 0) == nullptr,
                             TestTimeout::mediumMs());
    QCOMPARE(roles->roleForSysid(1), QStringLiteral("Rover"));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_1"), TestTimeout::mediumMs()));
}

void VehicleLinksPageUITest::_preferredAndStatusTagsFollowVehicle_data()
{
    QTest::addColumn<quint32>("requestedSysid");
    QTest::newRow("one") << quint32(1);
    QTest::newRow("uint32-max") << quint32(4294967295U);
}

void VehicleLinksPageUITest::_preferredAndStatusTagsFollowVehicle()
{
    QFETCH(quint32, requestedSysid);
    runWithMockLink(
        [requestedSysid]() -> MockLink* {
            auto config = std::make_shared<MockConfiguration>(QStringLiteral("UI RFD"));
            config->setSystemId(requestedSysid);
            config->setDynamic(true);
            config->setIncrementVehicleId(false);
            SharedLinkConfigurationPtr sharedConfig = config;
            if (!LinkManager::instance()->createConnectedLink(sharedConfig)) {
                return nullptr;
            }
            return qobject_cast<MockLink*>(config->link());
        },
        [this, requestedSysid](QPointer<MockLink> backup, Vehicle* vehicle) {
            QVERIFY(backup && vehicle);
            auto config = std::make_shared<MockConfiguration>(QStringLiteral("UI UDP"));
            config->setDynamic(true);
            config->setIncrementVehicleId(false);
            config->setSystemId(requestedSysid);
            SharedLinkConfigurationPtr sharedConfig = config;
            QVERIFY(LinkManager::instance()->createConnectedLink(sharedConfig));
            QPointer<MockLink> preferred = qobject_cast<MockLink*>(config->link());
            const auto disconnectPreferred = qScopeGuard([&preferred]() {
                if (preferred) {
                    preferred->disconnect();
                }
            });
            QVERIFY(preferred);
            QCOMPARE(preferred->vehicleId(), backup->vehicleId());
            auto* links = vehicle->vehicleLinkManager();
            QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
            auto* roles = _roles();
            QVERIFY(roles);
            auto* preference = roles->findChild<LinkPreferenceController*>();
            QVERIFY(preference);
            preference->setTimingForTesting({.initialSettleMs = 3, .baseHoldMs = 15}, 0);
            const quint32 sysid = vehicle->id();
            QCOMPARE(sysid, requestedSysid);
            roles->addEntry(sysid, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
            roles->addLink(0, QStringLiteral("RFD"), QStringLiteral("UI RFD"));
            roles->addLink(0, QStringLiteral("UDP"), QStringLiteral("UI UDP"));
            const quint32 otherSysid = sysid == 1 ? 4294967295U : 1;
            roles->addEntry(otherSysid, QStringLiteral("Plane"), QStringLiteral("Other"), 0);
            roles->addLink(1, QStringLiteral("UDP"), QStringLiteral("UI UDP"));
            QVERIFY(_openPage());
            QQuickItem* const firstPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_0").arg(sysid));
            QQuickItem* const secondPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_1").arg(sysid));
            QQuickItem* const secondLost = _requireItem(QStringLiteral("vehicleLinkLost_%1_1").arg(sysid));
            QQuickItem* const otherPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_0").arg(otherSysid));
            QQuickItem* const otherLost = _requireItem(QStringLiteral("vehicleLinkLost_%1_0").arg(otherSysid));
            QVERIFY(firstPrimary && secondPrimary && secondLost && otherPrimary && otherLost);
            QVERIFY(firstPrimary->isVisible());
            QVERIFY(!secondPrimary->isVisible() && !secondLost->isVisible());
            QVERIFY(!otherPrimary->isVisible() && !otherLost->isVisible());
            QVERIFY(clickButtonScrolled(QStringLiteral("vehiclePreferLink_%1_1").arg(sysid),
                                        QStringLiteral("vehicleLinksFlickable")));
            QCOMPARE(roles->preferredLinkConfigNameForSysid(sysid), QStringLiteral("UI UDP"));
            QVERIFY(roles->preferredLinkConfigNameForSysid(otherSysid).isEmpty());
            preference->evaluateForTesting(3);
            QTRY_VERIFY_WITH_TIMEOUT(secondPrimary->isVisible(), TestTimeout::shortMs());
            QVERIFY(!firstPrimary->isVisible());

            ignoreLogMessage("Vehicle.StandardModes", QtWarningMsg,
                             QRegularExpression(QStringLiteral("Failed to retrieve available modes")));
            expectAppMessage(QRegularExpression(QStringLiteral("Switching communication to secondary link")));
            preferred->setCommLost(true);
            QTRY_VERIFY_WITH_TIMEOUT(secondLost->isVisible(), TestTimeout::longMs());
            QTRY_VERIFY_WITH_TIMEOUT(firstPrimary->isVisible(), TestTimeout::shortMs());
            QVERIFY(!secondPrimary->isVisible());
            verifyExpectedLogMessage();
            QVERIFY(acceptDialog());
            preferred->setCommLost(false);
            QTRY_VERIFY_WITH_TIMEOUT(!secondLost->isVisible(), TestTimeout::longMs());
            QVERIFY(firstPrimary->isVisible());
            QVERIFY(!otherPrimary->isVisible() && !otherLost->isVisible());
            preference->evaluateForTesting(18);
            QTRY_VERIFY_WITH_TIMEOUT(secondPrimary->isVisible(), TestTimeout::shortMs());

            QQuickItem* const hold = _requireItem(QStringLiteral("failbackHoldSeconds"));
            QVERIFY(hold);
            QVERIFY(hold->setProperty("text", QStringLiteral("30")));
            QVERIFY(QMetaObject::invokeMethod(hold, "editingFinished"));
            QCOMPARE(roles->failbackHoldSeconds(), 30);
            preferred->disconnect();
            QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 1, TestTimeout::longMs());
            QVERIFY(!secondPrimary->isVisible() && !secondLost->isVisible());
            QQuickItem* const notAttached = _requireItem(QStringLiteral("vehicleLinkNotAttached_%1_1").arg(sysid));
            QVERIFY(notAttached);
            QTRY_VERIFY_WITH_TIMEOUT(notAttached->isVisible(), TestTimeout::shortMs());
            QCOMPARE(notAttached->property("text").toString(), QStringLiteral("Not connected to this vehicle"));
            backup->disconnect();
            QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->vehicles()->count(), 0, TestTimeout::longMs());
            QTRY_VERIFY_WITH_TIMEOUT(!firstPrimary->isVisible(), TestTimeout::shortMs());
            QTRY_VERIFY_WITH_TIMEOUT(!notAttached->isVisible(), TestTimeout::shortMs());
            QVERIFY(!secondPrimary->isVisible() && !secondLost->isVisible());
            QTRY_VERIFY_WITH_TIMEOUT(!backup, TestTimeout::longMs());
        });
}
