#include "VehicleRoleControllerTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "QGCCorePlugin.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "VehicleRoleController.h"
#include "VehicleSystemIdTestData.h"

void VehicleRoleControllerTest::init()
{
    UnitTest::init();
    QVERIFY(_directory.isValid());
    // Roles/links persist under savePath; start each test (and each stress iteration) from an empty directory
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
}

void VehicleRoleControllerTest::cleanup()
{
    SettingsManager::instance()->appSettings()->savePath()->setRawValue(_savePath);
    UnitTest::cleanup();
}

void VehicleRoleControllerTest::_systemIdRoundTrip_data()
{
    VehicleSystemIdTestData::addValidRows();
}

void VehicleRoleControllerTest::_systemIdRoundTrip()
{
    QFETCH(quint32, sysid);
    const quint32 otherSysid = sysid == 1 ? 257 : 1;
    {
        VehicleRoleController controller;
        controller.addEntry(sysid, QStringLiteral("Rover"), QStringLiteral("North"), 14550);
        controller.addEntry(otherSysid, QStringLiteral("Plane"), QStringLiteral("South"), 14551);
        QCOMPARE(controller.roleEntries()->count(), 2);
        QVERIFY(!controller.isNicknameTaken(QStringLiteral("North"), sysid));
        QVERIFY(controller.isNicknameTaken(QStringLiteral("North"), otherSysid));
        controller.addEntry(sysid, QStringLiteral("Copter"), QStringLiteral("North"), 14552);
        QCOMPARE(controller.roleEntries()->count(), 2);
    }

    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray saved = file.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(saved);
    QVERIFY(doc.isArray());
    QCOMPARE(doc.array().count(), 2);
    const QJsonValue savedId = doc.array().first().toObject().value(QStringLiteral("sysid"));
    QVERIFY(savedId.isDouble());
    QCOMPARE(savedId.toDouble(), double(sysid));
    QVERIFY(saved.contains(QByteArray("\"sysid\": ") + QByteArray::number(sysid)));

    VehicleRoleController reloaded;
    QCOMPARE(reloaded.roleEntries()->count(), 2);
    QCOMPARE(reloaded.roleEntries()->value<VehicleRoleEntry*>(0)->sysid(), sysid);
    QCOMPARE(reloaded.roleForSysid(sysid), QStringLiteral("Copter"));
    QCOMPARE(reloaded.nameForSysid(sysid), QStringLiteral("North"));
    QCOMPARE(reloaded.portForSysid(sysid), 14552);
    QCOMPARE(reloaded.roleForSysid(otherSysid), QStringLiteral("Plane"));
    QCOMPARE(reloaded.nameForSysid(otherSysid), QStringLiteral("South"));
    QCOMPARE(reloaded.portForSysid(otherSysid), 14551);

    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Ignoring invalid entry")));
    reloaded.addEntry(0, QStringLiteral("Rover"), QString(), 0);
    verifyExpectedLogMessage();
    QCOMPARE(reloaded.roleEntries()->count(), 2);
}

void VehicleRoleControllerTest::_invalidSavedSystemId_data()
{
    QTest::addColumn<QByteArray>("jsonId");
    QTest::newRow("zero") << QByteArray("0");
    QTest::newRow("negative") << QByteArray("-1");
    QTest::newRow("fraction") << QByteArray("1.5");
    QTest::newRow("overflow") << QByteArray("4294967296");
    QTest::newRow("fraction-above-max") << QByteArray("4294967295.5");
    QTest::newRow("text") << QByteArray("\"invalid\"");
    QTest::newRow("numeric-string") << QByteArray("\"255\"");
    QTest::newRow("null") << QByteArray("null");
    QTest::newRow("boolean") << QByteArray("true");
}

