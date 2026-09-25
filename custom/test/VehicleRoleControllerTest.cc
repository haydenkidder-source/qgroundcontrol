#include "VehicleRoleControllerTest.h"

#include <QtCore/QFile>
#include <QtCore/QRegularExpression>

#include "AppSettings.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "VehicleRoleController.h"

void VehicleRoleControllerTest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
}

void VehicleRoleControllerTest::cleanup()
{
    SettingsManager::instance()->appSettings()->savePath()->setRawValue(_savePath);
    UnitTest::cleanup();
}

void VehicleRoleControllerTest::_availableRolesAreArduPilotVehicleTypes()
{
    VehicleRoleController controller;
    const QStringList roles = controller.availableRoles();
    // The exact list ArduPilot ships as distinct vehicle firmwares/products - not MAVLink's finer
    // per-frame MAV_TYPE breakdown (e.g. Quadrotor/Hexarotor both fall under "Copter" here).
    QCOMPARE(roles, QStringList({QStringLiteral("Copter"), QStringLiteral("Plane"), QStringLiteral("Rover"),
                                 QStringLiteral("Sub"), QStringLiteral("Tracker"), QStringLiteral("Blimp")}));
}

void VehicleRoleControllerTest::_addEntryRejectsUnrecognizedRole()
{
    VehicleRoleController controller;
    // "Stallion"/"Hex" were the old fixed call-sign roles this program used before roles became
    // ArduPilot vehicle types - neither is a valid type, so both must still be rejected.
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Ignoring invalid entry")));
    controller.addEntry(1, QStringLiteral("Stallion"), QStringLiteral("nickname"), 0);
    verifyExpectedLogMessage();
    QCOMPARE(controller.roleEntries()->count(), 0);
    QCOMPARE(controller.roleForSysid(1), QString());

    controller.addEntry(1, QStringLiteral("Plane"), QStringLiteral("nickname"), 0);
    QCOMPARE(controller.roleEntries()->count(), 1);
    QCOMPARE(controller.roleForSysid(1), QStringLiteral("Plane"));
}

void VehicleRoleControllerTest::_loadMigratesUnrecognizedSavedRoleToName()
{
    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json");
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // A save from before roles became vehicle types: role holds the old call-sign, with no
        // separate nickname set.
        file.write(R"([{"sysid": 5, "role": "Stallion", "name": "", "port": 14550}])");
    }

    VehicleRoleController controller;
    QCOMPARE(controller.roleEntries()->count(), 1);
    // The entry survives - sysid and port preserved - but with an empty (unassigned) role, since
    // "Stallion" is no longer a valid vehicle type and there's no way to infer one from a name.
    // The old role value becomes the nickname instead of being silently discarded.
    QCOMPARE(controller.roleForSysid(5), QString());
    QCOMPARE(controller.nameForSysid(5), QStringLiteral("Stallion"));
    QCOMPARE(controller.portForSysid(5), 14550);
}

UT_REGISTER_TEST(VehicleRoleControllerTest, TestLabel::Unit)
