#include "VehicleLinksPageUITest.h"

#include <memory>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/qscopeguard.h>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include "AppSettings.h"
#include "COPController.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "LinkPreferenceController.h"
#include "MissionManager.h"
#include "MockLink.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "QGCPalette.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "UDPLink.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VehicleRoleController.h"
#include "VehicleSystemIdTestData.h"

class VehicleLinksTestTransport : public LinkInterface
{
public:
    explicit VehicleLinksTestTransport(SharedLinkConfigurationPtr& config)
        : LinkInterface(config)
    {}

    bool isConnected() const override { return _connected; }

    bool disconnectCalled() const { return _disconnectCalled; }

    void setConnected(bool connected)
    {
        _connected = connected;
        if (connected) {
            emit this->connected();
        } else {
            emit disconnected();
        }
    }

    void disconnect() override
    {
        _disconnectCalled = true;
        setConnected(false);
    }

private:
    bool _connect() override { return true; }

    void _writeBytes(const QByteArray&) override {}

    bool _connected = false;
    bool _disconnectCalled = false;
};

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
    return clickToolSelectDropdownButton(QStringLiteral("toolbar_viewSettings")) &&
           clickButtonScrolled(QStringLiteral("settingsButton_Vehicle Links"), QStringLiteral("settings_buttonList")) &&
           findVisibleItem(_rootItem, QStringLiteral("vehicleLinksFlickable"), TestTimeout::mediumMs()) != nullptr;
}

void VehicleLinksPageUITest::_settingsPlacementAndAnalyzeRemoval()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(_openPage());

    QQuickItem* const commLinksButton = findVisibleItem(_rootItem, QStringLiteral("settingsButton_Comm Links"));
    QQuickItem* const vehicleLinksButton = findVisibleItem(_rootItem, QStringLiteral("settingsButton_Vehicle Links"));
    QVERIFY(commLinksButton && vehicleLinksButton);
    QQuickItem* const commLinksColumn = commLinksButton->parentItem();
    QQuickItem* const vehicleLinksColumn = vehicleLinksButton->parentItem();
    QVERIFY(commLinksColumn && vehicleLinksColumn);
    QCOMPARE(commLinksColumn->parentItem(), vehicleLinksColumn->parentItem());
    QVERIFY(commLinksColumn->property("index").isValid());
    QVERIFY(vehicleLinksColumn->property("index").isValid());
    QCOMPARE(vehicleLinksColumn->property("index").toInt(), commLinksColumn->property("index").toInt() + 1);
    QTRY_VERIFY_WITH_TIMEOUT(vehicleLinksButton->mapToScene(QPointF()).y() > commLinksButton->mapToScene(QPointF()).y(),
                             TestTimeout::shortMs());

    QQuickItem* const rightPanel = findVisibleItem(_rootItem, QStringLiteral("settings_rightPanel"));
    QQuickItem* const flickable = findVisibleItem(_rootItem, QStringLiteral("vehicleLinksFlickable"));
    QVERIFY(rightPanel && flickable);
    QTRY_VERIFY_WITH_TIMEOUT(flickable->width() > 0 && flickable->height() > 0, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(rightPanel->mapRectToScene(rightPanel->boundingRect())
                                 .contains(flickable->mapRectToScene(flickable->boundingRect())),
                             TestTimeout::shortMs());

    QVERIFY(clickButtonScrolled(QStringLiteral("settingsButton_Comm Links"), QStringLiteral("settings_buttonList")));
    QQuickItem* const commLinksPage = findVisibleItem(_rootItem, QStringLiteral("settingsPage_CommLinks"));
    QVERIFY(commLinksPage);
    QVERIFY(findVisibleItem(commLinksPage, QStringLiteral("settingsGroup_AutoConnect")));
    QVERIFY(findVisibleItem(commLinksPage, QStringLiteral("settingsCheckBox_autoConnectUDP")));
    QVERIFY(clickButtonScrolled(QStringLiteral("settingsButton_Vehicle Links"), QStringLiteral("settings_buttonList")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleLinksFlickable")));

    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewAnalyze")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("analyzeButton_App Log Viewer")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("analyzeButton_Exclusion Zones")));
    QVERIFY(!findItem(_rootItem, QStringLiteral("analyzeButton_Vehicle Links")));
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
    QVERIFY(clickButtonScrolled(QStringLiteral("newVehicleAdd"), QStringLiteral("vehicleLinksFlickable")));

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
            QVERIFY(
                clickButtonScrolled(QStringLiteral("setUpAdd_%1").arg(sysid), QStringLiteral("vehicleLinksFlickable")));

            QTRY_COMPARE_WITH_TIMEOUT(roles->roleForSysid(sysid), QStringLiteral("Copter"), TestTimeout::mediumMs());
            QCOMPARE(roles->nameForSysid(sysid), QStringLiteral("Mapper"));
            QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleCard_%1").arg(sysid), TestTimeout::mediumMs()));
            roles->addLink(0, QStringLiteral("Radio"), QStringLiteral("Vehicle Links setup"));
            QCOMPARE(vehicle->vehicleLinkManager()->linkNames().size(), 1);
            QQuickItem* const returnStatus = _requireItem(QStringLiteral("vehicleReturnStatus_%1").arg(sysid));
            QVERIFY(returnStatus);
            QTRY_COMPARE_WITH_TIMEOUT(
                returnStatus->property("text").toString(),
                QStringLiteral("Automatic return is off. Tick Preferred on one link to turn it on."),
                TestTimeout::shortMs());
            QVERIFY(!returnStatus->isVisible());
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
    QVERIFY(clickButtonScrolled(QStringLiteral("newVehicleAdd"), QStringLiteral("vehicleLinksFlickable")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->nameForSysid(212), QStringLiteral("Sentinel"), TestTimeout::mediumMs());
    QCOMPARE(roles->nameForSysid(211), QStringLiteral("Scout"));
}

void VehicleLinksPageUITest::_existingSystemIdBlocksAdd()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    QVERIFY(_openPage());
    QQuickItem* const sysid = _requireItem(QStringLiteral("newVehicleSysid"));
    QQuickItem* const add = _requireItem(QStringLiteral("newVehicleAdd"));
    QVERIFY(sysid && add);
    QVERIFY(sysid->setProperty("text", QStringLiteral("212")));
    QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), TestTimeout::shortMs());
    QVERIFY(sysid->setProperty("text", QStringLiteral("211")));
    QTRY_VERIFY_WITH_TIMEOUT(!add->isEnabled(), TestTimeout::shortMs());
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("newVehicleSysidTaken"), TestTimeout::shortMs()));
}