void VehicleRoleControllerTest::_invalidSavedSystemId()
{
    QFETCH(QByteArray, jsonId);
    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("[{\"sysid\": " + jsonId +
               ", \"role\": \"Rover\"},"
               "{\"sysid\": 257, \"role\": \"Plane\"}]");
    file.close();
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Skipping invalid saved entry - sysid:")));
    VehicleRoleController controller;
    verifyExpectedLogMessage();
    QCOMPARE(controller.roleEntries()->count(), 1);
    QCOMPARE(controller.roleEntries()->value<VehicleRoleEntry*>(0)->sysid(), quint32(257));
    QCOMPARE(controller.roleForSysid(257), QStringLiteral("Plane"));
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

void VehicleRoleControllerTest::_suggestedRoleFollowsVehicleType_data()
{
    QTest::addColumn<int>("mavType");
    QTest::addColumn<QString>("role");

    QTest::newRow("quadrotor") << int(MAV_TYPE_QUADROTOR) << QStringLiteral("Copter");
    QTest::newRow("hexarotor") << int(MAV_TYPE_HEXAROTOR) << QStringLiteral("Copter");
    QTest::newRow("helicopter") << int(MAV_TYPE_HELICOPTER) << QStringLiteral("Copter");
    QTest::newRow("fixed wing") << int(MAV_TYPE_FIXED_WING) << QStringLiteral("Plane");
    QTest::newRow("quadplane") << int(MAV_TYPE_VTOL_FIXEDROTOR) << QStringLiteral("Plane");
    QTest::newRow("tiltrotor") << int(MAV_TYPE_VTOL_TILTROTOR) << QStringLiteral("Plane");
    QTest::newRow("ground rover") << int(MAV_TYPE_GROUND_ROVER) << QStringLiteral("Rover");
    QTest::newRow("surface boat") << int(MAV_TYPE_SURFACE_BOAT) << QStringLiteral("Rover");
    QTest::newRow("submarine") << int(MAV_TYPE_SUBMARINE) << QStringLiteral("Sub");
    QTest::newRow("antenna tracker") << int(MAV_TYPE_ANTENNA_TRACKER) << QStringLiteral("Tracker");
    QTest::newRow("airship") << int(MAV_TYPE_AIRSHIP) << QStringLiteral("Blimp");
    // Types with no matching ArduPilot product leave the choice to the operator
    QTest::newRow("generic") << int(MAV_TYPE_GENERIC) << QString();
    QTest::newRow("gcs") << int(MAV_TYPE_GCS) << QString();
}

void VehicleRoleControllerTest::_suggestedRoleFollowsVehicleType()
{
    QFETCH(int, mavType);
    QFETCH(QString, role);

    QCOMPARE(VehicleRoleController::roleForMavType(mavType), role);
    if (!role.isEmpty()) {
        QVERIFY(VehicleRoleController().availableRoles().contains(role));
    }
    // No vehicle, nothing to suggest
    QVERIFY(VehicleRoleController().suggestedRole(nullptr).isEmpty());
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

void VehicleRoleControllerTest::_nicknamesMustBeDistinct()
{
    VehicleRoleController controller;
    controller.addEntry(1, QStringLiteral("Rover"), QStringLiteral("Mower-1"), 0);

    // Case and surrounding whitespace do not make a nickname distinct; a vehicle's own nickname and an empty one never
    // clash.
    QVERIFY(controller.isNicknameTaken(QStringLiteral(" mower-1 "), 2));
    QVERIFY(!controller.isNicknameTaken(QStringLiteral("Mower-1"), 1));
    QVERIFY(!controller.isNicknameTaken(QString(), 2));
    QVERIFY(!controller.isNicknameTaken(QStringLiteral("Scout"), 2));

    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Ignoring entry - nickname already used")));
    controller.addEntry(2, QStringLiteral("Copter"), QStringLiteral("MOWER-1"), 0);
    verifyExpectedLogMessage();
    QCOMPARE(controller.roleEntries()->count(), 1);

    controller.addEntry(2, QStringLiteral("Copter"), QStringLiteral("Scout"), 0);
    QCOMPARE(controller.roleEntries()->count(), 2);

    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Ignoring nickname already used")));
    controller.setName(1, QStringLiteral("mower-1"));
    verifyExpectedLogMessage();
    QCOMPARE(controller.nameForSysid(2), QStringLiteral("Scout"));

    // Saving a vehicle again under its own nickname is not a clash.
    controller.addEntry(1, QStringLiteral("Rover"), QStringLiteral("Mower-1"), 14550);
    QCOMPARE(controller.portForSysid(1), 14550);
}

void VehicleRoleControllerTest::_savedDuplicateNicknamesStillLoad()
{
    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json");
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // A save from before nicknames had to be distinct.
        file.write(R"([{"sysid": 5, "role": "Rover", "name": "Same", "port": 0},
                       {"sysid": 6, "role": "Copter", "name": "Same", "port": 0}])");
    }

    VehicleRoleController controller;
    QCOMPARE(controller.roleEntries()->count(), 2);
    QCOMPARE(controller.nameForSysid(5), QStringLiteral("Same"));
    QCOMPARE(controller.nameForSysid(6), QStringLiteral("Same"));
    QVERIFY(controller.isNicknameTaken(QStringLiteral("Same"), 5));

    // The operator can fix it by renaming either one.
    controller.setName(1, QStringLiteral("Other"));
    QCOMPARE(controller.nameForSysid(6), QStringLiteral("Other"));
}

