#include "PlanStyleTest.h"

#include <memory>

#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "CustomPlugin.h"
#include "PlanStyle/CustomPlanStyle.h"
#include "SettingsManager.h"

void PlanStyleTest::init()
{
    VehicleTestManualConnect::init();
    QVERIFY(_directory.isValid());
    QDir saveDirectory(_directory.path());
    QVERIFY(saveDirectory.removeRecursively());
    QVERIFY(saveDirectory.mkpath(QStringLiteral(".")));
    QSettings().remove(QStringLiteral("COP"));
    auto* settings = SettingsManager::instance()->appSettings();
    QVERIFY(settings);
    _savePath = settings->savePath()->rawValue();
    _audioMuted = settings->audioMuted()->rawValue();
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void PlanStyleTest::cleanup()
{
    auto* settings = SettingsManager::instance()->appSettings();
    QVERIFY(settings);
    settings->savePath()->setRawValue(_savePath);
    settings->audioMuted()->setRawValue(_audioMuted);
    VehicleTestManualConnect::cleanup();
}

void PlanStyleTest::_constants()
{
    QGCPlanStyle inert;
    QCOMPARE(inert.revision(), 0);
    QCOMPARE(inert.missionLineWidth(), 0);
    QCOMPARE(inert.boundaryLineWidth(), 0);
    QVERIFY(inert.keepInDashPattern().isEmpty());
    QVERIFY(inert.keepOutDashPattern().isEmpty());
    QVERIFY(!inert.keepOutFillColor().isValid());
    QVERIFY(inert.rallyShape().isEmpty());
    QVERIFY(!inert.markerBorderColor().isValid());
    QCOMPARE(inert.markerBorderWidth(), 0);
    QVERIFY(!inert.markerTextColor().isValid());
    QVERIFY(!inert.vehicleColor(1).isValid());
    QGCCorePlugin stock;
    QVERIFY(!stock.planStyle());

    CustomPlanStyle style;
    QCOMPARE(style.missionLineWidth(), 4);
    QCOMPARE(style.boundaryLineWidth(), 3);
    QCOMPARE(style.keepInDashPattern(), QVariantList({6.0, 3.0}));
    QCOMPARE(style.keepOutDashPattern(), QVariantList({2.0, 2.0}));
    const QColor fill = style.keepOutFillColor();
    QCOMPARE(fill.red(), 229);
    QCOMPARE(fill.green(), 57);
    QCOMPARE(fill.blue(), 53);
    QVERIFY(qAbs(fill.alphaF() - 0.35) < 0.00002);
    QCOMPARE(style.rallyShape(), QStringLiteral("diamond"));
    QCOMPARE(style.markerBorderColor(), QColor(Qt::white));
    QCOMPARE(style.markerBorderWidth(), 2);
    QCOMPARE(style.markerTextColor(), QColor(Qt::black));
    QVERIFY(!style.vehicleColor(1).isValid());
    QVERIFY(!style.vehicleColor(257).isValid());
}

void PlanStyleTest::_colorsAndRestoredOrder()
{
    const QVariantList savedOrder{257U, 1U, 3U, 4U, 5U, 6U, 7U};
    QSettings().setValue(QStringLiteral("COP/VehicleOrder"), savedOrder);
    VehicleRoleController roles;
    // Deliberately reverse the role order relative to the saved discovery order.
    roles.addEntry(1, QStringLiteral("Rover"), QString(), 0);
    roles.addEntry(257, QStringLiteral("Plane"), QString(), 0);
    COPController controller;
    controller.initialize(&roles);
    CustomPlanStyle style;
    style.attachRegistry(&controller);
    QCOMPARE(controller.vehicles()->count(), 7);
    for (quint32 sysid : {1U, 257U}) {
        controller.selectVehicle(sysid);
        auto* entry = controller.selected();
        QVERIFY(entry);
        QVERIFY(style.vehicleColor(sysid).isValid());
        QCOMPARE(style.vehicleColor(sysid), entry->color());
    }
    auto* first = controller.vehicles()->value<COPVehicle*>(0);
    auto* seventh = controller.vehicles()->value<COPVehicle*>(6);
    QVERIFY(first);
    QVERIFY(seventh);
    QCOMPARE(first->sysid(), 257U);
    QCOMPARE(first->color(), QColor(QStringLiteral("#2196F3")));
    QCOMPARE(style.vehicleColor(7), first->color());
    QCOMPARE(seventh->color(), first->color());
    QVERIFY(style.vehicleColor(1) != style.vehicleColor(257));

    const int count = controller.vehicles()->count();
    const int revision = style.revision();
    const QColor original = style.vehicleColor(1);
    QVERIFY(!style.vehicleColor(0).isValid());
    QVERIFY(!style.vehicleColor(999).isValid());
    QCOMPARE(controller.vehicles()->count(), count);
    QCOMPARE(style.revision(), revision);
    QCOMPARE(QSettings().value(QStringLiteral("COP/VehicleOrder")).toList(), savedOrder);
    roles.setRole(0, QStringLiteral("Copter"));
    roles.setName(0, QStringLiteral("Renamed"));
    QCOMPARE(style.vehicleColor(1), original);
    QCOMPARE(controller.vehicles()->count(), count);
    QCOMPARE(style.revision(), revision);

    COPController restored;
    restored.initialize(&roles);
    style.attachRegistry(&restored);
    QCOMPARE(restored.vehicles()->count(), count);
    QCOMPARE(style.vehicleColor(1), original);
    QCOMPARE(style.vehicleColor(257), first->color());
}

void PlanStyleTest::_revisionsAndDetach()
{
    CustomPlanStyle style;
    QSignalSpy changed(&style, &QGCPlanStyle::revisionChanged);
    QVERIFY(changed.isValid());
    VehicleRoleController roles;
    auto controller = std::make_unique<COPController>();
    controller->initialize(&roles);
    style.attachRegistry(controller.get());
    QCOMPARE(style.revision(), 1);
    QCOMPARE(changed.count(), 1);
    style.attachRegistry(controller.get());
    QCOMPARE(changed.count(), 1);

    roles.addEntry(1, QStringLiteral("Rover"), QString(), 0);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(style.revision(), 2);
    controller->selectVehicle(1);
    QPointer<COPVehicle> entry = controller->selected();
    QVERIFY(entry);
    const QColor original = style.vehicleColor(1);
    entry->setColorIndex(1);
    QCOMPARE(changed.count(), 3);
    QCOMPARE(style.revision(), 3);
    QVERIFY(style.vehicleColor(1) != original);
    QCOMPARE(style.vehicleColor(1), entry->color());
    roles.setRole(0, QStringLiteral("Plane"));
    roles.setName(0, QStringLiteral("A name"));
    QCOMPARE(changed.count(), 3);
    QCOMPARE(controller->vehicles()->count(), 1);

    style.attachRegistry(nullptr);
    QCOMPARE(changed.count(), 4);
    QCOMPARE(style.revision(), 4);
    QVERIFY(!style.vehicleColor(1).isValid());
    entry->setColorIndex(2);
    roles.addEntry(257, QStringLiteral("Plane"), QString(), 0);
    QCOMPARE(changed.count(), 4);
    style.attachRegistry(controller.get());
    QCOMPARE(changed.count(), 5);
    QCOMPARE(style.vehicleColor(257), controller->colorForSysid(257));
    controller->selectVehicle(257);
    QVERIFY(controller->selected());
    controller->selected()->setColorIndex(3);
    QCOMPARE(changed.count(), 6);
    QPointer<COPController> guard = controller.get();
    controller.reset();
    QVERIFY(guard.isNull());
    QVERIFY(entry.isNull());
    QCOMPARE(changed.count(), 7);
    QCOMPARE(style.revision(), 7);
    QVERIFY(!style.vehicleColor(257).isValid());
    style.attachRegistry(nullptr);
    QCOMPARE(changed.count(), 7);
}

void PlanStyleTest::_connectedWithoutRole()
{
    VehicleRoleController roles;
    QVERIFY(roles.roleEntries()->count() == 0);
    COPController controller;
    controller.initialize(&roles);
    CustomPlanStyle style;
    style.attachRegistry(&controller);
    _connectMockLink();
    QVERIFY(vehicle());
    const quint32 sysid = vehicle()->id();
    QTRY_VERIFY_WITH_TIMEOUT(controller.colorForSysid(sysid).isValid(), TestTimeout::longMs());
    controller.selectVehicle(sysid);
    auto* entry = controller.selected();
    QVERIFY(entry);
    QVERIFY(entry->connected());
    QCOMPARE(entry->vehicle(), vehicle());
    QVERIFY(roles.roleForSysid(sysid).isEmpty());
    QCOMPARE(style.vehicleColor(sysid), entry->color());
    const int count = controller.vehicles()->count();
    _disconnectMockLink();
    QCOMPARE(style.vehicleColor(sysid), entry->color());
    QCOMPARE(controller.vehicles()->count(), count);
}

void PlanStyleTest::_engineLifecycle()
{
    QPointer<QGCPlanStyle> styleGuard;
    {
        CustomPlugin plugin;
        auto* style = plugin.planStyle();
        QVERIFY(style);
        styleGuard = style;
        QVERIFY(!style->vehicleColor(257).isValid());
        auto destroyEngine = [&plugin](QQmlApplicationEngine* engine) { plugin.destroyQmlApplicationEngine(engine); };
        std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(
            plugin.createQmlApplicationEngine(nullptr), destroyEngine);
        QVERIFY(engine);
        QPointer<QQmlApplicationEngine> engineGuard = engine.get();
        QPointer<COPController> registry = engine->singletonInstance<COPController*>("QGC", "COPController");
        QVERIFY(registry);
        VehicleRoleController roles;
        roles.addEntry(257, QStringLiteral("Plane"), QString(), 0);
        registry->initialize(&roles);
        QCOMPARE(style->vehicleColor(257), registry->colorForSysid(257));
        QVERIFY(style->vehicleColor(257).isValid());
        const int revision = style->revision();
        engine.reset();
        QVERIFY(engineGuard.isNull());
        QVERIFY(registry.isNull());
        QVERIFY(style->revision() > revision);
        QVERIFY(!style->vehicleColor(257).isValid());

        engine.reset(plugin.createQmlApplicationEngine(nullptr));
        QVERIFY(engine);
        registry = engine->singletonInstance<COPController*>("QGC", "COPController");
        QVERIFY(registry);
        registry->initialize(&roles);
        QCOMPARE(style->vehicleColor(257), registry->colorForSysid(257));
        QVERIFY(style->vehicleColor(257).isValid());
        // Also cover deletion by the QObject owner rather than the explicit destroy hook.
        delete engine.release();
        QVERIFY(registry.isNull());
        QVERIFY(!style->vehicleColor(257).isValid());
        plugin.destroyQmlApplicationEngine(nullptr);
    }
    QVERIFY(styleGuard.isNull());
}

void PlanStyleTest::_qmlContextTracksRegistry()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    auto* registry = engine->singletonInstance<COPController*>("QGC", "COPController");
    QVERIFY(registry);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QGroundControl.FlightMap
        PlanStyleContext { vehicleId: 257 }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> context(component.create());
    QVERIFY2(context, qPrintable(component.errorString()));
    QVERIFY(context->property("enabled").toBool());
    QVERIFY(!context->property("vehicleColor").value<QColor>().isValid());
    const QColor neutral = context->property("lineColor").value<QColor>();
    QVERIFY(neutral.isValid());
    QCOMPARE(registry->vehicles()->count(), 0);
    VehicleRoleController roles;
    roles.addEntry(257, QStringLiteral("Plane"), QString(), 0);
    registry->initialize(&roles);
    QTRY_COMPARE_WITH_TIMEOUT(context->property("lineColor").value<QColor>(), registry->colorForSysid(257),
                              TestTimeout::shortMs());
    registry->selectVehicle(257);
    QVERIFY(registry->selected());
    registry->selected()->setColorIndex(2);
    QTRY_COMPARE_WITH_TIMEOUT(context->property("vehicleColor").value<QColor>(), registry->selected()->color(),
                              TestTimeout::shortMs());
    QVERIFY(context->setProperty("vehicleId", 999U));
    QCOMPARE(context->property("lineColor").value<QColor>(), neutral);
    QCOMPARE(registry->vehicles()->count(), 1);
    QVERIFY(context->setProperty("supported", false));
    QVERIFY(!context->property("enabled").toBool());
    auto* style = qobject_cast<CustomPlanStyle*>(plugin->planStyle());
    QVERIFY(style);
    style->attachRegistry(nullptr);
    QCOMPARE(context->property("lineColor").value<QColor>(), neutral);
    style->attachRegistry(registry);
    QVERIFY(context->setProperty("vehicleId", 257U));
    QCOMPARE(context->property("lineColor").value<QColor>(), registry->colorForSysid(257));
    roles.addEntry(4294967295U, QStringLiteral("Rover"), QString(), 0);
    QVERIFY(context->setProperty("vehicleId", 4294967295.0));
    QCOMPARE(context->property("vehicleId").toDouble(), 4294967295.0);
    QCOMPARE(context->property("vehicleColor").value<QColor>(), registry->colorForSysid(4294967295U));
    QVERIFY(context->property("vehicleColor").value<QColor>().isValid());
}

UT_REGISTER_TEST(PlanStyleTest, TestLabel::Unit)