void VehicleLinksPageUITest::_failedAddKeepsFields()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* const roles = _roles();
    QVERIFY(roles);
    QVERIFY(_openPage());
    QQuickItem* const sysid = _requireItem(QStringLiteral("newVehicleSysid"));
    QQuickItem* const name = _requireItem(QStringLiteral("newVehicleNickname"));
    QQuickItem* const add = _requireItem(QStringLiteral("newVehicleAdd"));
    QVERIFY(sysid && name && add);
    QVERIFY(sysid->setProperty("text", QStringLiteral("211")));
    QVERIFY(name->setProperty("text", QStringLiteral("Draft")));
    QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), TestTimeout::shortMs());
    roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Existing"), 0);
    QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
    QCOMPARE(sysid->property("text").toString(), QStringLiteral("211"));
    QCOMPARE(name->property("text").toString(), QStringLiteral("Draft"));
    QVERIFY(sysid->setProperty("text", QStringLiteral("212")));
    QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), TestTimeout::shortMs());
    QVERIFY(clickButtonScrolled(QStringLiteral("newVehicleAdd"), QStringLiteral("vehicleLinksFlickable")));
    QCOMPARE(sysid->property("text").toString(), QString());
    QCOMPARE(name->property("text").toString(), QString());
}

void VehicleLinksPageUITest::_duplicateConfigurationDisablesLinkAction()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    auto* manager = LinkManager::instance();
    const auto first = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Duplicate UI link")));
    SharedLinkConfigurationPtr second;
    const auto cleanup = qScopeGuard([&] {
        manager->removeConfiguration(first.get());
        if (second) {
            manager->removeConfiguration(second.get());
        }
    });
    roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    roles->addLink(0, QStringLiteral("Radio"), QStringLiteral("Duplicate UI link"));
    QVERIFY(_openPage());
    QQuickItem* const status = _requireItem(QStringLiteral("vehicleLinkStatus_211_0"));
    QQuickItem* const action = _requireItem(QStringLiteral("vehicleLinkAction_211_0"));
    QVERIFY(status && action);
    QCOMPARE(status->property("text").toString(), QStringLiteral("Disconnected"));
    QVERIFY(action->isEnabled());
    second = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Duplicate UI link")));
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Ambiguous name"),
                              TestTimeout::shortMs());
    QVERIFY(!action->isEnabled());
    QQuickItem* const page = findItem(_rootItem, QStringLiteral("vehicleLinksPage"));
    QVERIFY(page);
    QVariant result;
    QVERIFY(QMetaObject::invokeMethod(page, "_configForName", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QStringLiteral("Duplicate UI link"))));
    QVERIFY(result.isNull());
    manager->removeConfiguration(second.get());
    second.reset();
    QVERIFY(QMetaObject::invokeMethod(page, "_configForName", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QStringLiteral("Duplicate UI link"))));
    QCOMPARE(result.value<QObject*>(), first.get());
    QVERIFY(QMetaObject::invokeMethod(page, "_configForName", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QStringLiteral("Missing UI link"))));
    QVERIFY(result.isNull());
}