void VehicleRoleControllerTest::_newerFormatIsReadOnly()
{
    const QString path =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json");
    const QByteArray original("{\"version\": 2}");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), qint64(original.size()));
    file.close();

    for (int i = 0; i < 2; ++i) {
        expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                         QRegularExpression(QStringLiteral("^Ignoring .* roles file:")));
        VehicleRoleController controller;
        verifyExpectedLogMessage();
        controller.addEntry(1, QStringLiteral("Rover"), QString(), 0);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll() == original);
        file.close();
        QVERIFY(!controller.property("persistenceError").toString().isEmpty());
    }
}

void VehicleRoleControllerTest::_roundTripKeepsMeaning()
{
    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(
        R"([{"sysid":7,"role":"Rover","name":"Alpha","port":14550,"links":[{"label":"Radio","linkConfigName":"Config","preferred":true}]}])");
    file.close();
    VehicleRoleController controller;
    controller.setName(0, QStringLiteral("Beta"));
    VehicleRoleController reloaded;
    QCOMPARE(reloaded.roleForSysid(7), QStringLiteral("Rover"));
    QCOMPARE(reloaded.nameForSysid(7), QStringLiteral("Beta"));
    QCOMPARE(reloaded.portForSysid(7), 14550);
    auto* link = reloaded.linksForSysid(7)->value<VehicleRoleLinkEntry*>(0);
    QCOMPARE(link->label(), QStringLiteral("Radio"));
    QCOMPARE(link->linkConfigName(), QStringLiteral("Config"));
    QVERIFY(link->preferred());
}

void VehicleRoleControllerTest::_unknownDataSurvivesEdit()
{
    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(
        R"([42,{"sysid":7,"role":"Rover","future":{"value":3},"links":[false,{"label":"Radio","futureLink":"kept"}]}])");
    file.close();
    VehicleRoleController controller;
    controller.setName(0, QStringLiteral("Beta"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    QCOMPARE(array.size(), 2);
    QCOMPARE(array.at(0).toInt(), 42);
    const QJsonObject role = array.at(1).toObject();
    QCOMPARE(role.value(QStringLiteral("future")).toObject().value(QStringLiteral("value")).toInt(), 3);
    const QJsonArray links = role.value(QStringLiteral("links")).toArray();
    QCOMPARE(links.size(), 2);
    QCOMPARE(links.at(0).toBool(), false);
    QCOMPARE(links.at(1).toObject().value(QStringLiteral("futureLink")).toString(), QStringLiteral("kept"));
}

void VehicleRoleControllerTest::_writeFailureKeepsOriginal_data()
{
    QTest::addColumn<QString>("failure");
    QTest::newRow("open") << QStringLiteral("open");
    QTest::newRow("short write") << QStringLiteral("short write");
    QTest::newRow("commit") << QStringLiteral("commit");
}

void VehicleRoleControllerTest::_writeFailureKeepsOriginal()
{
    QFETCH(QString, failure);
    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    const QByteArray original("[]");
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), qint64(original.size()));
    file.close();
    VehicleRoleController controller;
    controller._writeForTesting = [failure](const QString&, const QByteArray&, QString* error) {
        *error = failure;
        return false;
    };
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Failed to save roles file:")));
    controller.addEntry(7, QStringLiteral("Rover"), QStringLiteral("Alpha"), 0);
    verifyExpectedLogMessage();
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    file.close();
    QVERIFY(!controller.persistenceError().isEmpty());
    QVERIFY(controller._dirty);
    controller._writeForTesting = {};
    controller.setName(0, QStringLiteral("Beta"));
    QVERIFY(!controller._dirty);
    QVERIFY(controller.persistenceError().isEmpty());
    VehicleRoleController reloaded;
    QCOMPARE(reloaded.nameForSysid(7), QStringLiteral("Beta"));
}

