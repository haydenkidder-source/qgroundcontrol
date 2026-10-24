#include "VehicleLinksPageUITest.h"

#include <QtCore/QDir>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QTest>

#include "AppSettings.h"
#include "MockLink.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VehicleRoleController.h"

UT_REGISTER_TEST(VehicleLinksPageUITest, TestLabel::Integration, TestLabel::Vehicle)

void VehicleLinksPageUITest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
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

void VehicleLinksPageUITest::_vehicleAddedByHandGetsACard()
{
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
    QVERIFY(sysidField->setProperty("text", QStringLiteral("211")));
    QVERIFY(roleCombo->setProperty("currentIndex", roles->availableRoles().indexOf(QStringLiteral("Rover"))));
    QVERIFY(nicknameField->setProperty("text", QStringLiteral("Scout")));
    QTRY_VERIFY_WITH_TIMEOUT(addButton->isEnabled(), TestTimeout::shortMs());
    QVERIFY(clickButton(QStringLiteral("newVehicleAdd")));

    QTRY_COMPARE_WITH_TIMEOUT(roles->roleForSysid(211), QStringLiteral("Rover"), TestTimeout::mediumMs());
    QCOMPARE(roles->nameForSysid(211), QStringLiteral("Scout"));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_211"), TestTimeout::mediumMs()));
    QCOMPARE(_requireItem(QStringLiteral("vehicleNickname_211"))->property("text").toString(), QStringLiteral("Scout"));
    QQuickItem* const hint = _requireItem(QStringLiteral("noVehiclesSetUpHint"));
    QVERIFY(hint);
    QTRY_VERIFY_WITH_TIMEOUT(!hint->isVisible(), TestTimeout::shortMs());
    // Ready for the next one
    QCOMPARE(sysidField->property("text").toString(), QString());
    QCOMPARE(nicknameField->property("text").toString(), QString());
}

void VehicleLinksPageUITest::_connectedVehicleIsOfferedForOneClickSetup()
{
    runWithMockLink(
        [] { return MockLink::startAPMArduCopterMockLink(); },
        [this](QPointer<MockLink> /*mockLink*/, Vehicle* vehicle) {
            VehicleRoleController* const roles = _roles();
            QVERIFY(roles);
            QVERIFY(_openPage());

            const int sysid = static_cast<int>(vehicle->id());
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
    roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    roles->addEntry(212, QStringLiteral("Plane"), QStringLiteral("Sentinel"), 0);
    QVERIFY(_openPage());
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_211"), TestTimeout::mediumMs()));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_212"), TestTimeout::mediumMs()));

    QQuickItem* const linkName = _requireItem(QStringLiteral("vehicleNewLinkName_212"));
    QVERIFY(linkName);
    QVERIFY(linkName->setProperty("text", QStringLiteral("Microhard")));
    QVERIFY(clickButton(QStringLiteral("vehicleAddLink_212")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->linksForSysid(212)->count(), 1, TestTimeout::mediumMs());
    QCOMPARE(roles->linksForSysid(211)->count(), 0);
    QCOMPARE(linkName->property("text").toString(), QString());

    QVERIFY(clickButton(QStringLiteral("vehicleRemoveLink_212_0")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->linksForSysid(212)->count(), 0, TestTimeout::mediumMs());

    // Removing a vehicle takes its card with it and leaves the other alone
    QVERIFY(clickButton(QStringLiteral("vehicleRemove_212")));
    QTRY_VERIFY_WITH_TIMEOUT(roles->roleForSysid(212).isEmpty(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_212"), 0) == nullptr,
                             TestTimeout::mediumMs());
    QCOMPARE(roles->roleForSysid(211), QStringLiteral("Rover"));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_211"), TestTimeout::mediumMs()));
}