void VehicleLinksPageUITest::_stockLinkEditor()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* const manager = LinkManager::instance();
    const auto existing = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Editor existing")));
    const auto cleanup = qScopeGuard([&] { manager->removeConfiguration(existing.get()); });
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewSettings")));
    QVERIFY(clickButtonScrolled(QStringLiteral("settingsButton_Comm Links"), QStringLiteral("settings_buttonList")));
    QQuickItem* const add = findVisibleItem(_rootItem, QStringLiteral("stockAddLink"));
    QVERIFY(add);
    QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
    QVERIFY(waitForDialog(QStringLiteral("Add New Link")));
    QQuickItem* const name = findVisibleItem(_rootItem, QStringLiteral("stockLinkName"));
    QQuickItem* const save = findVisibleItem(_rootItem, QStringLiteral("popupDialog_acceptButton"));
    QQuickItem* const type = findVisibleItem(_rootItem, QStringLiteral("stockLinkType"));
    QQuickItem* const loader = findVisibleItem(_rootItem, QStringLiteral("stockLinkSettingsLoader"));
    QVERIFY(name && save && type && loader);
    QVERIFY(name->setProperty("text", QStringLiteral("Editor existing")));
    QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("stockLinkNameTaken"), 0),
                             TestTimeout::longMs());
    QVERIFY(!save->isEnabled());
    QVERIFY(name->setProperty("text", QStringLiteral("Editor unique")));
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), TestTimeout::longMs());
    QVERIFY(name->setProperty("text", QStringLiteral("Editor existing")));
    QObject* const dialog = name->property("dialog").value<QObject*>();
    QVERIFY(dialog);
    QPointer<LinkConfiguration> previous = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(previous);
    QSignalSpy destroyed(previous, &QObject::destroyed);
    const int nextType =
        previous->type() == LinkConfiguration::TypeUdp ? LinkConfiguration::TypeSerial : LinkConfiguration::TypeUdp;
    QVERIFY(QMetaObject::invokeMethod(type, "activated", Q_ARG(int, nextType)));
    QTRY_VERIFY_WITH_TIMEOUT(previous.isNull(), TestTimeout::shortMs());
    QCOMPARE(destroyed.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(loader->property("item").value<QObject*>(), TestTimeout::longMs());
    QVERIFY(name->setProperty("text", QStringLiteral("Editor unique")));
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), TestTimeout::longMs());
    QVERIFY(loader->setProperty("active", false));
    QVERIFY(!loader->property("item").value<QObject*>());
    QVERIFY(!save->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "_accept"));
    QVERIFY(dialogVisible(QStringLiteral("Add New Link")));
    QVERIFY(loader->setProperty("active", true));
    QTRY_VERIFY_WITH_TIMEOUT(loader->property("item").value<QObject*>(), TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), TestTimeout::shortMs());
    QVERIFY(name->setProperty("text", QStringLiteral("Editor existing")));
    QVERIFY(!save->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "_accept"));
    QVERIFY(dialogVisible(QStringLiteral("Add New Link")));
    QPointer<LinkConfiguration> current = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(current);
    QSignalSpy currentDestroyed(current, &QObject::destroyed);
    QVERIFY(rejectDialog());
    QTRY_VERIFY_WITH_TIMEOUT(current.isNull(), TestTimeout::shortMs());
    QCOMPARE(currentDestroyed.count(), 1);

    // Locate the existing configuration's edit icon through its row, which has no object name.
    const auto findText = [&](QQuickItem* root, const auto& self) -> QQuickItem* {
        if (root->isVisible() && root->property("text").toString() == QStringLiteral("Editor existing")) {
            return root;
        }
        for (QQuickItem* child : root->childItems()) {
            if (auto* found = self(child, self)) {
                return found;
            }
        }
        return nullptr;
    };
    QQuickItem* const existingLabel = findText(_rootItem, findText);
    QVERIFY(existingLabel && existingLabel->parentItem());
    const auto rowItems = existingLabel->parentItem()->childItems();
    const int labelIndex = rowItems.indexOf(existingLabel);
    QVERIFY(labelIndex >= 0 && labelIndex + 1 < rowItems.size());
    QVERIFY(_clickItemAt(rowItems.at(labelIndex + 1), 0.5, 0.5, QStringLiteral("Edit existing link")));
    QVERIFY(waitForDialog(QStringLiteral("Edit Link")));
    QQuickItem* const editName = findVisibleItem(_rootItem, QStringLiteral("stockLinkName"));
    QVERIFY(editName);
    QObject* const editDialog = editName->property("dialog").value<QObject*>();
    QVERIFY(editDialog);
    const auto conflicting = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Edit conflict")));
    const auto cleanupConflict = qScopeGuard([&] { manager->removeConfiguration(conflicting.get()); });
    QVERIFY(editName->setProperty("text", QStringLiteral("Edit conflict")));
    QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("stockLinkNameTaken"), 0),
                             TestTimeout::shortMs());
    QVERIFY(QMetaObject::invokeMethod(editDialog, "_accept"));
    QVERIFY(dialogVisible(QStringLiteral("Edit Link")));
    QVERIFY(qvariant_cast<LinkConfiguration*>(editDialog->property("editingConfig")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("stockLinkNameTaken"), 0));
    QCOMPARE(existing->name(), QStringLiteral("Editor existing"));
    // The stock loader is asynchronous; accepted() returns early until its item exists.
    QQuickItem* const editLoader = findVisibleItem(_rootItem, QStringLiteral("stockLinkSettingsLoader"));
    QVERIFY(editLoader);
    QTRY_VERIFY_WITH_TIMEOUT(editLoader->property("item").value<QObject*>() != nullptr, TestTimeout::longMs());
    // _accept() is blocked while the name is taken, so emit accepted() to reach the manager's own refusal.
    QVERIFY(QMetaObject::invokeMethod(editDialog, "accepted"));
    QVERIFY(editDialog->property("preventClose").toBool());
    QVERIFY(qvariant_cast<LinkConfiguration*>(editDialog->property("editingConfig")));
    QCOMPARE(existing->name(), QStringLiteral("Editor existing"));
    // _accept() would have cleared the flag; left set, the next _reject() would not close the dialog.
    QVERIFY(editDialog->setProperty("preventClose", false));
    QVERIFY(rejectDialog());

    QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
    QVERIFY(waitForDialog(QStringLiteral("Add New Link")));
    QQuickItem* const newName = findVisibleItem(_rootItem, QStringLiteral("stockLinkName"));
    QQuickItem* const newSave = findVisibleItem(_rootItem, QStringLiteral("popupDialog_acceptButton"));
    QVERIFY(newName && newSave);
    QVERIFY(newName->setProperty("text", QStringLiteral("Stock editor saved")));
    QTRY_VERIFY_WITH_TIMEOUT(newSave->isEnabled(), TestTimeout::longMs());
    QObject* const newDialog = newName->property("dialog").value<QObject*>();
    QVERIFY(newDialog);
    QPointer<LinkConfiguration> saved = qvariant_cast<LinkConfiguration*>(newDialog->property("editingConfig"));
    QVERIFY(saved);
    const auto cleanupSaved = qScopeGuard([&] {
        if (saved)
            manager->removeConfiguration(saved.data());
    });
    auto* const configurations = manager->property("linkConfigurations").value<QmlObjectListModel*>();
    QVERIFY(configurations);
    const int count = configurations->count();
    QVERIFY(acceptDialog());
    QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(QStringLiteral("Add New Link")), TestTimeout::shortMs());
    QTRY_COMPARE_WITH_TIMEOUT(configurations->count(), count + 1, TestTimeout::shortMs());
    QVERIFY(saved);
    QCOMPARE(saved->name(), QStringLiteral("Stock editor saved"));
}

void VehicleLinksPageUITest::_customLinkEditor_data()
{
    QTest::addColumn<int>("savedType");
    QTest::newRow("serial") << static_cast<int>(LinkConfiguration::TypeSerial);
    QTest::newRow("udp") << static_cast<int>(LinkConfiguration::TypeUdp);
}