void VehicleRoleControllerTest::_realOpenFailureKeepsStateUnsaved()
{
    const QString settingsPath = SettingsManager::instance()->appSettings()->settingsSavePath();
    const QString path = settingsPath + QStringLiteral("/VehicleRoles.json");
    const QByteArray original(R"([{"sysid":7,"role":"Rover","name":"Alpha"}])");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), qint64(original.size()));
    file.close();

    VehicleRoleController controller;
    QCOMPARE(controller.nameForSysid(7), QStringLiteral("Alpha"));
    QDir settingsDir(settingsPath);
    QVERIFY(settingsDir.removeRecursively());
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Failed to save roles file:")));
    controller.setName(0, QStringLiteral("Beta"));
    verifyExpectedLogMessage();
    QVERIFY(!controller.persistenceError().isEmpty());
    QVERIFY(controller._dirty);
    QCOMPARE(controller.nameForSysid(7), QStringLiteral("Beta"));
    QVERIFY(!file.exists());

    QVERIFY(QDir().mkpath(settingsPath));
    controller.setName(0, QStringLiteral("Gamma"));
    QVERIFY(controller.persistenceError().isEmpty());
    QVERIFY(!controller._dirty);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonArray saved = QJsonDocument::fromJson(file.readAll()).array();
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Gamma"));
}

void VehicleRoleControllerTest::_malformedFileIsQuarantined()
{
    const QString path =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json");
    const QByteArray bad("not json");
    const QDir settingsDir(SettingsManager::instance()->appSettings()->settingsSavePath());
    QStringList quarantined;
    QString previousPath;
    for (int i = 0; i < 2; ++i) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bad), qint64(bad.size()));
        file.close();
        QSignalSpy noticeSpy(QGCCorePlugin::instance(), &QGCCorePlugin::operatorNotification);
        VehicleRoleController controller;
        QCOMPARE(noticeSpy.count(), 1);
        QCOMPARE(noticeSpy.at(0).at(0).toString(), controller.persistenceNotice());
        QCOMPARE(controller.roleEntries()->count(), 0);
        quarantined = settingsDir.entryList({QStringLiteral("VehicleRoles.json.invalid-*")}, QDir::Files);
        QCOMPARE(quarantined.size(), i + 1);
        for (const QString& name : quarantined) {
            QFile saved(settingsDir.filePath(name));
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(saved.readAll(), bad);
        }
        QString currentPath;
        for (const QString& name : quarantined) {
            const QString candidate = settingsDir.filePath(name);
            if (controller.persistenceNotice().contains(candidate)) {
                currentPath = candidate;
            }
        }
        QVERIFY(!currentPath.isEmpty());
        QVERIFY(currentPath != previousPath);
        previousPath = currentPath;
        controller.addEntry(7, QStringLiteral("Rover"), QString(), 0);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(file.readAll()).isArray());
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bad), qint64(bad.size()));
    file.close();
    QVERIFY(QFile::remove(path));
    VehicleRoleController controller;
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bad), qint64(bad.size()));
    file.close();
    controller._renameForTesting = [](const QString&, const QString&) { return false; };
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Failed to quarantine roles file:")));
    controller._load();
    verifyExpectedLogMessage();
    controller.addEntry(7, QStringLiteral("Rover"), QString(), 0);
    QVERIFY(!controller.persistenceError().isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), bad);
}

void VehicleRoleControllerTest::_unreadableFileBlocksWrites()
{
    const QString path =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json");
    VehicleRoleController controller;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("[]"), qint64(2));
    file.close();
    controller._readForTesting = [](const QString&, QByteArray*, QString* error) {
        *error = QStringLiteral("denied");
        return false;
    };
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^Failed to read roles file:")));
    controller._load();
    verifyExpectedLogMessage();
    controller.addEntry(7, QStringLiteral("Rover"), QString(), 0);
    QVERIFY(!controller.persistenceError().isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("[]"));
}

UT_REGISTER_TEST(VehicleRoleControllerTest, TestLabel::Unit)
