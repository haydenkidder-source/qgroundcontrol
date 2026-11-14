#include "PlanStyleComponentTest.h"

#include <memory>

#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>

#include "QGCCorePlugin.h"
#include "QGCPlanStyle.h"

void PlanStyleComponentTest::_sharedComponents()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtQuick.Shapes
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        Item {
            readonly property int dashedStrokeStyle: ShapePath.DashLine
            readonly property color neutralColor: QGroundControl.globalPalette.mapMissionTrajectory
            readonly property real markerSize: ScreenTools.defaultFontPixelHeight * 1.6
            MapDashedLine {
                objectName: "line"
                width: 100
                height: 50
                points: [Qt.point(0, 0), Qt.point(100, 50)]
                color: "blue"
                dashPattern: [6, 3]
            }
            MapBoundaryOutline {
                objectName: "outline"
                lineColor: "blue"
                dashPattern: [2, 2]
            }
            RallyPointMarker {
                objectName: "marker"
                color: "blue"
                borderColor: "white"
                borderWidth: 2
                textColor: "black"
            }
            PlanStyleContext { }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* line = root->findChild<QQuickItem*>(QStringLiteral("line"));
    auto* outline = root->findChild<QQuickItem*>(QStringLiteral("outline"));
    auto* marker = root->findChild<QQuickItem*>(QStringLiteral("marker"));
    auto* context = root->findChild<QObject*>(QStringLiteral("planStyleContext"));
    QVERIFY(line);
    QVERIFY(outline);
    QVERIFY(marker);
    QVERIFY(context);
    QCOMPARE(line->property("lineWidth").toReal(), 3);
    QCOMPARE(line->property("dashPattern").toList(), QVariantList({6.0, 3.0}));
    const auto strokePath = [](QObject* shape) -> QObject* {
        if (!shape) {
            return nullptr;
        }
        for (auto* child : shape->findChildren<QObject*>()) {
            if (child && child->metaObject()->indexOfProperty("strokeWidth") >= 0) {
                return child;
            }
        }
        return nullptr;
    };
    auto* path = strokePath(line);
    QVERIFY(path);
    QCOMPARE(path->property("strokeWidth").toReal(), 3);
    QCOMPARE(path->property("strokeStyle").toInt(), root->property("dashedStrokeStyle").toInt());
    QCOMPARE(path->property("fillColor").value<QColor>(), QColor(Qt::transparent));
    QCOMPARE(path->property("strokeColor").value<QColor>(), QColor(Qt::blue));
    QCOMPARE(path->property("dashPattern").toList(), QVariantList({6.0, 3.0}));
    QVERIFY(line->setProperty("lineWidth", 5));
    QCOMPARE(path->property("strokeWidth").toReal(), 5);
    auto* outlineLine = qvariant_cast<QQuickItem*>(outline->property("sourceItem"));
    QVERIFY(outlineLine);
    QCOMPARE(outline->property("lineWidth").toReal(), 3);
    QCOMPARE(outlineLine->property("lineWidth").toReal(), 3);
    QCOMPARE(outlineLine->property("color").value<QColor>(), QColor(Qt::blue));
    QCOMPARE(outlineLine->property("dashPattern").toList(), QVariantList({2.0, 2.0}));
    QVERIFY(outline->setProperty("lineWidth", 7));
    QCOMPARE(outlineLine->property("lineWidth").toReal(), 7);
    QVERIFY(outline->property("_screenPoints").toList().isEmpty());

    QCOMPARE(marker->width(), root->property("markerSize").toReal());
    QCOMPARE(marker->height(), marker->width());
    QQuickItem* diamond = nullptr;
    QQuickItem* label = nullptr;
    for (auto* child : marker->childItems()) {
        if (!child) {
            continue;
        }
        if (child->rotation() == 45) {
            diamond = child;
        } else if (child->metaObject()->indexOfProperty("text") >= 0) {
            label = child;
        }
    }
    QVERIFY(diamond);
    QVERIFY(label);
    QCOMPARE(diamond->width(), marker->width() * 0.72);
    QCOMPARE(diamond->height(), diamond->width());
    QCOMPARE(diamond->property("color").value<QColor>(), QColor(Qt::blue));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.color")).read().value<QColor>(), QColor(Qt::white));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.width")).read().toReal(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(diamond->x() - (marker->width() - diamond->width()) / 2) < 1, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(diamond->y() - (marker->height() - diamond->height()) / 2) < 1,
                             TestTimeout::shortMs());
    QCOMPARE(label->rotation(), 0);
    QVERIFY(QQmlProperty(label, QStringLiteral("font.bold")).read().toBool());
    QCOMPARE(label->property("text").toString(), QStringLiteral("R"));
    QCOMPARE(label->property("color").value<QColor>(), QColor(Qt::black));
    QVERIFY(marker->setProperty("color", QColor(Qt::green)));
    QCOMPARE(diamond->property("color").value<QColor>(), QColor(Qt::green));
    QVERIFY(marker->setProperty("borderColor", QColor(Qt::red)));
    QVERIFY(marker->setProperty("borderWidth", 4));
    QVERIFY(marker->setProperty("textColor", QColor(Qt::yellow)));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.color")).read().value<QColor>(), QColor(Qt::red));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.width")).read().toReal(), 4);
    QCOMPARE(label->property("color").value<QColor>(), QColor(Qt::yellow));

    auto* policy = plugin->planStyle();
    QCOMPARE(context->property("enabled").toBool(), policy != nullptr);
    QCOMPARE(context->property("vehicleId").toUInt(), 0U);
    QCOMPARE(context->property("lineColor").value<QColor>(), root->property("neutralColor").value<QColor>());
    QVERIFY(context->property("lineColor").value<QColor>().isValid());
    QCOMPARE(context->property("missionLineWidth").toReal(), policy ? policy->missionLineWidth() : 0);
    QCOMPARE(context->property("boundaryLineWidth").toReal(), policy ? policy->boundaryLineWidth() : 0);
    QCOMPARE(context->property("keepInDashPattern").toList(), policy ? policy->keepInDashPattern() : QVariantList());
    QCOMPARE(context->property("keepOutDashPattern").toList(), policy ? policy->keepOutDashPattern() : QVariantList());
    QCOMPARE(context->property("keepOutFillColor").value<QColor>(), policy ? policy->keepOutFillColor() : QColor());
    QCOMPARE(context->property("markerBorderColor").value<QColor>(), policy ? policy->markerBorderColor() : QColor());
    QCOMPARE(context->property("markerBorderWidth").toReal(), policy ? policy->markerBorderWidth() : 0);
    QCOMPARE(context->property("markerTextColor").value<QColor>(), policy ? policy->markerTextColor() : QColor());
    QCOMPARE(context->property("rallyShape").toString(), policy ? policy->rallyShape() : QString());
    QVERIFY(context->setProperty("supported", false));
    QVERIFY(!context->property("enabled").toBool());
}

UT_REGISTER_TEST(PlanStyleComponentTest, TestLabel::Unit)