void VehicleLinksPageUITest::_customLinkEditor()
{
    QFETCH(int, savedType);
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    VehicleRoleController* const roles = _roles();
    QVERIFY(roles);
    auto* const manager = LinkManager::instance();
    const auto existing = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Editor existing")));
    QPointer<LinkConfiguration> saved;
    const auto cleanup = qScopeGuard([&] {
        manager->removeConfiguration(existing.get());
        if (saved) {
            manager->removeConfiguration(saved.data());
        }
    });
    roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
    roles->addLink(0, QStringLiteral("Radio"), QString());
    QVERIFY(_openPage());
    QQuickItem* const page = findItem(_rootItem, QStringLiteral("vehicleLinksPage"));
    QVERIFY(page);
    QVERIFY(QMetaObject::invokeMethod(page, "_openCreateLinkDialog", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QStringLiteral("Rover")),
                                      Q_ARG(QVariant, QStringLiteral("Radio"))));
    QVERIFY(waitForDialog(QStringLiteral("New Link Configuration")));
    QQuickItem* name = findVisibleItem(_rootItem, QStringLiteral("customLinkName"));
    QQuickItem* save = findVisibleItem(_rootItem, QStringLiteral("popupDialog_acceptButton"));
    QQuickItem* type = findVisibleItem(_rootItem, QStringLiteral("customLinkType"));
    QVERIFY(name && save && type);
    QObject* dialog = name->property("dialog").value<QObject*>();
    QVERIFY(dialog);
    QVERIFY(name->setProperty("text", QStringLiteral("Editor existing")));
    QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("customLinkNameTaken"), 0),
                             TestTimeout::longMs());
    QVERIFY(!save->isEnabled());
    QVERIFY(name->setProperty("text", QStringLiteral("Editor unique")));
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), TestTimeout::longMs());
    QQuickItem* const initialLoader = findItem(_rootItem, QStringLiteral("customLinkSettingsLoader"));
    QVERIFY(initialLoader);
    QVERIFY(initialLoader->setProperty("active", false));
    QVERIFY(!initialLoader->property("item").value<QObject*>());
    QVERIFY(!save->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "_accept"));
    QVERIFY(dialogVisible(QStringLiteral("New Link Configuration")));
    QVERIFY(initialLoader->setProperty("active", true));
    QTRY_VERIFY_WITH_TIMEOUT(initialLoader->property("item").value<QObject*>(), TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), TestTimeout::shortMs());
    QVERIFY(name->setProperty("text", QStringLiteral("Editor existing")));
    QVERIFY(!save->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "_accept"));
    QVERIFY(dialogVisible(QStringLiteral("New Link Configuration")));
    QPointer<LinkConfiguration> first = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(first);
    QSignalSpy firstDestroyed(first, &QObject::destroyed);
    const int firstType = first->type();
    const int secondType =
        firstType == LinkConfiguration::TypeUdp ? LinkConfiguration::TypeTcp : LinkConfiguration::TypeUdp;
    QVERIFY(QMetaObject::invokeMethod(type, "activated", Q_ARG(int, secondType)));
    QTRY_VERIFY_WITH_TIMEOUT(first.isNull(), TestTimeout::shortMs());
    QCOMPARE(firstDestroyed.count(), 1);
    QPointer<LinkConfiguration> second = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(second);
    QSignalSpy secondDestroyed(second, &QObject::destroyed);
    QVERIFY(QMetaObject::invokeMethod(type, "activated", Q_ARG(int, firstType)));
    QTRY_VERIFY_WITH_TIMEOUT(second.isNull(), TestTimeout::shortMs());
    QCOMPARE(secondDestroyed.count(), 1);
    QPointer<LinkConfiguration> current = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(current);
    QCOMPARE(current->type(), static_cast<LinkConfiguration::LinkType>(firstType));
    QSignalSpy currentDestroyed(current, &QObject::destroyed);
    auto* const configurations = manager->property("linkConfigurations").value<QmlObjectListModel*>();
    QVERIFY(configurations);
    const int count = configurations->count();
    QVERIFY(rejectDialog());
    QTRY_VERIFY_WITH_TIMEOUT(current.isNull(), TestTimeout::shortMs());
    QCOMPARE(currentDestroyed.count(), 1);
    QCOMPARE(configurations->count(), count);

    QVERIFY(QMetaObject::invokeMethod(page, "_openCreateLinkDialog", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QStringLiteral("Rover")),
                                      Q_ARG(QVariant, QStringLiteral("Radio"))));
    QVERIFY(waitForDialog(QStringLiteral("New Link Configuration")));
    name = findVisibleItem(_rootItem, QStringLiteral("customLinkName"));
    type = findVisibleItem(_rootItem, QStringLiteral("customLinkType"));
    QQuickItem* const loader = findVisibleItem(_rootItem, QStringLiteral("customLinkSettingsLoader"));
    QVERIFY(name && type && loader);
    dialog = name->property("dialog").value<QObject*>();
    QVERIFY(dialog);
    QVERIFY(name->setProperty("text", QStringLiteral("Editor unique")));
    auto* initial = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(initial);
    if (initial->type() != LinkConfiguration::TypeSerial) {
        QVERIFY(
            QMetaObject::invokeMethod(type, "activated", Q_ARG(int, static_cast<int>(LinkConfiguration::TypeSerial))));
    }
    QTRY_VERIFY_WITH_TIMEOUT(loader->property("item").value<QObject*>(), TestTimeout::longMs());
    QVERIFY(loader->property("source").toUrl().toString().endsWith(QStringLiteral("/SerialSettings.qml")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("serialPortCombo"), TestTimeout::longMs()));
    if (savedType == LinkConfiguration::TypeUdp) {
        QVERIFY(QMetaObject::invokeMethod(type, "activated", Q_ARG(int, savedType)));
        QTRY_VERIFY_WITH_TIMEOUT(loader->property("item").value<QObject*>(), TestTimeout::longMs());
        QVERIFY(loader->property("source").toUrl().toString().endsWith(QStringLiteral("/UdpSettings.qml")));
    }
    save = findVisibleItem(_rootItem, QStringLiteral("popupDialog_acceptButton"));
    QVERIFY(save);
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), TestTimeout::longMs());
    saved = qvariant_cast<LinkConfiguration*>(dialog->property("editingConfig"));
    QVERIFY(saved);
    QVERIFY(acceptDialog());
    QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(QStringLiteral("New Link Configuration")), TestTimeout::shortMs());
    QTRY_COMPARE_WITH_TIMEOUT(configurations->count(), count + 1, TestTimeout::shortMs());
    QCOMPARE(saved->name(), QStringLiteral("Editor unique"));
    QCOMPARE(saved->type(), static_cast<LinkConfiguration::LinkType>(savedType));
    auto* const roleLink = qobject_cast<VehicleRoleLinkEntry*>(roles->linksForSysid(211)->get(0));
    QVERIFY(roleLink);
    QCOMPARE(roleLink->linkConfigName(), QStringLiteral("Editor unique"));
}

