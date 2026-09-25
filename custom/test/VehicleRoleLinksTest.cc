#include "VehicleRoleLinksTest.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include "AppSettings.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "VehicleRoleController.h"

void VehicleRoleLinksTest::init()
{
    CommsTest::init();
    QVERIFY(_directory.isValid());
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
}

void VehicleRoleLinksTest::cleanup()
{
    SettingsManager::instance()->appSettings()->savePath()->setRawValue(_savePath);
    CommsTest::cleanup();
}

void VehicleRoleLinksTest::_addRemoveAndPersistLinks()
{
    VehicleRoleController controller;
    controller.addEntry(210, QStringLiteral("Rover"), QStringLiteral("North"), 0);

    QVERIFY(controller.linksForSysid(210));
    QCOMPARE(controller.linksForSysid(210)->count(), 0);
    QVERIFY(!controller.linksForSysid(999));  // no entry for this sysid

    expectLogMessage("Custom.VehicleRoles", QtWarningMsg, QRegularExpression(QStringLiteral("Ignoring invalid link")));
    QCOMPARE(controller.addLink(0, QString(), QString()), -1);
    verifyExpectedLogMessage();
    QCOMPARE(controller.linksForSysid(210)->count(), 0);

    QCOMPARE(controller.addLink(0, QStringLiteral("RFD900x"), QString()), 0);
    QCOMPARE(controller.addLink(0, QStringLiteral("Microhard 2450"), QStringLiteral("Rover Microhard")), 1);

    QmlObjectListModel* links = controller.linksForSysid(210);
    QCOMPARE(links->count(), 2);
    auto* rfd900x = qobject_cast<VehicleRoleLinkEntry*>(links->get(0));
    QCOMPARE(rfd900x->label(), QStringLiteral("RFD900x"));
    QCOMPARE(rfd900x->linkConfigName(), QString());
    QCOMPARE(qobject_cast<VehicleRoleLinkEntry*>(links->get(1))->linkConfigName(), QStringLiteral("Rover Microhard"));

    controller.setLinkConfigName(0, 0, QStringLiteral("Rover RFD900x"));
    QCOMPARE(rfd900x->linkConfigName(), QStringLiteral("Rover RFD900x"));

    controller.removeLink(0, 1);
    QCOMPARE(links->count(), 1);
    QCOMPARE(qobject_cast<VehicleRoleLinkEntry*>(links->get(0))->label(), QStringLiteral("RFD900x"));

    // A fresh controller reading the same (redirected) settings path round-trips what was saved.
    VehicleRoleController reloaded;
    QmlObjectListModel* reloadedLinks = reloaded.linksForSysid(210);
    QVERIFY(reloadedLinks);
    QCOMPARE(reloadedLinks->count(), 1);
    auto* reloadedLink = qobject_cast<VehicleRoleLinkEntry*>(reloadedLinks->get(0));
    QCOMPARE(reloadedLink->label(), QStringLiteral("RFD900x"));
    QCOMPARE(reloadedLink->linkConfigName(), QStringLiteral("Rover RFD900x"));
}

void VehicleRoleLinksTest::_oldFormatFileLoadsWithoutLinks()
{
    // Simulates a settings file saved before the multi-link model existed: sysid/role/name/port
    // only, no "links" array at all.
    const QString filePath =
        SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json");
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QJsonObject obj;
    obj[QStringLiteral("sysid")] = 211;
    obj[QStringLiteral("role")] = QStringLiteral("Hex");
    obj[QStringLiteral("name")] = QStringLiteral("East");
    obj[QStringLiteral("port")] = 14550;
    file.write(QJsonDocument(QJsonArray{obj}).toJson());
    file.close();

    VehicleRoleController controller;
    QCOMPARE(controller.roleForSysid(211), QStringLiteral("Hex"));
    QCOMPARE(controller.nameForSysid(211), QStringLiteral("East"));
    QCOMPARE(controller.portForSysid(211), 14550);

    QmlObjectListModel* links = controller.linksForSysid(211);
    QVERIFY(links);
    QCOMPARE(links->count(), 0);
}

void VehicleRoleLinksTest::_isLinkConfigConnectedTracksLiveLink()
{
    VehicleRoleController controller;
    QVERIFY(!controller.isLinkConfigConnected(QString()));
    QVERIFY(!controller.isLinkConfigConnected(QStringLiteral("Nonexistent Link")));

    const SharedLinkInterfacePtr link = createMockLink(QStringLiteral("RFD900x Mock"));
    QVERIFY(link);
    QVERIFY(controller.isLinkConfigConnected(QStringLiteral("RFD900x Mock")));

    disconnectAllLinks();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.isLinkConfigConnected(QStringLiteral("RFD900x Mock")),
                             TestTimeout::mediumMs());
}

UT_REGISTER_TEST(VehicleRoleLinksTest, TestLabel::Unit, TestLabel::Comms)
