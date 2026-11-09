#include "VehicleRoleLinksTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>

#include "AppSettings.h"
#include "MockLink.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "VehicleRoleController.h"
#include "VehicleSystemIdTestData.h"

void VehicleRoleLinksTest::init()
{
    CommsTest::init();
    QSettings settingsStore;
    _failbackHold = settingsStore.value(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    settingsStore.remove(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    QVERIFY(_directory.isValid());
    // Roles/links persist under savePath; start each test (and each stress iteration) from an empty directory
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    auto* settings = SettingsManager::instance()->appSettings();
    _savePath = settings->savePath()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
}

void VehicleRoleLinksTest::cleanup()
{
    SettingsManager::instance()->appSettings()->savePath()->setRawValue(_savePath);
    QSettings settingsStore;
    if (_failbackHold.isValid()) {
        settingsStore.setValue(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"), _failbackHold);
    } else {
        settingsStore.remove(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"));
    }
    CommsTest::cleanup();
}

void VehicleRoleLinksTest::_addRemoveAndPersistLinks_data()
{
    VehicleSystemIdTestData::addValidRows();
}

void VehicleRoleLinksTest::_addRemoveAndPersistLinks()
{
    QFETCH(quint32, sysid);
    VehicleRoleController controller;
    controller.addEntry(sysid, QStringLiteral("Rover"), QStringLiteral("North"), 0);

    QVERIFY(controller.linksForSysid(sysid));
    QCOMPARE(controller.linksForSysid(sysid)->count(), 0);
    QVERIFY(!controller.linksForSysid(999));  // no entry for this sysid

    expectLogMessage("Custom.VehicleRoles", QtWarningMsg, QRegularExpression(QStringLiteral("Ignoring invalid link")));
    QCOMPARE(controller.addLink(0, QString(), QString()), -1);
    verifyExpectedLogMessage();
    QCOMPARE(controller.linksForSysid(sysid)->count(), 0);

    QCOMPARE(controller.addLink(0, QStringLiteral("RFD900x"), QString()), 0);
    QCOMPARE(controller.addLink(0, QStringLiteral("Microhard 2450"), QStringLiteral("Rover Microhard")), 1);

    QmlObjectListModel* links = controller.linksForSysid(sysid);
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
    QmlObjectListModel* reloadedLinks = reloaded.linksForSysid(sysid);
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
    obj[QStringLiteral("role")] = QStringLiteral("Copter");
    obj[QStringLiteral("name")] = QStringLiteral("East");
    obj[QStringLiteral("port")] = 14550;
    file.write(QJsonDocument(QJsonArray{obj}).toJson());
    file.close();

    VehicleRoleController controller;
    QCOMPARE(controller.roleForSysid(211), QStringLiteral("Copter"));
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

void VehicleRoleLinksTest::_preferredLinksPersistAndStayUnique_data()
{
    QTest::addColumn<quint32>("sysid");
    QTest::newRow("one") << quint32(1);
    QTest::newRow("uint32-max") << quint32(4294967295U);
}

void VehicleRoleLinksTest::_preferredLinksPersistAndStayUnique()
{
    QFETCH(quint32, sysid);
    const quint32 otherSysid = sysid == 1 ? 4294967295U : 1;
    VehicleRoleController controller;
    controller.addEntry(sysid, QStringLiteral("Rover"), QStringLiteral("North"), 0);
    controller.addEntry(otherSysid, QStringLiteral("Plane"), QStringLiteral("East"), 0);
    controller.addLink(0, QStringLiteral("RFD"), QStringLiteral("Rover RFD"));
    controller.addLink(0, QStringLiteral("UDP"), QStringLiteral("Rover UDP"));
    controller.addLink(1, QStringLiteral("UDP"), QStringLiteral("Plane UDP"));
    auto* first = qobject_cast<VehicleRoleLinkEntry*>(controller.linksForSysid(sysid)->get(0));
    auto* second = qobject_cast<VehicleRoleLinkEntry*>(controller.linksForSysid(sysid)->get(1));
    QVERIFY(first && second);
    QVERIFY(!first->preferred() && !second->preferred());
    controller.setLinkPreferred(0, 0, true);
    controller.setLinkPreferred(0, 1, true);
    controller.setLinkPreferred(1, 0, true);
    QVERIFY(!first->preferred() && second->preferred());
    QCOMPARE(controller.preferredLinkConfigNameForSysid(sysid), QStringLiteral("Rover UDP"));
    QCOMPARE(controller.preferredLinkConfigNameForSysid(otherSysid), QStringLiteral("Plane UDP"));
    QVERIFY(controller.preferredLinkConfigNameForSysid(257).isEmpty());
    controller.setFailbackHoldSeconds(42);
    QCOMPARE(QSettings().value(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds")).toInt(), 42);
    QFile savedFile(SettingsManager::instance()->appSettings()->settingsSavePath() +
                    QStringLiteral("/VehicleRoles.json"));
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    const auto saved = QJsonDocument::fromJson(savedFile.readAll());
    QVERIFY(saved.isArray());
    QCOMPARE(saved.array().size(), 2);
    QCOMPARE(saved.array().at(0).toObject().value(QStringLiteral("sysid")).toDouble(), double(sysid));
    QCOMPARE(saved.array().at(1).toObject().value(QStringLiteral("sysid")).toDouble(), double(otherSysid));
    QVERIFY(!saved.array().at(0).toObject().contains(QStringLiteral("failbackHoldSeconds")));
    const auto savedLinks = saved.array().at(0).toObject().value(QStringLiteral("links")).toArray();
    QVERIFY(!savedLinks.at(0).toObject().contains(QStringLiteral("preferred")));
    QVERIFY(savedLinks.at(1).toObject().value(QStringLiteral("preferred")).toBool());
    savedFile.close();
    {
        VehicleRoleController reloaded;
        QCOMPARE(reloaded.failbackHoldSeconds(), 42);
        QCOMPARE(reloaded.preferredLinkConfigNameForSysid(sysid), QStringLiteral("Rover UDP"));
        QVERIFY(qobject_cast<VehicleRoleLinkEntry*>(reloaded.linksForSysid(sysid)->get(1))->preferred());
        QVERIFY(!qobject_cast<VehicleRoleLinkEntry*>(reloaded.linksForSysid(sysid)->get(0))->preferred());
    }
    controller.setLinkConfigName(0, 1, QStringLiteral("Different UDP"));
    QVERIFY(!second->preferred());
    QVERIFY(controller.preferredLinkConfigNameForSysid(sysid).isEmpty());
    controller.setLinkPreferred(0, 1, true);
    controller.removeLink(0, 1);
    QVERIFY(controller.preferredLinkConfigNameForSysid(sysid).isEmpty());
    controller.setLinkConfigName(0, 0, QString());
    controller.setLinkPreferred(0, 0, true);
    QVERIFY(!first->preferred());
    controller.setFailbackHoldSeconds(1);
    QCOMPARE(controller.failbackHoldSeconds(), 5);
    controller.setFailbackHoldSeconds(1000);
    QCOMPARE(controller.failbackHoldSeconds(), 300);
    VehicleRoleController reloaded;
    QCOMPARE(reloaded.failbackHoldSeconds(), 300);
    QVERIFY(reloaded.preferredLinkConfigNameForSysid(sysid).isEmpty());
    QCOMPARE(reloaded.preferredLinkConfigNameForSysid(otherSysid), QStringLiteral("Plane UDP"));
}

void VehicleRoleLinksTest::_loadPreferences_data()
{
    QTest::addColumn<int>("savedHold");
    QTest::addColumn<int>("expectedHold");
    QTest::newRow("legacy-links-without-preferred") << 0 << 15;
    QTest::newRow("clamp-low") << 1 << 5;
    QTest::newRow("clamp-high") << 900 << 300;
}

void VehicleRoleLinksTest::_loadPreferences()
{
    QFETCH(int, savedHold);
    QFETCH(int, expectedHold);
    const QJsonObject link{{QStringLiteral("label"), QStringLiteral("UDP")},
                           {QStringLiteral("linkConfigName"), QStringLiteral("Rover UDP")}};
    const QJsonArray entries{QJsonObject{{QStringLiteral("sysid"), 210},
                                         {QStringLiteral("role"), QStringLiteral("Rover")},
                                         {QStringLiteral("links"), QJsonArray{link}}}};
    if (savedHold != 0) {
        QSettings().setValue(QStringLiteral("Custom/LinkPreference/FailbackHoldSeconds"), savedHold);
    }
    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QJsonDocument json(entries);
    QVERIFY(file.write(json.toJson()) > 0);
    file.close();
    VehicleRoleController controller;
    QCOMPARE(controller.failbackHoldSeconds(), expectedHold);
    QVERIFY(controller.linksForSysid(210));
    QCOMPARE(controller.linksForSysid(210)->count(), 1);
    QVERIFY(!qobject_cast<VehicleRoleLinkEntry*>(controller.linksForSysid(210)->get(0))->preferred());
    QVERIFY(controller.preferredLinkConfigNameForSysid(210).isEmpty());
}

void VehicleRoleLinksTest::_duplicateSavedPreferences_data()
{
    QTest::addColumn<quint32>("sysid");
    QTest::newRow("one") << quint32(1);
    QTest::newRow("uint32-max") << quint32(4294967295U);
}

void VehicleRoleLinksTest::_duplicateSavedPreferences()
{
    QFETCH(quint32, sysid);
    const auto link = [](const QString& name) {
        return QJsonObject{{QStringLiteral("label"), name},
                           {QStringLiteral("linkConfigName"), name},
                           {QStringLiteral("preferred"), true}};
    };
    const QJsonArray entries{
        QJsonObject{{QStringLiteral("sysid"), double(sysid)},
                    {QStringLiteral("role"), QStringLiteral("Rover")},
                    {QStringLiteral("links"),
                     QJsonArray{link(QString()), link(QStringLiteral("RFD")), link(QStringLiteral("UDP"))}}}};
    QFile file(SettingsManager::instance()->appSettings()->settingsSavePath() + QStringLiteral("/VehicleRoles.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(entries).toJson()) > 0);
    file.close();
    // An empty label is skipped; among valid associations, the first preferred wins.
    expectLogMessage("Custom.VehicleRoles", QtWarningMsg,
                     QRegularExpression(QStringLiteral("Skipping saved link with empty label")));
    VehicleRoleController controller;
    verifyExpectedLogMessage();
    auto* links = controller.linksForSysid(sysid);
    QVERIFY(links);
    QCOMPARE(links->count(), 2);
    QCOMPARE(controller.preferredLinkConfigNameForSysid(sysid), QStringLiteral("RFD"));
    QVERIFY(qobject_cast<VehicleRoleLinkEntry*>(links->get(0))->preferred());
    QVERIFY(!qobject_cast<VehicleRoleLinkEntry*>(links->get(1))->preferred());
    controller.setLinkPreferred(0, 1, true);
    VehicleRoleController reloaded;
    QCOMPARE(reloaded.preferredLinkConfigNameForSysid(sysid), QStringLiteral("UDP"));
}

UT_REGISTER_TEST(VehicleRoleLinksTest, TestLabel::Unit, TestLabel::Comms)