void VehicleLinksPageUITest::_connectedTransportStatus()
{
    auto* const manager = LinkManager::instance();
    auto* config = new MockConfiguration(QStringLiteral("Status mock"));
    config->setSystemId(211);
    config->setIncrementVehicleId(false);
    config->setDynamic(true);
    auto listed = manager->addConfiguration(config);
    const auto cleanup = qScopeGuard([&] { manager->removeConfiguration(listed.get()); });
    runWithMockLink(
        [manager, listed]() mutable -> MockLink* {
            return manager->createConnectedLink(listed) ? qobject_cast<MockLink*>(listed->link()) : nullptr;
        },
        [this, listed](QPointer<MockLink> link, Vehicle* vehicle) {
            QVERIFY(link && vehicle && link->isConnected());
            _roles()->addEntry(vehicle->id(), QStringLiteral("Rover"), QString(), 0);
            _roles()->addLink(0, QStringLiteral("Radio"), listed->name());
            QVERIFY(_openPage());
            QQuickItem* const status = findVisibleItemScrolled(QStringLiteral("vehicleLinkStatus_211_0"),
                                                               QStringLiteral("vehicleLinksFlickable"));
            QQuickItem* const action = findItem(_rootItem, QStringLiteral("vehicleLinkAction_211_0"));
            QVERIFY(status && action);
            QGCPalette palette;
            QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Connected"),
                                      TestTimeout::longMs());
            QCOMPARE(status->property("color").value<QColor>(), palette.colorGreen());
            QCOMPARE(action->property("text").toString(), QStringLiteral("Disconnect link"));
        });
}

void VehicleLinksPageUITest::_connectingTransportStatus()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* const manager = LinkManager::instance();
    SharedLinkConfigurationPtr config =
        manager->addConfiguration(new UDPConfiguration(QStringLiteral("Pending status")));
    auto link = std::make_shared<VehicleLinksTestTransport>(config);
    config->setLink(link);
    const auto cleanup = qScopeGuard([&] {
        config->setLink(nullptr);
        manager->removeConfiguration(config.get());
    });
    _roles()->addEntry(211, QStringLiteral("Rover"), QString(), 0);
    _roles()->addLink(0, QStringLiteral("Radio"), config->name());
    QVERIFY(_openPage());
    QQuickItem* const status =
        findVisibleItemScrolled(QStringLiteral("vehicleLinkStatus_211_0"), QStringLiteral("vehicleLinksFlickable"));
    QQuickItem* const action = findItem(_rootItem, QStringLiteral("vehicleLinkAction_211_0"));
    QVERIFY(status && action);
    QGCPalette palette;
    QCOMPARE(status->property("text").toString(), QStringLiteral("Connecting…"));
    QCOMPARE(status->property("color").value<QColor>(), palette.warningText());
    QCOMPARE(action->property("text").toString(), QStringLiteral("Disconnect link"));
    QVERIFY(clickButtonScrolled(QStringLiteral("vehicleLinkAction_211_0"), QStringLiteral("vehicleLinksFlickable")));
    QVERIFY(link->disconnectCalled());
    config->setLink(nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Disconnected"),
                              TestTimeout::shortMs());
    QCOMPARE(status->property("color").value<QColor>(), palette.colorGrey());
    QCOMPARE(action->property("text").toString(), QStringLiteral("Connect"));
}

void VehicleLinksPageUITest::_retryingTransportStatus()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* const manager = LinkManager::instance();
    SharedLinkConfigurationPtr config = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Retry status")));
    const auto cleanup = qScopeGuard([&] { manager->removeConfiguration(config.get()); });
    _roles()->addEntry(211, QStringLiteral("Rover"), QString(), 0);
    _roles()->addLink(0, QStringLiteral("Radio"), config->name());
    QVERIFY(_openPage());
    QQuickItem* const status =
        findVisibleItemScrolled(QStringLiteral("vehicleLinkStatus_211_0"), QStringLiteral("vehicleLinksFlickable"));
    QQuickItem* const action = findItem(_rootItem, QStringLiteral("vehicleLinkAction_211_0"));
    QVERIFY(status && action);
    // Leave time to inspect and stop the pending retry before the manager creates a UDP link.
    for (int i = 0; i < 4; ++i) {
        config->noteReconnectAttempt();
    }
    config->setAutoConnect(true);
    config->setAutoConnectStarted(true);
    QVERIFY(!config->link());
    QGCPalette palette;
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Retrying…"), TestTimeout::shortMs());
    QCOMPARE(status->property("color").value<QColor>(), palette.warningText());
    QCOMPARE(action->property("text").toString(), QStringLiteral("Disconnect link"));
    QVERIFY(clickButtonScrolled(QStringLiteral("vehicleLinkAction_211_0"), QStringLiteral("vehicleLinksFlickable")));
    QVERIFY(config->suppressAutoReconnect());
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Disconnected"),
                              TestTimeout::shortMs());
    QCOMPARE(status->property("color").value<QColor>(), palette.colorGrey());
    QCOMPARE(action->property("text").toString(), QStringLiteral("Connect"));
}

void VehicleLinksPageUITest::_transportStatusChangesLive()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* const manager = LinkManager::instance();
    SharedLinkConfigurationPtr config = manager->addConfiguration(new UDPConfiguration(QStringLiteral("Live status")));
    auto link = std::make_shared<VehicleLinksTestTransport>(config);
    config->setLink(link);
    const auto cleanup = qScopeGuard([&] {
        config->setLink(nullptr);
        manager->removeConfiguration(config.get());
    });
    _roles()->addEntry(211, QStringLiteral("Rover"), QString(), 0);
    _roles()->addLink(0, QStringLiteral("Radio"), config->name());
    QVERIFY(_openPage());
    QQuickItem* const status =
        findVisibleItemScrolled(QStringLiteral("vehicleLinkStatus_211_0"), QStringLiteral("vehicleLinksFlickable"));
    QVERIFY(status);
    QCOMPARE(status->property("text").toString(), QStringLiteral("Connecting…"));
    link->setConnected(true);
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Connected"), TestTimeout::shortMs());
    link->setConnected(false);
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Connecting…"),
                              TestTimeout::shortMs());
    config->setLink(nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Disconnected"),
                              TestTimeout::shortMs());
}

void VehicleLinksPageUITest::_vehicleCommunicationLossChangesLive()
{
    ignoreLogMessage("Vehicle.MavCommandQueue", QtWarningMsg,
                     QRegularExpression("Giving up sending command after max retries:"));
    ignoreLogMessage("Vehicle.StandardModes", QtWarningMsg, QRegularExpression("Failed to retrieve available modes"));
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("Comm loss status"));
    config->setSystemId(211);
    config->setIncrementVehicleId(false);
    runWithMockLink(
        [config]() -> MockLink* {
            SharedLinkConfigurationPtr shared = config;
            return LinkManager::instance()->createConnectedLink(shared) ? qobject_cast<MockLink*>(config->link())
                                                                        : nullptr;
        },
        [this](QPointer<MockLink> link, Vehicle* vehicle) {
            QVERIFY(link && vehicle);
            _roles()->addEntry(vehicle->id(), QStringLiteral("Rover"), QString(), 0);
            QVERIFY(_openPage());
            QQuickItem* const status = findVisibleItemScrolled(QStringLiteral("vehicleConnection_211"),
                                                               QStringLiteral("vehicleLinksFlickable"));
            QVERIFY(status);
            QGCPalette palette;
            QCOMPARE(status->property("text").toString(), QStringLiteral("Vehicle connected"));
            QCOMPARE(status->property("color").value<QColor>(), palette.colorGreen());
            link->setCommLost(true);
            link->waitForWorkerIdle();
            QCoreApplication::sendPostedEvents();
            QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Communication lost"),
                                      VehicleLinkManager::kTestCommLostDetectionTimeoutMs);
            QCOMPARE(status->property("color").value<QColor>(), palette.warningText());
            link->setCommLost(false);
            QTRY_COMPARE_WITH_TIMEOUT(status->property("text").toString(), QStringLiteral("Vehicle connected"),
                                      VehicleLinkManager::kTestCommLostDetectionTimeoutMs);
            QCOMPARE(status->property("color").value<QColor>(), palette.colorGreen());
        });
}

void VehicleLinksPageUITest::_absentVehicleStatus()
{
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    _roles()->addEntry(211, QStringLiteral("Rover"), QString(), 0);
    QVERIFY(_openPage());
    QQuickItem* const status =
        findVisibleItemScrolled(QStringLiteral("vehicleConnection_211"), QStringLiteral("vehicleLinksFlickable"));
    QVERIFY(status);
    QGCPalette palette;
    QCOMPARE(status->property("text").toString(), QStringLiteral("Vehicle not connected"));
    QCOMPARE(status->property("color").value<QColor>(), palette.colorGrey());
}

void VehicleLinksPageUITest::_singleVehicleDisconnectsImmediately()
{
    auto* const manager = LinkManager::instance();
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("Single vehicle link"));
    config->setSystemId(211);
    config->setIncrementVehicleId(false);
    config->setDynamic(true);
    auto listed = manager->addConfiguration(new MockConfiguration(config.get()));
    const auto cleanup = qScopeGuard([&] { manager->removeConfiguration(listed.get()); });
    runWithMockLink(
        [manager, listed]() mutable -> MockLink* {
            if (!manager->createConnectedLink(listed)) {
                return nullptr;
            }
            return qobject_cast<MockLink*>(listed->link());
        },
        [this, listed](QPointer<MockLink> link, Vehicle* vehicle) {
            QVERIFY(link && vehicle);
            auto* const roles = _roles();
            QVERIFY(roles);
            roles->addEntry(vehicle->id(), QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
            roles->addLink(0, QStringLiteral("Radio"), listed->name());
            QVERIFY(_openPage());
            const QString action = QStringLiteral("vehicleLinkAction_%1_0").arg(vehicle->id());
            QQuickItem* const button = findVisibleItemScrolled(action, QStringLiteral("vehicleLinksFlickable"));
            QVERIFY(button);
            QTRY_COMPARE_WITH_TIMEOUT(button->property("text").toString(), QStringLiteral("Disconnect link"),
                                      TestTimeout::longMs());
            QVERIFY(clickButtonScrolled(action, QStringLiteral("vehicleLinksFlickable")));
            QVERIFY(!dialogVisible(QStringLiteral("Disconnect link")));
            QTRY_VERIFY_WITH_TIMEOUT(!listed->link(), TestTimeout::longMs());
        });
}

void VehicleLinksPageUITest::_sharedLinkDisconnectAsksFirst()
{
    auto* const manager = LinkManager::instance();
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("Shared vehicle link"));
    config->setSystemId(211);
    config->setIncrementVehicleId(false);
    config->setDynamic(true);
    auto listed = manager->addConfiguration(new MockConfiguration(config.get()));
    const auto cleanup = qScopeGuard([&] { manager->removeConfiguration(listed.get()); });
    runWithMockLink(
        [manager, listed]() mutable -> MockLink* {
            return manager->createConnectedLink(listed) ? qobject_cast<MockLink*>(listed->link()) : nullptr;
        },
        [this, listed](QPointer<MockLink> link, Vehicle* vehicle) {
            QVERIFY(link && vehicle);
            auto* const roles = _roles();
            QVERIFY(roles);
            roles->addEntry(211, QStringLiteral("Rover"), QStringLiteral("Scout"), 0);
            roles->addEntry(212, QStringLiteral("Plane"), QStringLiteral("Sentinel"), 0);
            roles->addLink(0, QStringLiteral("Radio"), listed->name());
            QVERIFY(_openPage());
            QVERIFY(findVisibleItem(_rootItem, QStringLiteral("vehicleLinksFlickable"), TestTimeout::mediumMs()));
            auto* secondConfig = new MockConfiguration(QStringLiteral("Second vehicle link"));
            secondConfig->setSystemId(212);
            secondConfig->setIncrementVehicleId(false);
            secondConfig->setDynamic(true);
            expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle 212")));
            QPointer<MockLink> secondLink = MockLink::startMockLink(secondConfig);
            QVERIFY(secondLink);
            const auto disconnectSecond = qScopeGuard([&] {
                if (secondLink)
                    secondLink->disconnect();
            });
            auto* const vehicleManager = MultiVehicleManager::instance();
            QTRY_VERIFY_WITH_TIMEOUT(vehicleManager->getVehicleById(212), TestTimeout::longMs());
            Vehicle* const secondVehicle = vehicleManager->getVehicleById(212);
            QTRY_VERIFY_WITH_TIMEOUT(secondVehicle->isInitialConnectComplete(), TestTimeout::longMs());
            QVERIFY(waitForDialog(QCoreApplication::applicationName(), TestTimeout::longMs()));
            verifyExpectedLogMessage();
            QVERIFY(acceptDialog());
            QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(QString()), TestTimeout::shortMs());
            mavlink_message_t heartbeat{};
            heartbeat.msgid = MAVLINK_MSG_ID_HEARTBEAT;
            secondVehicle->vehicleLinkManager()->mavlinkMessageReceived(link, heartbeat);
            QVERIFY(secondVehicle->vehicleLinkManager()->linkNames().contains(listed->name()));
            const QString action = QStringLiteral("vehicleLinkAction_211_0");
            QQuickItem* const button = findVisibleItemScrolled(action, QStringLiteral("vehicleLinksFlickable"));
            QVERIFY(button);
            QTRY_COMPARE_WITH_TIMEOUT(button->property("text").toString(), QStringLiteral("Disconnect link"),
                                      TestTimeout::longMs());
            QVERIFY(clickButtonScrolled(action, QStringLiteral("vehicleLinksFlickable")));
            QVERIFY(waitForDialog(QStringLiteral("Disconnect link")));
            QVERIFY(listed->link() && link->isConnected());
            const std::function<bool(QQuickItem*)> containsMessage = [&](QQuickItem* item) {
                if (item->isVisible()) {
                    const QString text = item->property("text").toString();
                    if (text.contains(QStringLiteral("Scout (211)")) &&
                        text.contains(QStringLiteral("Sentinel (212)"))) {
                        return true;
                    }
                    for (QQuickItem* child : item->childItems()) {
                        if (containsMessage(child)) {
                            return true;
                        }
                    }
                }
                return false;
            };
            QVERIFY(containsMessage(_rootItem));
            QVERIFY(rejectDialog());
            QTRY_VERIFY_WITH_TIMEOUT(!dialogVisible(QString()), TestTimeout::shortMs());
            QVERIFY(listed->link() && link->isConnected());
            QVERIFY(clickButtonScrolled(action, QStringLiteral("vehicleLinksFlickable")));
            QVERIFY(waitForDialog(QStringLiteral("Disconnect link")));
            QVERIFY(listed->link() && link->isConnected());
            QVERIFY(acceptDialog());
            QTRY_VERIFY_WITH_TIMEOUT(!listed->link(), TestTimeout::longMs());
        });
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
    QVERIFY(clickButtonScrolled(QStringLiteral("vehicleAddLink_257"), QStringLiteral("vehicleLinksFlickable")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->linksForSysid(257)->count(), 1, TestTimeout::mediumMs());
    QCOMPARE(roles->linksForSysid(1)->count(), 0);
    QCOMPARE(linkName->property("text").toString(), QString());

    QVERIFY(clickButtonScrolled(QStringLiteral("vehicleRemoveLink_257_0"), QStringLiteral("vehicleLinksFlickable")));
    QTRY_COMPARE_WITH_TIMEOUT(roles->linksForSysid(257)->count(), 0, TestTimeout::mediumMs());

    // Removing a vehicle takes its card with it and leaves the other alone
    QVERIFY(clickButtonScrolled(QStringLiteral("vehicleRemove_257"), QStringLiteral("vehicleLinksFlickable")));
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
            auto secondaryConfig = std::make_shared<MockConfiguration>(QStringLiteral("UI Secondary"));
            secondaryConfig->setDynamic(true);
            secondaryConfig->setIncrementVehicleId(false);
            secondaryConfig->setSystemId(requestedSysid);
            SharedLinkConfigurationPtr sharedSecondaryConfig = secondaryConfig;
            QVERIFY(LinkManager::instance()->createConnectedLink(sharedSecondaryConfig));
            QPointer<MockLink> secondary = qobject_cast<MockLink*>(secondaryConfig->link());
            const auto disconnectSecondary = qScopeGuard([&secondary]() {
                if (secondary) {
                    secondary->disconnect();
                }
            });
            QVERIFY(secondary);
            auto* links = vehicle->vehicleLinkManager();
            QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
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
            QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 3, TestTimeout::longMs());
            auto* parameters = vehicle->parameterManager();
            QTRY_VERIFY_WITH_TIMEOUT(parameters->parametersReady() && !parameters->pendingWrites() &&
                                         parameters->loadProgress() <= 0 && !vehicle->missionManager()->inProgress() &&
                                         !vehicle->geoFenceManager()->inProgress() &&
                                         !vehicle->rallyPointManager()->inProgress(),
                                     TestTimeout::longMs());
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
            roles->addLink(0, QStringLiteral("Secondary"), QStringLiteral("UI Secondary"));
            const quint32 otherSysid = sysid == 1 ? 4294967295U : 1;
            roles->addEntry(otherSysid, QStringLiteral("Plane"), QStringLiteral("Other"), 0);
            roles->addLink(1, QStringLiteral("UDP"), QStringLiteral("UI UDP"));
            QVERIFY(_openPage());
            QQuickItem* const returnStatus = _requireItem(QStringLiteral("vehicleReturnStatus_%1").arg(sysid));
            QVERIFY(returnStatus);
            QTRY_VERIFY_WITH_TIMEOUT(returnStatus->isVisible(), TestTimeout::shortMs());
            QCOMPARE(returnStatus->property("text").toString(),
                     QStringLiteral("Automatic return is off. Tick Preferred on one link to turn it on."));
            QQuickItem* const otherReturnStatus =
                _requireItem(QStringLiteral("vehicleReturnStatus_%1").arg(otherSysid));
            QVERIFY(otherReturnStatus);
            QVERIFY(!otherReturnStatus->isVisible());
            QQuickItem* const firstPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_0").arg(sysid));
            QQuickItem* const secondPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_1").arg(sysid));
            QQuickItem* const secondLost = _requireItem(QStringLiteral("vehicleLinkLost_%1_1").arg(sysid));
            QQuickItem* const otherPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_0").arg(otherSysid));
            QQuickItem* const otherLost = _requireItem(QStringLiteral("vehicleLinkLost_%1_0").arg(otherSysid));
            QVERIFY(firstPrimary && secondPrimary && secondLost && otherPrimary && otherLost);
            const auto checkAmbiguousTag = [this](QQuickItem* tag, const QString& name) {
                auto* const manager = LinkManager::instance();
                const auto first = manager->addConfiguration(new UDPConfiguration(name));
                const auto second = manager->addConfiguration(new UDPConfiguration(name));
                const auto cleanup = qScopeGuard([&] {
                    manager->removeConfiguration(first.get());
                    manager->removeConfiguration(second.get());
                });
                QTRY_VERIFY_WITH_TIMEOUT(!tag->isVisible(), TestTimeout::shortMs());
            };
            QTRY_VERIFY_WITH_TIMEOUT(firstPrimary->isVisible(), TestTimeout::shortMs());
            checkAmbiguousTag(firstPrimary, QStringLiteral("UI RFD"));
            QVERIFY(!QTest::currentTestFailed());
            QTRY_VERIFY_WITH_TIMEOUT(firstPrimary->isVisible(), TestTimeout::shortMs());
            QVERIFY(!secondPrimary->isVisible() && !secondLost->isVisible());
            QVERIFY(!otherPrimary->isVisible() && !otherLost->isVisible());
            QVERIFY(clickButtonScrolled(QStringLiteral("vehiclePreferLink_%1_1").arg(sysid),
                                        QStringLiteral("vehicleLinksFlickable")));
            QCOMPARE(roles->preferredLinkConfigNameForSysid(sysid), QStringLiteral("UI UDP"));
            QVERIFY(roles->preferredLinkConfigNameForSysid(otherSysid).isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(returnStatus->property("text").toString(),
                                      QStringLiteral("Returning to UI UDP in 1 s if it stays steady."),
                                      TestTimeout::shortMs());
            preference->evaluateForTesting(3);
            QTRY_VERIFY_WITH_TIMEOUT(secondPrimary->isVisible(), TestTimeout::shortMs());
            QTRY_COMPARE_WITH_TIMEOUT(returnStatus->property("text").toString(),
                                      QStringLiteral("Using UI UDP, the preferred link."), TestTimeout::shortMs());
            QVERIFY(!firstPrimary->isVisible());

            QCOMPARE(links->linkNames().indexOf(QStringLiteral("UI Secondary")), 1);
            QCOMPARE(links->linkNames().indexOf(QStringLiteral("UI UDP")), 2);
            QQuickItem* const secondaryRowPrimary = _requireItem(QStringLiteral("vehicleLinkPrimary_%1_2").arg(sysid));
            QQuickItem* const secondaryRowLost = _requireItem(QStringLiteral("vehicleLinkLost_%1_2").arg(sysid));
            QVERIFY(secondaryRowPrimary && secondaryRowLost);
            secondary->setCommLost(true);
            QTRY_VERIFY_WITH_TIMEOUT(secondaryRowLost->isVisible(), TestTimeout::longMs());
            checkAmbiguousTag(secondaryRowLost, QStringLiteral("UI Secondary"));
            QVERIFY(!QTest::currentTestFailed());
            QTRY_VERIFY_WITH_TIMEOUT(secondaryRowLost->isVisible(), TestTimeout::shortMs());
            QVERIFY(!secondaryRowPrimary->isVisible());
            QVERIFY(secondPrimary->isVisible() && !secondLost->isVisible());
            QSignalSpy primaryChanges(links, &VehicleLinkManager::primaryLinkChanged);
            QSignalSpy statusChanges(links, &VehicleLinkManager::linkStatusesChanged);
            secondary->disconnect();
            QTRY_COMPARE_WITH_TIMEOUT(links->linkNames().size(), 2, TestTimeout::longMs());
            QCOMPARE(links->linkNames().indexOf(QStringLiteral("UI UDP")), 1);
            // No later signal repairs a stale pre-erase snapshot while UDP stays healthy and primary.
            QTRY_VERIFY_WITH_TIMEOUT(!secondaryRowLost->isVisible(), TestTimeout::shortMs());
            QVERIFY(!secondaryRowPrimary->isVisible());
            QVERIFY(!secondLost->isVisible());
            QVERIFY(!firstPrimary->isVisible() && secondPrimary->isVisible());
            QCOMPARE(links->primaryLinkName(), QStringLiteral("UI UDP"));
            QVERIFY(primaryChanges.isEmpty());
            QCOMPARE(statusChanges.count(), 1);  // Only linkNamesChanged's pre-erase status notification.

            ignoreLogMessage("Vehicle.StandardModes", QtWarningMsg,
                             QRegularExpression(QStringLiteral("Failed to retrieve available modes")));
            expectAppMessage(QRegularExpression(QStringLiteral("Switching communication to secondary link")));
            preferred->setCommLost(true);
            QTRY_VERIFY_WITH_TIMEOUT(secondLost->isVisible(), TestTimeout::longMs());
            QVERIFY(!secondaryRowLost->isVisible() && !secondaryRowPrimary->isVisible());
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
            QTRY_VERIFY_WITH_TIMEOUT(!secondPrimary->isVisible() && !secondLost->isVisible(), TestTimeout::shortMs());
            QQuickItem* const notAttached = _requireItem(QStringLiteral("vehicleLinkNotAttached_%1_1").arg(sysid));
            QVERIFY(notAttached);
            QTRY_VERIFY_WITH_TIMEOUT(notAttached->isVisible(), TestTimeout::shortMs());
            checkAmbiguousTag(notAttached, QStringLiteral("UI UDP"));
            QVERIFY(!QTest::currentTestFailed());
            QTRY_VERIFY_WITH_TIMEOUT(notAttached->isVisible(), TestTimeout::shortMs());
            QCOMPARE(notAttached->property("text").toString(), QStringLiteral("Not connected to this vehicle"));
            backup->disconnect();
            QTRY_COMPARE_WITH_TIMEOUT(MultiVehicleManager::instance()->vehicles()->count(), 0, TestTimeout::longMs());
            QTRY_VERIFY_WITH_TIMEOUT(!firstPrimary->isVisible(), TestTimeout::shortMs());
            QTRY_VERIFY_WITH_TIMEOUT(!notAttached->isVisible(), TestTimeout::shortMs());
            QTRY_VERIFY_WITH_TIMEOUT(returnStatus->property("text").toString().isEmpty(), TestTimeout::shortMs());
            QVERIFY(!returnStatus->isVisible());
            QVERIFY(!secondPrimary->isVisible() && !secondLost->isVisible());
            QTRY_VERIFY_WITH_TIMEOUT(!backup, TestTimeout::longMs());
        });
}
