#include "PlanStyleUITest.h"

#include <limits>
#include <memory>
#include <vector>

#include <QtCore/QAbstractItemModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaProperty>
#include <QtCore/QPair>
#include <QtCore/QRectF>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QStyleHints>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtTest/QSignalSpy>

#include "AppSettings.h"
#include "COPController.h"
#include "Fact.h"
#include "FirmwarePlugin.h"
#include "FirmwareUpgradeSettings.h"
#include "FixedWingLandingComplexItem.h"
#include "FlightPathSegment.h"
#include "GeoFenceController.h"
#include "GeoFenceManager.h"
#include "LinkManager.h"
#include "MissionController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MissionSettingsItem.h"
#include "MultiVehicleManager.h"
#include "ParameterManager.h"
#include "PerimeterScanComplexItem.h"
#include "PlanManager.h"
#include "PlanMasterController.h"
#include "PlanStyle/CustomPlanStyle.h"
#include "PlanStylePatternTest.h"
#include "PlanViewSettings.h"
#include "QGCCorePlugin.h"
#include "QGCFenceCircle.h"
#include "QGCFencePolygon.h"
#include "QGCMAVLink.h"
#include "QGCMapCircle.h"
#include "QGCMapPolygon.h"
#include "QGCPlanStyle.h"
#include "QGroundControlQmlGlobal.h"
#include "QmlObjectListModel.h"
#include "RallyPoint.h"
#include "RallyPointController.h"
#include "RallyPointManager.h"
#include "SettingsFact.h"
#include "SettingsManager.h"
#include "SimpleMissionItem.h"
#include "TakeoffMissionItem.h"
#include "VTOLLandingComplexItem.h"
#include "Vehicle.h"
#include "VehicleRoleController.h"
#include "VisualMissionItem.h"

namespace {
void planStyleWarmUpScreenTools(QQmlApplicationEngine* engine)
{
    // Match main-window singleton initialization so the stock GCS image's height binding cannot re-enter itself.
    QQmlComponent component(engine);
    component.setData(R"(
        import QtQuick
        import QGroundControl.Controls
        Item { property real h: ScreenTools.defaultFontPixelHeight }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
}

QList<QQuickItem*> planStyleItems(QQuickItem* root, const QString& name)
{
    QList<QQuickItem*> result;
    QSet<QObject*> visited;
    // Map items leave their Loader's visual tree; sourceItem may remain detached until window polish.
    const auto visit = [&](QObject* object, auto&& self) -> void {
        if (!object || visited.contains(object)) {
            return;
        }
        visited.insert(object);
        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            if (item->objectName() == name) {
                result.append(item);
            }
            for (auto* child : item->childItems()) {
                self(child, self);
            }
            self(qvariant_cast<QQuickItem*>(item->property("sourceItem")), self);
        }
        for (auto* child : object->children()) {
            self(child, self);
        }
    };
    visit(root, visit);
    return result;
}

QQuickItem* planStyleTab(QQuickItem* root, const QString& text)
{
    if (!root || !root->isVisible()) {
        return nullptr;
    }
    if (root->property("text").toString() == text && root->metaObject()->indexOfSignal("clicked()") >= 0) {
        return root;
    }
    for (auto* child : root->childItems()) {
        if (auto* result = planStyleTab(child, text)) {
            return result;
        }
    }
    return nullptr;
}

QColor planStyleColor(QObject* object, const QString& property)
{
    return QQmlProperty(object, property).read().value<QColor>();
}

QQuickItem* planStyleSource(QQuickItem* item)
{
    return item ? qvariant_cast<QQuickItem*>(item->property("sourceItem")) : nullptr;
}

bool planStyleRenderMap(QQuickItem* map)
{
    auto* window = map->window();
    return window && window->isExposed() && !window->grabWindow().isNull();
}

bool planStyleProjectedPosition(QQuickItem* map, const QGeoCoordinate& coordinate, const QPointF& scenePosition)
{
    QPointF projected;
    return coordinate.isValid() &&
           QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, projected),
                                     Q_ARG(QGeoCoordinate, coordinate), Q_ARG(bool, false)) &&
           (scenePosition - map->mapToScene(projected)).manhattanLength() < 1;
}

bool planStyleMapItemProjected(QQuickItem* map, QQuickItem* item)
{
    auto* source = planStyleSource(item);
    // A sourceItem may carry its own offset or transform, as MapLineArrow does.
    return source && source->window() == map->window() &&
           planStyleProjectedPosition(map, item->property("coordinate").value<QGeoCoordinate>(),
                                      item->mapToScene(item->property("anchorPoint").toPointF()));
}

void planStyleCheckBoundaries(QQuickItem* map, quint32 vehicleId, const QColor& color, int polygonCount,
                              int circleCount)
{
    const auto fills = planStyleItems(map, QStringLiteral("planFenceFill"));
    const auto keepIn = planStyleItems(map, QStringLiteral("planKeepInOutline"));
    const auto keepOut = planStyleItems(map, QStringLiteral("planKeepOutOutline"));
    QCOMPARE(fills.size(), polygonCount + circleCount + 1);
    QCOMPARE(keepIn.size() + keepOut.size(), fills.size());
    QSet<QQuickItem*> matched;
    int polygons = 0;
    int circles = 0;
    int parameterCircles = 0;
    for (auto* fill : fills) {
        QCOMPARE(fill->property("vehicleId").toUInt(), vehicleId);
        const auto kind = fill->property("boundaryKind").toString();
        const bool inclusion = fill->property("inclusion").toBool();
        const auto outlines = inclusion ? keepIn : keepOut;
        QQuickItem* outline = nullptr;
        for (auto* candidate : outlines) {
            if (matched.contains(candidate) || candidate->property("boundaryKind").toString() != kind) {
                continue;
            }
            if (kind == QStringLiteral("polygon")) {
                if (candidate->property("vertices").toList() != fill->property("path").toList()) {
                    continue;
                }
            } else {
                const auto vertices = candidate->property("vertices").toList();
                const auto center = fill->property("center").value<QGeoCoordinate>();
                const double radius = fill->property("radius").toDouble();
                if (center.isValid() && radius > 0 &&
                    (vertices.size() != 120 || vertices.first().value<QGeoCoordinate>().distanceTo(
                                                   center.atDistanceAndAzimuth(radius, 0)) > 0.01)) {
                    continue;
                }
            }
            outline = candidate;
            break;
        }
        QVERIFY(outline);
        matched.insert(outline);
        QTRY_VERIFY_WITH_TIMEOUT(!outline->property("_projectionPending").toBool(), TestTimeout::shortMs());
        QCOMPARE(outline->property("vehicleId").toUInt(), vehicleId);
        QCOMPARE(outline->property("lineColor").value<QColor>(), color);
        QCOMPARE(outline->property("renderedStrokeColor").value<QColor>(), color);
        QCOMPARE(outline->property("lineWidth").toReal(), 3.0);
        QCOMPARE(outline->property("renderedStrokeWidth").toReal(), 3.0);
        const QVariantList dashes = inclusion ? QVariantList{6.0, 3.0} : QVariantList{2.0, 2.0};
        QCOMPARE(outline->property("dashPattern").toList(), dashes);
        QCOMPARE(outline->property("renderedDashPattern").toList(), dashes);
        auto* shape = planStyleSource(outline);
        QVERIFY(shape);
        QObject* stroke = nullptr;
        for (auto* child : shape->findChildren<QObject*>()) {
            if (child->metaObject()->indexOfProperty("strokeStyle") >= 0) {
                stroke = child;
                break;
            }
        }
        QVERIFY(stroke);
        const auto styleEnum = stroke->metaObject()->enumerator(stroke->metaObject()->indexOfEnumerator("StrokeStyle"));
        QVERIFY(styleEnum.isValid());
        QCOMPARE(stroke->property("strokeStyle").toInt(), styleEnum.keyToValue("DashLine"));
        QCOMPARE(outline->property("renderedStrokeStyle").toInt(), stroke->property("strokeStyle").toInt());
        QCOMPARE(stroke->property("strokeColor").value<QColor>(), color);
        QCOMPARE(stroke->property("strokeWidth").toReal(), 3.0);
        QCOMPARE(stroke->property("dashPattern").toList(), dashes);
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), 0.0);
        const auto fillColor = fill->property("color").value<QColor>();
        if (inclusion) {
            QCOMPARE(fillColor.alpha(), 0);
        } else {
            QCOMPARE(fillColor.red(), 229);
            QCOMPARE(fillColor.green(), 57);
            QCOMPARE(fillColor.blue(), 53);
            QVERIFY(qAbs(fillColor.alphaF() - 0.35) < 0.005);
        }
        QCOMPARE(fill->opacity(), outline->opacity());
        QVERIFY(outline->z() > fill->z());
        QVERIFY(outline->z() < 47);
        QVERIFY(outline->z() < 51);
        for (auto* route : planStyleItems(map, QStringLiteral("planMissionRoute"))) {
            QVERIFY(outline->z() < route->z());
        }
        QCOMPARE(outline->acceptedMouseButtons(), Qt::NoButton);
        QCOMPARE(shape->acceptedMouseButtons(), Qt::NoButton);
        if (kind == QStringLiteral("polygon")) {
            ++polygons;
        } else if (kind == QStringLiteral("circle")) {
            ++circles;
        } else {
            QCOMPARE(kind, QStringLiteral("parameterCircle"));
            ++parameterCircles;
        }
        const auto vertices = outline->property("vertices").toList();
        if (vertices.isEmpty()) {
            QVERIFY(!outline->isVisible());
            continue;
        }
        const auto projected = [&] { return outline->property("_screenPoints").toList(); };
        // MapQuickItem placement is polished by the whole map's render pass, not by its source alone.
        QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map) && !outline->property("_projectionPending").toBool() &&
                                     planStyleMapItemProjected(map, outline),
                                 TestTimeout::shortMs());
        QTRY_COMPARE_WITH_TIMEOUT(projected().size(), vertices.size() + 1, TestTimeout::shortMs());
        QTRY_COMPARE_WITH_TIMEOUT(shape->property("points").toList(), projected(), TestTimeout::shortMs());
        const QPointF anchor = outline->property("anchorPoint").toPointF();
        QCOMPARE(outline->property("coordinate").value<QGeoCoordinate>(), vertices.first().value<QGeoCoordinate>());
        QCOMPARE(projected().first().toPointF(), anchor);
        QCOMPARE(projected().last().toPointF(), anchor);
        QPointF origin;
        QVERIFY(QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, origin),
                                          Q_ARG(QGeoCoordinate, vertices.first().value<QGeoCoordinate>()),
                                          Q_ARG(bool, false)));
        const auto expectedScene = map->mapToScene(origin);
        QTRY_VERIFY_WITH_TIMEOUT((shape->mapToScene(anchor) - expectedScene).manhattanLength() < 1,
                                 TestTimeout::shortMs());
        const QRectF bounds(0, 0, shape->width(), shape->height());
        // Edge vertices can round one ulp beyond the extent used to size the Shape.
        const auto tolerantBounds = bounds.adjusted(-0.5, -0.5, 0.5, 0.5);
        for (const auto& point : projected()) {
            QVERIFY(tolerantBounds.contains(point.toPointF()));
        }
        for (int i = 0; i < vertices.size(); ++i) {
            QPointF position;
            const auto vertex = vertices[i].value<QGeoCoordinate>();
            QVERIFY(QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, position),
                                              Q_ARG(QGeoCoordinate, vertex), Q_ARG(bool, false)));
            QTRY_VERIFY_WITH_TIMEOUT((projected()[i].toPointF() - anchor - position + origin).manhattanLength() < 0.05,
                                     TestTimeout::shortMs());
            if (kind != QStringLiteral("polygon")) {
                const auto center = fill->property("center").value<QGeoCoordinate>();
                const double radius = fill->property("radius").toDouble();
                QVERIFY(vertex.distanceTo(center.atDistanceAndAzimuth(radius, i * 3)) < 0.01);
            }
        }
    }
    QCOMPARE(polygons, polygonCount);
    QCOMPARE(circles, circleCount);
    QCOMPARE(parameterCircles, 1);
}

void planStyleCheckRally(QQuickItem* map, QQuickItem* marker, quint32 vehicleId, const QColor& color)
{
    QVERIFY(marker);
    // Render before reading source geometry; offscreen Fly markers need not be pointer targets.
    QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map), TestTimeout::shortMs());
    QCOMPARE(marker->property("vehicleId").toUInt(), vehicleId);
    QCOMPARE(marker->property("markerShape").toString(), QStringLiteral("diamond"));
    QCOMPARE(marker->property("markerColor").value<QColor>(), color);
    QCOMPARE(marker->property("markerBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
    QCOMPARE(marker->property("markerBorderWidth").toReal(), 2.0);
    QCOMPARE(marker->property("label").toString(), QStringLiteral("R"));
    auto* loader = planStyleSource(marker);
    QVERIFY(loader);
    auto* source = qvariant_cast<QQuickItem*>(loader->property("item"));
    QVERIFY(source);
    QCOMPARE(source->property("anchorPointX").toReal(), source->width() / 2);
    QCOMPARE(source->property("anchorPointY").toReal(), source->height() / 2);
    QCOMPARE(loader->property("anchorPointX").toReal(), source->property("anchorPointX").toReal());
    QCOMPARE(loader->property("anchorPointY").toReal(), source->property("anchorPointY").toReal());
    QQuickItem* diamond = nullptr;
    QQuickItem* text = nullptr;
    for (auto* child : source->childItems()) {
        if (child->rotation() == 45) {
            diamond = child;
        }
        if (child->property("text").toString() == QStringLiteral("R")) {
            text = child;
        }
    }
    QVERIFY(diamond && text);
    QCOMPARE(diamond->width(), source->width() * 0.72);
    QCOMPARE(diamond->height(), diamond->width());
    QCOMPARE(diamond->property("radius").toReal(), 0.0);
    QCOMPARE(diamond->property("color").value<QColor>(), color);
    QCOMPARE(planStyleColor(diamond, QStringLiteral("border.color")), QColor(QStringLiteral("white")));
    QCOMPARE(QQmlProperty(diamond, QStringLiteral("border.width")).read().toReal(), 2.0);
    QCOMPARE(text->rotation(), 0.0);
    QCOMPARE(source->rotation(), 0.0);
    QCOMPARE(loader->rotation(), 0.0);
    QCOMPARE(marker->rotation(), 0.0);
    QCOMPARE(text->property("color").value<QColor>(), QColor(QStringLiteral("black")));
    QVERIFY(QQmlProperty(text, QStringLiteral("font.bold")).read().toBool());
    const auto halos = planStyleItems(marker, QStringLiteral("planRallySelectionHalo"));
    QCOMPARE(halos.size(), 1);
    QCOMPARE(halos.first()->isVisible(), marker->property("checked").toBool());
    QVERIFY(halos.first()->width() > source->width());
    QVERIFY(QQmlProperty(halos.first(), QStringLiteral("border.width")).read().toReal() > 0);
    QVERIFY(planStyleColor(halos.first(), QStringLiteral("border.color")).alpha() > 0);
}

QString planStyleItemChain(QQuickItem* item)
{
    QStringList chain;
    for (auto* ancestor = item; ancestor; ancestor = ancestor->parentItem()) {
        chain.append(QStringLiteral("%1(%2,index=%3,visible=%4,opacity=%5)")
                         .arg(QString::fromLatin1(ancestor->metaObject()->className()), ancestor->objectName(),
                              ancestor->property("index").toString())
                         .arg(ancestor->isVisible())
                         .arg(ancestor->opacity()));
    }
    return chain.isEmpty() ? QStringLiteral("null") : chain.join(QStringLiteral(" -> "));
}

QString planStyleLookupContext(QQuickItem* map, const QString& name, qsizetype expectedCount)
{
    const auto matches = planStyleItems(map, name);
    QStringList details;
    for (auto* match : matches) {
        auto* source = qvariant_cast<QQuickItem*>(match->property("sourceItem"));
        details.append(QStringLiteral("%1 sourceIndex=%2 coordinate=%3 body=%4 stroke=%5")
                           .arg(planStyleItemChain(match),
                                source ? source->property("index").toString() : QStringLiteral("null"),
                                match->property("coordinate").value<QGeoCoordinate>().toString(),
                                match->property("renderedBodyColor").toString(),
                                match->property("renderedStrokeColor").toString()));
    }
    return QStringLiteral("lookup %1: count=%2 expected=%3 mapReady=%4; [%5]")
        .arg(name)
        .arg(matches.size())
        .arg(expectedCount)
        .arg(map->property("mapReady").toBool())
        .arg(details.join(QStringLiteral("; ")));
}

bool planStyleResolveStableItem(const std::function<QQuickItem*()>& resolve, QPointer<QQuickItem>& previous,
                                QPointer<QQuickItem>& current)
{
    // A still-visible delegate may already have been replaced and queued for deletion.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    current = resolve();
    const bool same = current && current == previous;
    previous = current;
    return same;
}

void planStyleWaitForVisualBindings(QQuickItem* map, QmlObjectListModel* model, QObject* context)
{
    QVERIFY2(map && model && context, "visual bindings: missing map, mission model or context");
    const auto unbound = [&] {
        QStringList pending;
        for (int i = 0; i < model->count(); ++i) {
            auto* missionItem = qobject_cast<VisualMissionItem*>(model->get(i));
            if (!missionItem) {
                continue;
            }
            QList<QQuickItem*> roots;
            for (auto* candidate : map->childItems()) {
                if (qvariant_cast<VisualMissionItem*>(candidate->property("_missionItem")) == missionItem) {
                    roots.append(candidate);
                }
            }
            if (roots.size() != 1 || qvariant_cast<QObject*>(roots.first()->property("planStyle")) != context) {
                pending.append(
                    QStringLiteral("unbound root %1 sequence=%2 mapParentedRoots=%3 [%4]")
                        .arg(missionItem->mapVisualQML())
                        .arg(missionItem->sequenceNumber())
                        .arg(roots.size())
                        .arg(roots.isEmpty() ? QStringLiteral("missing") : planStyleItemChain(roots.first())));
            }
        }
        for (const auto& name : {QStringLiteral("planWaypoint"), QStringLiteral("planPatternMarker"),
                                 QStringLiteral("planPerimeterMarker")}) {
            for (auto* marker : planStyleItems(map, name)) {
                auto* label = marker->property("planStyle").isValid() ? marker : planStyleSource(marker);
                if (!label || qvariant_cast<QObject*>(label->property("planStyle")) != context) {
                    pending.append(QStringLiteral("unbound marker %1 vehicleId=%2 label=[%3]")
                                       .arg(planStyleItemChain(marker), marker->property("vehicleId").toString(),
                                            planStyleItemChain(label)));
                }
            }
        }
        return pending.join(QStringLiteral("; "));
    };
    // Component.onCompleted can create map primitives before Loader.onLoaded binds/reparents their root.
    QTRY_VERIFY2_WITH_TIMEOUT(([&] {
                                  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                                  return unbound().isEmpty();
                              }()),
                              qPrintable(QStringLiteral("visual bindings: %1").arg(unbound())),
                              TestTimeout::mediumMs());
}

qreal planStyleOpacityBelowMap(QQuickItem* item, QQuickItem* map)
{
    qreal opacity = 1;
    auto* ancestor = item;
    while (ancestor && ancestor != map) {
        opacity *= ancestor->opacity();
        ancestor = ancestor->parentItem();
    }
    if (ancestor == map) {
        return opacity;
    }
    // A MapQuickItem source can be detached before polish or when culled. Follow only a verified source edge.
    opacity = 1;
    for (ancestor = item; ancestor; ancestor = ancestor->parentItem()) {
        opacity *= ancestor->opacity();
        for (auto* owner : map->childItems()) {
            if (planStyleSource(owner) == ancestor) {
                return opacity * planStyleOpacityBelowMap(owner, map);
            }
        }
    }
    return qQNaN();
}

QString planStyleOpacityContext(QQuickItem* item, QQuickItem* map, qreal expected)
{
    QStringList owners;
    for (auto* owner : map->childItems()) {
        if (planStyleSource(owner) == item) {
            owners.append(planStyleItemChain(owner));
        }
    }
    return QStringLiteral("opacity actual=%1 expected=%2; label/item chain: %3; source owners: [%4]")
        .arg(planStyleOpacityBelowMap(item, map))
        .arg(expected)
        .arg(planStyleItemChain(item), owners.join(QStringLiteral("; ")));
}

QQuickItem* planStyleMouseItemAt(QQuickItem* item, const QPointF& position)
{
    // MouseArea.enabled shadows Item.enabled. Both must allow the press.
    if (!item || !item->isVisible() || !item->isEnabled() || !item->property("enabled").toBool()) {
        return nullptr;
    }
    for (auto* child : item->childItems()) {
        if (auto* hit = planStyleMouseItemAt(child, position)) {
            return hit;
        }
    }
    return item->acceptedMouseButtons().testFlag(Qt::LeftButton) && item->contains(item->mapFromScene(position))
               ? item
               : nullptr;
}

QString planStyleProjectionContext(QQuickItem* map, QQuickItem* item, const QGeoCoordinate& coordinate,
                                   const QPointF& anchor)
{
    auto* source = planStyleSource(item);
    const auto actual = item->mapToScene(anchor);
    QPointF projected;
    const bool valid =
        coordinate.isValid() && QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, projected),
                                                          Q_ARG(QGeoCoordinate, coordinate), Q_ARG(bool, false));
    const auto expected = valid ? map->mapToScene(projected) : QPointF(qQNaN(), qQNaN());
    return QStringLiteral(
               "objectName=%1 class=%2 sourceClass=%3 anchor=(%4,%5) actualScene=(%6,%7) expectedScene=(%8,%9) "
               "inView=%10 pending=%11")
        .arg(item->objectName(), QString::fromLatin1(item->metaObject()->className()),
             source ? QString::fromLatin1(source->metaObject()->className()) : QStringLiteral("null"))
        .arg(anchor.x())
        .arg(anchor.y())
        .arg(actual.x())
        .arg(actual.y())
        .arg(expected.x())
        .arg(expected.y())
        .arg(valid && map->boundingRect().contains(projected))
        .arg(item->property("_projectionPending").toBool());
}

QString planStyleEditorProjectionMismatch(QQuickItem* map)
{
    const auto culled = [map](QQuickItem* item) {
        auto* source = planStyleSource(item);
        QPointF position;
        return source && !source->isVisible() &&
               QMetaObject::invokeMethod(map, "fromCoordinate", Q_RETURN_ARG(QPointF, position),
                                         Q_ARG(QGeoCoordinate, item->property("coordinate").value<QGeoCoordinate>()),
                                         Q_ARG(bool, false)) &&
               !map->boundingRect().contains(position);
    };
    const auto projected = [map, &culled](QQuickItem* item) -> QString {
        if (!item->isVisible() || culled(item) || !item->property("coordinate").value<QGeoCoordinate>().isValid()) {
            return {};
        }
        const auto context = [map, item] {
            return planStyleProjectionContext(map, item, item->property("coordinate").value<QGeoCoordinate>(),
                                              item->property("anchorPoint").toPointF());
        };
        if (item->metaObject()->indexOfProperty("_screenPoints") >= 0) {
            if (item->property("_projectionPending").toBool()) {
                return context();
            }
            // Tracing has no drawable outline until its second vertex.
            if (item->property("vertices").toList().size() < 2) {
                return {};
            }
        }
        return planStyleMapItemProjected(map, item) ? QString() : context();
    };
    for (auto* item : map->childItems()) {
        if (!item->isVisible()) {
            continue;
        }
        if (planStyleSource(item)) {
            const auto mismatch = projected(item);
            if (!mismatch.isEmpty()) {
                return mismatch;
            }
        }
        if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            auto* indicator = qvariant_cast<QQuickItem*>(item->property("itemIndicator"));
            if (!indicator || !indicator->isVisible() || culled(indicator)) {
                continue;
            }
            // Idle polygon draggers keep their creation coordinate while the indicator follows model edits.
            const auto coordinate = (item->property("_dragStartSignalled").toBool() ? item->property("itemCoordinate")
                                                                                    : indicator->property("coordinate"))
                                        .value<QGeoCoordinate>();
            if (!coordinate.isValid()) {
                continue;
            }
            const auto anchor = indicator->property("anchorPoint").toPointF() +
                                QPointF(item->property("_touchMarginHorizontal").toReal(),
                                        item->property("_touchMarginVertical").toReal());
            if (!planStyleProjectedPosition(map, coordinate, item->mapToScene(anchor))) {
                return planStyleProjectionContext(map, item, coordinate, anchor);
            }
        }
    }
    for (const auto& name :
         {QStringLiteral("planWaypoint"), QStringLiteral("planHomeMarker"), QStringLiteral("planRallyPoint"),
          QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline")}) {
        for (auto* item : planStyleItems(map, name)) {
            const auto mismatch = projected(item);
            if (!mismatch.isEmpty()) {
                return mismatch;
            }
        }
    }
    return {};
}

bool planStyleEditorProjected(QQuickItem* map)
{
    return planStyleRenderMap(map) && planStyleEditorProjectionMismatch(map).isEmpty();
}
}  // namespace

void PlanStyleUITest::init()
{
    initQuickStyle();
    _savedMapPosition = QGroundControlQmlGlobal::flightMapPosition();
    _savedMapZoom = QGroundControlQmlGlobal::flightMapZoom();
    _savedAdvancedUI = QGCCorePlugin::instance()->showAdvancedUI();
    _settingsSnapshot.clear();
    _factSnapshot.clear();
    QSettings stored;
    for (const auto& key : stored.allKeys()) {
        _settingsSnapshot.insert(key, stored.value(key));
    }
    auto* manager = SettingsManager::instance();
    auto* settings = manager->appSettings();
    // Materialize lazily created Facts that the UI, connection and UnitTest::init paths can change.
    const QList<Fact*> touchedFacts{settings->savePath(),
                                    settings->audioMuted(),
                                    settings->offlineEditingFirmwareClass(),
                                    settings->offlineEditingVehicleClass(),
                                    settings->firstRunPromptIdsShown(),
                                    manager->firmwareUpgradeSettings()->defaultFirmwareType(),
                                    manager->planViewSettings()->useNewPlanPanel(),
                                    manager->planViewSettings()->useGeoMapEngine(),
                                    manager->planViewSettings()->showMissionItemStatus()};
    for (auto* fact : manager->findChildren<SettingsFact*>()) {
        _factSnapshot.append({fact, fact->rawValue()});
    }
    for (auto* fact : touchedFacts) {
        QVERIFY(fact);
    }
    UnitTest::init();
    QVERIFY(_directory.isValid());
    QDir directory(_directory.path());
    QVERIFY(directory.removeRecursively());
    QVERIFY(directory.mkpath(QStringLiteral(".")));
    QSettings().remove(QStringLiteral("COP"));
    settings->savePath()->setRawValue(_directory.path());
    settings->audioMuted()->setRawValue(true);
}

void PlanStyleUITest::cleanup()
{
    // Restore after engine destruction and the base cleanup's event drain, including failed rows.
    const auto restoreSettings = qScopeGuard([this] {
        for (const auto& saved : _factSnapshot) {
            if (saved.first) {
                saved.first->setRawValue(saved.second);
            }
        }
        QGroundControlQmlGlobal::setFlightMapViewForTest(_savedMapPosition, _savedMapZoom);
        QGCCorePlugin::instance()->setProperty("showAdvancedUI", _savedAdvancedUI);
        QSettings stored;
        stored.clear();
        for (auto it = _settingsSnapshot.cbegin(); it != _settingsSnapshot.cend(); ++it) {
            stored.setValue(it.key(), it.value());
        }
        stored.sync();
        _factSnapshot.clear();
        _settingsSnapshot.clear();
    });
    if (auto* controller = _controller()) {
        controller->cancelControl();
    }
    for (const auto& link : _links) {
        if (link) {
            link->disconnect();
        }
    }
    const bool removed = waitForCondition([] { return MultiVehicleManager::instance()->vehicles()->count() == 0; },
                                          TestTimeout::longMs(), QStringLiteral("plan style vehicles removed"));
    const bool inactive = waitForCondition([] { return MultiVehicleManager::instance()->activeVehicle() == nullptr; },
                                           TestTimeout::longMs(), QStringLiteral("plan style active vehicle cleared"));
    _links.clear();
    _configs.clear();
    QmlUITestBase::cleanup();
    QVERIFY(removed);
    QVERIFY(inactive);
}

COPController* PlanStyleUITest::_controller() const
{
    return _engine ? _engine->singletonInstance<COPController*>("QGC", "COPController") : nullptr;
}

COPVehicle* PlanStyleUITest::_entryFor(quint32 sysid) const
{
    auto* controller = _controller();
    if (controller) {
        for (int i = 0; i < controller->vehicles()->count(); ++i) {
            auto* entry = controller->vehicles()->value<COPVehicle*>(i);
            if (entry && entry->sysid() == sysid) {
                return entry;
            }
        }
    }
    return nullptr;
}

MockLink* PlanStyleUITest::_start(MAV_TYPE type)
{
    if (_links.isEmpty()) {
        auto* manager = MultiVehicleManager::instance();
        connect(manager, &MultiVehicleManager::vehicleAdded, _window, [this, manager](Vehicle* vehicle) {
            if (vehicle && manager->vehicles()->count() > 1) {
                expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle %1").arg(vehicle->id())));
                QTimer::singleShot(0, _window, [this] {
                    QVERIFY(acceptDialog());
                    verifyExpectedLogMessage();
                });
            }
        });
    }
    auto config = std::make_shared<MockConfiguration>(QStringLiteral("Plan style %1").arg(_configs.size()));
    config->setDynamic(true);
    config->setFirmwareType(MAV_AUTOPILOT_ARDUPILOTMEGA);
    config->setVehicleType(type);
    config->setIncrementVehicleId(true);
    SharedLinkConfigurationPtr shared = config;
    _configs.append(shared);
    if (!LinkManager::instance()->createConnectedLink(shared)) {
        return nullptr;
    }
    auto* link = qobject_cast<MockLink*>(config->link());
    _links.append(link);
    return link;
}

bool PlanStyleUITest::_selectTab(quint32 sysid)
{
    auto* entry = _entryFor(sysid);
    if (!entry) {
        return false;
    }
    const auto label = entry->label();
    _clickStableItem(label, [&] { return planStyleTab(_rootItem, label); });
    return !QTest::currentTestFailed();
}

void PlanStyleUITest::_flyPlans_data()
{
    QTest::addColumn<bool>("grouped");
    QTest::newRow("separated-waypoints") << false;
    QTest::newRow("grouped-waypoints") << true;
}

void PlanStyleUITest::_flyPlans()
{
    QFETCH(bool, grouped);
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* controller = _controller();
    QVERIFY(controller);
    auto* policy = QGCCorePlugin::instance()->planStyle();
    QVERIFY(policy);
    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> first = _start(MAV_TYPE_GROUND_ROVER);
    QPointer<MockLink> second = _start(MAV_TYPE_QUADROTOR);
    QVERIFY(first && second);
    QVERIFY(first->vehicleId() != 0);
    QVERIFY(second->vehicleId() != 0);
    QVERIFY(first->vehicleId() != second->vehicleId());
    auto* manager = MultiVehicleManager::instance();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    QList<Vehicle*> vehicles;
    QList<QList<QGeoCoordinate>> uploaded;
    for (const auto& link : {first, second}) {
        QTRY_VERIFY_WITH_TIMEOUT(_entryFor(link->vehicleId()) && _entryFor(link->vehicleId())->vehicle(),
                                 TestTimeout::longMs());
        auto* vehicle = _entryFor(link->vehicleId())->vehicle();
        vehicles.append(vehicle);
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->isInitialConnectComplete(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->missionManager()->inProgress(), TestTimeout::longMs());
        QList<MissionItem*> items;
        QList<QGeoCoordinate> coordinates;
        const int count = vehicle == vehicles.first() ? 4 : 6;
        for (int i = 0; i < count; ++i) {
            const int step = grouped && i == 2 ? 1 : i;
            const auto coordinate = vehicle->homePosition().atDistanceAndAzimuth(300 * step, count == 4 ? 90 : 180);
            coordinates.append(coordinate);
            items.append(new MissionItem(i, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, 0,
                                         coordinate.latitude(), coordinate.longitude(), 100, true, false, this));
        }
        uploaded.append(coordinates);
        QSignalSpy sent(vehicle->missionManager(), &PlanManager::sendComplete);
        QVERIFY(sent.isValid());
        vehicle->missionManager()->writeMissionItems(items);
        QTRY_VERIFY_WITH_TIMEOUT(sent.count() >= 1, TestTimeout::longMs());
        QVERIFY(!sent.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->missionManager()->inProgress(), TestTimeout::longMs());
        QmlObjectListModel polygons;
        QmlObjectListModel circles;
        for (bool inclusion : {true, false}) {
            auto* polygon = new QGCFencePolygon(inclusion);
            for (double azimuth : {0.0, 120.0, 240.0}) {
                polygon->appendVertex(vehicle->homePosition().atDistanceAndAzimuth(
                    inclusion ? 400 + count * 10 : 70 + count * 10, azimuth));
            }
            polygons.append(polygon);
            circles.append(
                new QGCFenceCircle(vehicle->homePosition().atDistanceAndAzimuth(count * 50, inclusion ? 90 : 180),
                                   inclusion ? 90 : 30, inclusion));
        }
        QSignalSpy fenceSent(vehicle->geoFenceManager(), &GeoFenceManager::sendComplete);
        QVERIFY(fenceSent.isValid());
        vehicle->geoFenceManager()->sendToVehicle(vehicle->homePosition(), polygons, circles);
        polygons.clearAndDeleteContents();
        circles.clearAndDeleteContents();
        QTRY_VERIFY_WITH_TIMEOUT(!fenceSent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!fenceSent.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->geoFenceManager()->inProgress(), TestTimeout::longMs());
        QList<RallyPointData> rallyPoints;
        for (int i = 0; i < count - 1; ++i) {
            auto coordinate = vehicle->homePosition().atDistanceAndAzimuth(100 + i * 60, count == 4 ? 45 : 225);
            coordinate.setAltitude(80 + i * 10);
            rallyPoints.append({coordinate, MAV_FRAME_GLOBAL_RELATIVE_ALT});
        }
        QSignalSpy rallySent(vehicle->rallyPointManager(), &RallyPointManager::sendComplete);
        QVERIFY(rallySent.isValid());
        vehicle->rallyPointManager()->sendToVehicle(rallyPoints);
        QTRY_VERIFY_WITH_TIMEOUT(!rallySent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!rallySent.first().first().toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicle->rallyPointManager()->inProgress(), TestTimeout::longMs());
        auto* parameters = vehicle->parameterManager();
        QVERIFY(parameters);
        if (parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))) {
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS"))
                ->setRawValue(500 + count);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE"))
                ->setRawValue(2);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))
                ->setRawValue(1);
        }
    }
    QVERIFY(_entryFor(vehicles[0]->id())->color() != _entryFor(vehicles[1]->id())->color());
    const auto loaders = [this] { return planStyleItems(_rootItem, QStringLiteral("flyViewVehiclePlan")); };
    const auto loaderFor = [&](Vehicle* vehicle) -> QQuickItem* {
        for (auto* loader : loaders()) {
            if (qvariant_cast<QObject*>(loader->parentItem()->property("_vehicle")) == vehicle) {
                return loader;
            }
        }
        return nullptr;
    };
    const auto visualsFor = [&](Vehicle* vehicle) -> QObject* {
        auto* loader = loaderFor(vehicle);
        return loader ? qvariant_cast<QObject*>(loader->property("item")) : nullptr;
    };
    QTRY_COMPARE_WITH_TIMEOUT(loaders().size(), 2, TestTimeout::longMs());
    QList<QPointer<PlanMasterController>> controllers(2);
    QList<QPointer<QmlObjectListModel>> models(2);
    QList<QPointer<QmlObjectListModel>> segmentModels(2);
    QList<int> downloads(2);
    // Capture each retained controller only after its uploaded plan has reached the Fly model.
    for (int v = 0; v < 2; ++v) {
        auto* entry = _entryFor(vehicles[v]->id());
        QVERIFY(entry);
        QCOMPARE(entry->vehicle(), vehicles[v]);
        QVERIFY(entry->connected());
        controller->selectVehicle(vehicles[v]->id());
        QCOMPARE(controller->selected(), entry);
        controller->assumeControl();
        QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), vehicles[v], TestTimeout::longMs());
        QVERIFY(_selectTab(vehicles[v]->id()));
        QTRY_VERIFY_WITH_TIMEOUT(visualsFor(vehicles[v]), TestTimeout::longMs());
        auto* master = qvariant_cast<PlanMasterController*>(visualsFor(vehicles[v])->property("planMasterController"));
        QVERIFY(master);
        controllers[v] = master;
        QTRY_COMPARE_WITH_TIMEOUT(master->missionController()->visualItems()->count(), uploaded[v].size(),
                                  TestTimeout::longMs());
        models[v] = master->missionController()->visualItems();
        segmentModels[v] = master->missionController()->simpleFlightPathSegments();
        QTRY_VERIFY_WITH_TIMEOUT(segmentModels[v]->count() > 0, TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(!vehicles[v]->missionManager()->inProgress(), TestTimeout::longMs());
        downloads[v] = _links[v]->receivedMissionRequestListCount(MAV_MISSION_TYPE_MISSION);
    }
    QPointer<PlanMasterController> retainedFenceMaster;
    QPointer<QmlObjectListModel> retainedFencePolygons;
    QPointer<QmlObjectListModel> retainedFenceCircles;
    QPointer<QmlObjectListModel> retainedRallyPoints;
    const QList<int> fenceDownloads{first->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE),
                                    second->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE)};
    const QList<int> rallyDownloads{first->receivedMissionRequestListCount(MAV_MISSION_TYPE_RALLY),
                                    second->receivedMissionRequestListCount(MAV_MISSION_TYPE_RALLY)};
    for (int v : {0, 1, 0}) {
        auto* vehicle = vehicles[v];
        auto* entry = _entryFor(vehicle->id());
        QVERIFY(entry);
        QCOMPARE(entry->vehicle(), vehicle);
        QVERIFY(entry->connected());
        controller->selectVehicle(vehicle->id());
        QCOMPARE(controller->selected(), entry);
        controller->assumeControl();
        QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), vehicle, TestTimeout::longMs());
        QVERIFY(_selectTab(vehicle->id()));
        QTRY_VERIFY_WITH_TIMEOUT(visualsFor(vehicle), TestTimeout::longMs());
        QVERIFY(!loaderFor(vehicles[1 - v])->property("active").toBool());
        QVERIFY(!visualsFor(vehicles[1 - v]));
        QCOMPARE(qvariant_cast<PlanMasterController*>(visualsFor(vehicle)->property("planMasterController")),
                 controllers[v].data());
        for (int retained = 0; retained < 2; ++retained) {
            QVERIFY(controllers[retained]);
            QCOMPARE(controllers[retained]->missionController()->visualItems(), models[retained].data());
            QCOMPARE(controllers[retained]->missionController()->simpleFlightPathSegments(),
                     segmentModels[retained].data());
            QCOMPARE(_links[retained]->receivedMissionRequestListCount(MAV_MISSION_TYPE_MISSION), downloads[retained]);
        }
        auto* map = qvariant_cast<QQuickItem*>(visualsFor(vehicle)->property("map"));
        QVERIFY(map);
        QTRY_VERIFY_WITH_TIMEOUT(map->isVisible() && map->property("mapReady").toBool(), TestTimeout::longMs());
        const QColor expected = _entryFor(vehicle->id())->color();
        const auto routes = [&] { return planStyleItems(map, QStringLiteral("planMissionRoute")); };
        const auto markers = [&] { return planStyleItems(map, QStringLiteral("planWaypoint")); };
        const auto homes = [&] { return planStyleItems(map, QStringLiteral("planHomeMarker")); };
        QTRY_COMPARE_WITH_TIMEOUT(routes().size(), segmentModels[v]->count(), TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(markers().size(), uploaded[v].size() - 1, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(homes().size(), 1, TestTimeout::longMs());
        auto* fenceMaster = qvariant_cast<PlanMasterController*>(map->property("_planMasterController"));
        QVERIFY(fenceMaster);
        QCOMPARE(fenceMaster->managerVehicle(), vehicle);
        if (!retainedFenceMaster) {
            retainedFenceMaster = fenceMaster;
            retainedFencePolygons = fenceMaster->geoFenceController()->polygons();
            retainedFenceCircles = fenceMaster->geoFenceController()->circles();
            retainedRallyPoints = fenceMaster->rallyPointController()->points();
        }
        QCOMPARE(fenceMaster, retainedFenceMaster.data());
        QCOMPARE(fenceMaster->geoFenceController()->polygons(), retainedFencePolygons.data());
        QCOMPARE(fenceMaster->geoFenceController()->circles(), retainedFenceCircles.data());
        QCOMPARE(fenceMaster->rallyPointController()->points(), retainedRallyPoints.data());
        const auto fenceContexts = map->findChildren<QObject*>(QStringLiteral("flyFencePlanStyleContext"));
        QCOMPARE(fenceContexts.size(), 1);
        QCOMPARE(fenceContexts.first()->property("vehicleId").toUInt(), vehicle->id());
        for (int retained = 0; retained < 2; ++retained) {
            QCOMPARE(_links[retained]->receivedMissionRequestListCount(MAV_MISSION_TYPE_FENCE),
                     fenceDownloads[retained]);
            QCOMPARE(_links[retained]->receivedMissionRequestListCount(MAV_MISSION_TYPE_RALLY),
                     rallyDownloads[retained]);
        }

        QTRY_COMPARE_WITH_TIMEOUT(fenceMaster->geoFenceController()->polygons()->count(), 2, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(fenceMaster->geoFenceController()->circles()->count(), 2, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(fenceMaster->rallyPointController()->points()->count(), uploaded[v].size() - 1,
                                  TestTimeout::longMs());
        QCOMPARE(vehicle->geoFenceManager()->polygons().size(), 2);
        QCOMPARE(vehicle->geoFenceManager()->circles().size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(
            fenceMaster->geoFenceController()->polygons()->count() == 2 &&
                fenceMaster->geoFenceController()->circles()->count() == 2 &&
                fenceMaster->geoFenceController()->polygons()->value<QGCFencePolygon*>(0)->path() ==
                    vehicle->geoFenceManager()->polygons()[0].path() &&
                fenceMaster->geoFenceController()->circles()->value<QGCFenceCircle*>(0)->center() ==
                    vehicle->geoFenceManager()->circles()[0].center(),
            TestTimeout::longMs());
        for (int i = 0; i < 2; ++i) {
            auto* polygon = fenceMaster->geoFenceController()->polygons()->value<QGCFencePolygon*>(i);
            auto* circle = fenceMaster->geoFenceController()->circles()->value<QGCFenceCircle*>(i);
            QVERIFY(polygon && circle);
            QCOMPARE(polygon->path(), vehicle->geoFenceManager()->polygons()[i].path());
            QCOMPARE(polygon->inclusion(), vehicle->geoFenceManager()->polygons()[i].inclusion());
            QCOMPARE(circle->center(), vehicle->geoFenceManager()->circles()[i].center());
            QGCFenceCircle uploadedCircle(vehicle->geoFenceManager()->circles()[i]);
            QCOMPARE(circle->radius()->rawValue(), uploadedCircle.radius()->rawValue());
        }
        const auto fenceFills = [&] { return planStyleItems(map, QStringLiteral("planFenceFill")); };
        QTRY_COMPARE_WITH_TIMEOUT(fenceFills().size(), 5, TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleItems(map, QStringLiteral("planKeepInOutline")).size(), 3,
                                  TestTimeout::longMs());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleItems(map, QStringLiteral("planKeepOutOutline")).size(), 2,
                                  TestTimeout::longMs());
        planStyleCheckBoundaries(map, vehicle->id(), expected, 2, 2);
        QVERIFY(!QTest::currentTestFailed());
        for (auto* fill : fenceFills()) {
            QCOMPARE(fill->opacity(), 1.0);
            if (fill->property("boundaryKind").toString() == QStringLiteral("polygon")) {
                bool found = false;
                for (int i = 0; i < 2; ++i) {
                    const auto* polygon = fenceMaster->geoFenceController()->polygons()->value<QGCFencePolygon*>(i);
                    found |= fill->property("path").toList() == polygon->path();
                }
                QVERIFY(found);
            }
        }
        const auto rallies = [&] { return planStyleItems(map, QStringLiteral("planRallyPoint")); };
        QTRY_COMPARE_WITH_TIMEOUT(rallies().size(), uploaded[v].size() - 1, TestTimeout::longMs());
        auto* rallyController = fenceMaster->rallyPointController();
        const auto vehicleRallies = vehicle->rallyPointManager()->points();
        QCOMPARE(vehicleRallies.size(), rallyController->points()->count());
        for (int i = 0; i < vehicleRallies.size(); ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(rallyController->points()->value<RallyPoint*>(i)->coordinate(),
                                      vehicleRallies[i].coordinate, TestTimeout::longMs());
        }
        rallyController->setCurrentRallyPoint(rallyController->points()->get(1));
        int selectedRallies = 0;
        for (auto* marker : rallies()) {
            planStyleCheckRally(map, marker, vehicle->id(), expected);
            QVERIFY(!QTest::currentTestFailed());
            const auto coordinate = marker->property("coordinate").value<QGeoCoordinate>();
            bool found = false;
            for (int i = 0; i < rallyController->points()->count(); ++i) {
                found |= coordinate == rallyController->points()->value<RallyPoint*>(i)->coordinate();
            }
            QVERIFY(found);
            selectedRallies += marker->property("checked").toBool() ? 1 : 0;
        }
        QCOMPARE(selectedRallies, 1);
        const auto breach = planStyleItems(map, QStringLiteral("planBreachReturnPoint"));
        QCOMPARE(breach.size(), 1);
        QCOMPARE(breach.first()->property("vehicleId").toUInt(), vehicle->id());
        QCOMPARE(breach.first()->property("label").toString(), QStringLiteral("B"));
        QCOMPARE(breach.first()->property("markerColor").value<QColor>(), expected);
        QCOMPARE(breach.first()->property("markerBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
        QCOMPARE(breach.first()->property("markerBorderWidth").toReal(), 2.0);
        QCOMPARE(breach.first()->property("markerTextColor").value<QColor>(), QColor(QStringLiteral("black")));

        // Browsing a different tab does not change the owner of the active vehicle's retained plan.
        const auto firstRoute = QPointer<QQuickItem>(routes().first());
        auto* browsedEntry = _entryFor(vehicles[1 - v]->id());
        QVERIFY(browsedEntry);
        controller->selectVehicle(vehicles[1 - v]->id());
        QCOMPARE(controller->selected(), browsedEntry);
        QCOMPARE(manager->activeVehicle(), vehicle);
        const auto contexts = loaderFor(vehicle)->parentItem()->findChildren<QObject*>(
            QStringLiteral("flyPlanStyleContext"), Qt::FindDirectChildrenOnly);
        QCOMPARE(contexts.size(), 1);
        QCOMPARE(contexts.first()->property("vehicleId").toUInt(), vehicle->id());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(firstRoute, QStringLiteral("line.color")), expected,
                                  TestTimeout::shortMs());
        for (auto* outline : planStyleItems(map, QStringLiteral("planKeepInOutline")) +
                                 planStyleItems(map, QStringLiteral("planKeepOutOutline"))) {
            QCOMPARE(outline->property("vehicleId").toUInt(), vehicle->id());
            QCOMPARE(outline->property("renderedStrokeColor").value<QColor>(), expected);
        }
        for (auto* marker : rallies()) {
            QCOMPARE(marker->property("markerColor").value<QColor>(), expected);
            QCOMPARE(marker->property("vehicleId").toUInt(), vehicle->id());
        }
        QVERIFY(_entryFor(vehicle->id()) == entry);
        controller->selectVehicle(vehicle->id());
        QCOMPARE(controller->selected(), entry);
        QVERIFY(_selectTab(vehicle->id()));
        QVERIFY(firstRoute);
        QCOMPARE(routes().first(), firstRoute.data());
        QList<bool> matched(segmentModels[v]->count(), false);
        for (auto* route : routes()) {
            QCOMPARE(route->property("vehicleId").toUInt(), vehicle->id());
            QCOMPARE(route->property("layerKind").toString(), QStringLiteral("route"));
            QVERIFY(route->isVisible());
            QCOMPARE(planStyleColor(route, QStringLiteral("line.color")), expected);
            QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 4.0);
            QCOMPARE(route->property("renderedColor").value<QColor>(), expected);
            QCOMPARE(route->property("renderedWidth").toReal(), 4.0);
            // Native MapPolyline is solid; all route objects must be native polylines with exactly this path.
            const auto path = route->property("path").toList();
            QCOMPARE(path.size(), 2);
            bool found = false;
            for (int i = 0; i < segmentModels[v]->count(); ++i) {
                auto* segment = segmentModels[v]->value<FlightPathSegment*>(i);
                if (!matched[i] && path.first().value<QGeoCoordinate>() == segment->coordinate1() &&
                    path.last().value<QGeoCoordinate>() == segment->coordinate2()) {
                    matched[i] = true;
                    found = true;
                    break;
                }
            }
            QVERIFY(found);
        }
        QQuickItem* current = nullptr;
        QQuickItem* ordinary = nullptr;
        const int currentSeq = 1;
        const int firmwareSeq = currentSeq - (vehicle->firmwarePlugin()->sendHomePositionToVehicle() ? 0 : 1);
        emit vehicle->missionManager()->currentIndexChanged(firmwareSeq);
        QSet<int> sequences;
        for (auto* marker : markers()) {
            QCOMPARE(marker->property("vehicleId").toUInt(), vehicle->id());
            QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), expected);
            QCOMPARE(marker->property("renderedTextColor").value<QColor>(), policy->markerTextColor());
            QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), policy->markerBorderColor());
            QCOMPARE(marker->property("renderedBorderWidth").toReal(), policy->markerBorderWidth());
            const int sequence = marker->property("sequenceNumber").toInt();
            QVERIFY(sequence > 0 && sequence < uploaded[v].size());
            QVERIFY(!sequences.contains(sequence));
            sequences.insert(sequence);
            auto coordinate = marker->property("coordinate").value<QGeoCoordinate>();
            QVERIFY(coordinate.distanceTo(uploaded[v][sequence]) < 0.05);
            QCOMPARE(marker->property("index").toInt(), sequence);
            QCOMPARE(marker->property("renderedIndexText").toString(), QString::number(sequence));
            QCOMPARE(marker->property("checked").toBool(), sequence == currentSeq);
            QCOMPARE(marker->property("current").toBool(), sequence == currentSeq);
            if (sequence == currentSeq) {
                current = marker;
            } else if (marker->isVisible() && !marker->property("grouped").toBool()) {
                ordinary = marker;
            }
        }
        QVERIFY(current && ordinary);
        auto* currentLabel = planStyleSource(current);
        auto* ordinaryLabel = planStyleSource(ordinary);
        QVERIFY(currentLabel && ordinaryLabel);
        auto* currentBody = qvariant_cast<QQuickItem*>(currentLabel->property("renderedIndicator"));
        auto* ordinaryBody = qvariant_cast<QQuickItem*>(ordinaryLabel->property("renderedIndicator"));
        QVERIFY(currentBody && ordinaryBody);
        QVERIFY(currentBody->width() > ordinaryBody->width());
        const auto halos = planStyleItems(currentLabel, QStringLiteral("planWaypointSelectionHalo"));
        QCOMPARE(halos.size(), 1);
        QVERIFY(halos.first()->isVisible());
        QCOMPARE(halos.first()->width(), currentBody->width() * 2);
        QCOMPARE(planStyleColor(halos.first(), QStringLiteral("border.color")), QColor::fromRgbF(1, 1, 1, 0.5));
        auto* home = homes().first();
        QCOMPARE(home->property("vehicleId").toUInt(), vehicle->id());
        QVERIFY(home->isVisible());
        QCOMPARE(home->property("coordinate").value<QGeoCoordinate>(),
                 controllers[v]->missionController()->plannedHomePosition());
        auto* house = planStyleSource(home);
        QVERIFY(house);
        QVERIFY(house->inherits("QQuickImage"));
        QCOMPARE(house->property("source").toUrl(), QUrl(QStringLiteral("qrc:///qmlimages/MapHome.svg")));
        const auto accents = planStyleItems(home, QStringLiteral("planHomeAccent"));
        QCOMPARE(accents.size(), 1);
        QVERIFY(accents.first()->isVisible());
        QCOMPARE(planStyleColor(accents.first(), QStringLiteral("border.color")), expected);
        QCOMPARE(accents.first()->property("color").value<QColor>(), QColor(QStringLiteral("transparent")));
        QCOMPARE(home->property("renderedBorderColor").value<QColor>(), expected);
        auto* tracker = qvariant_cast<QObject*>(map->property("positionTracker"));
        QVERIFY(tracker);
        QVERIFY(tracker->setProperty("userInteracting", true));
        // Pausing follow does not stop a recenter animation that is already running.
        QTRY_VERIFY_WITH_TIMEOUT(tracker->property("firstVehiclePositionReceived").toBool(), TestTimeout::longMs());
        QTRY_VERIFY_WITH_TIMEOUT(!tracker->property("animating").toBool(), TestTimeout::mediumMs());
        QVERIFY(map->setProperty("zoomLevel", 17));
        QVERIFY(map->setProperty("center", current->property("coordinate")));
        const auto clickTargetReady = [&] {
            map->ensurePolished();
            current->ensurePolished();
            currentLabel->ensurePolished();
            currentBody->ensurePolished();
            const QPointF sceneCenter =
                currentBody->mapToScene(QPointF(currentBody->width() / 2, currentBody->height() / 2));
            const QPointF mapCenter = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
            return currentBody->window() == _window && current->isVisible() && currentBody->isVisible() &&
                   currentBody->width() > 0 && currentBody->height() > 0 && sceneCenter.x() >= 0 &&
                   sceneCenter.x() < _window->width() && sceneCenter.y() >= 0 && sceneCenter.y() < _window->height() &&
                   (sceneCenter - mapCenter).manhattanLength() < 2;
        };
        QTRY_VERIFY_WITH_TIMEOUT(clickTargetReady(), TestTimeout::mediumMs());
        if (grouped) {
            QTRY_VERIFY_WITH_TIMEOUT(current->property("grouped").toBool(), TestTimeout::shortMs());
            QCOMPARE(currentLabel->property("indicatorSubText").toString(), QStringLiteral("…"));
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(!current->property("grouped").toBool(), TestTimeout::shortMs());
        }
        QVERIFY(_clickItemAt(currentBody, 0.5, 0.5, QStringLiteral("current waypoint")));
        if (grouped) {
            QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("missionItemSelection_2"), 0),
                                     TestTimeout::shortMs());
            auto* group = qvariant_cast<QObject*>(current->property("indicatorGroup"));
            QVERIFY(group);
            QSignalSpy selected(group, SIGNAL(itemSelected(int)));
            QVERIFY(selected.isValid());
            QVERIFY(clickItemFraction(QStringLiteral("missionItemSelection_2"), 0.5, 0.5));
            QTRY_COMPARE_WITH_TIMEOUT(selected.count(), 1, TestTimeout::shortMs());
            QCOMPARE(selected.first().first().toInt(), 2);
        }
        QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), 0),
                                 TestTimeout::shortMs());
        auto* confirmation = findItem(_rootItem, QStringLiteral("guidedActionConfirmButton"));
        while (confirmation && !confirmation->property("actionData").isValid()) {
            confirmation = confirmation->parentItem();
        }
        QVERIFY(confirmation);
        QVERIFY(confirmation->property("pending").toBool());
        auto* guidedController = qvariant_cast<QObject*>(confirmation->property("guidedController"));
        QVERIFY(guidedController);
        QCOMPARE(confirmation->property("action").toInt(), guidedController->property("actionSetWaypoint").toInt());
        QCOMPARE(confirmation->property("actionData").toInt(), grouped ? 2 : currentSeq);
        QCOMPARE(qvariant_cast<QObject*>(confirmation->property("vehicle")), static_cast<QObject*>(vehicle));
        QVERIFY(QMetaObject::invokeMethod(confirmation, "confirmCancelled", Q_ARG(QVariant, QVariant())));
        QVERIFY(!confirmation->property("pending").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!findVisibleItem(_rootItem, QStringLiteral("guidedActionConfirmButton"), 0),
                                 TestTimeout::shortMs());
        QVERIFY(tracker->setProperty("userInteracting", false));
        // A real path segment computes collision from a deterministic terrain profile and altitude changes.
        auto* segment = segmentModels[v]->value<FlightPathSegment*>(0);
        QVERIFY(segment);
        QVERIFY(segment->totalDistance() > 0.0);
        QVERIFY(segment->setProperty("distanceBetween", segment->totalDistance()));
        QVERIFY(segment->setProperty("finalDistanceBetween", segment->totalDistance()));
        QVERIFY(segment->setProperty("amslTerrainHeights", QVariantList{1.0e6, 1.0e6}));
        segment->setCoord1AMSLAlt(-100);
        segment->setCoord2AMSLAlt(-100);
        QVERIFY(segment->terrainCollision());
        const auto collided = [&]() -> QQuickItem* {
            for (auto* route : routes()) {
                const auto path = route->property("path").toList();
                if (!path.isEmpty() && path.first().value<QGeoCoordinate>() == segment->coordinate1() &&
                    path.last().value<QGeoCoordinate>() == segment->coordinate2()) {
                    return route;
                }
            }
            return nullptr;
        };
        QVERIFY(collided());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(collided(), QStringLiteral("line.color")),
                                  QColor(QStringLiteral("red")), TestTimeout::shortMs());
        QVERIFY(collided()->property("terrainCollision").toBool());
        segment->setCoord1AMSLAlt(2.0e6);
        segment->setCoord2AMSLAlt(2.0e6);
        QVERIFY(!segment->terrainCollision());
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(collided(), QStringLiteral("line.color")), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(_links[v]->receivedMissionRequestListCount(MAV_MISSION_TYPE_MISSION), downloads[v]);
    }
}

void PlanStyleUITest::_perimeterVisuals_data()
{
    QTest::addColumn<int>("contextMode");
    QTest::addColumn<bool>("px4");
    for (int mode : {0, 1, 2}) {
        for (bool px4 : {false, true}) {
            const auto name =
                QStringLiteral("perimeter-%1-%2").arg(mode).arg(px4 ? QStringLiteral("px4") : QStringLiteral("apm"));
            QTest::newRow(qPrintable(name)) << mode << px4;
        }
    }
}

void PlanStyleUITest::_perimeterVisuals()
{
    QFETCH(int, contextMode);
    QFETCH(bool, px4);
    const bool styled = contextMode == 2;
    const auto firmware = px4 ? MAV_AUTOPILOT_PX4 : MAV_AUTOPILOT_ARDUPILOTMEGA;
    const auto restore = PlanStylePatternTest::planStyleOfflineVehicleSettings(firmware, MAV_TYPE_QUADROTOR);
    PlanMasterController master(firmware, MAV_TYPE_QUADROTOR);
    master.setFlyView(true);
    master.start();
    QVERIFY(master.controllerVehicle());
    QCOMPARE(master.controllerVehicle()->firmwareType(), firmware);
    QCOMPARE(master.controllerVehicle()->vehicleType(), MAV_TYPE_QUADROTOR);
    PerimeterScanComplexItem perimeter(&master, true);
    perimeter.setSequenceNumber(10);
    perimeter.perimeterPolygon()->appendVertices(PlanStylePatternTest::square(QGeoCoordinate(47, 8, 100)));
    QmlObjectListModel items;
    items.append(&perimeter);
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    planStyleWarmUpScreenTools(engine.get());
    engine->rootContext()->setContextProperty(QStringLiteral("testPerimeters"), &items);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtQuick.Shapes
        import QtPositioning
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        FlightMap {
            id: perimeterMap
            width: 640; height: 480
            center: QtPositioning.coordinate(47, 8)
            zoomLevel: 16
            planView: false
            isSatelliteMap: false
            allowGCSLocationCenter: false
            allowVehicleLocationCenter: false
            property rect centerViewport: Qt.rect(0, 0, width, height)
            property int contextMode: 0
            readonly property int dashStyle: ShapePath.DashLine
            QtObject {
                id: style
                property bool enabled: perimeterMap.contextMode === 2
                property real vehicleId: 4294967295
                property color lineColor: "magenta"
                property real missionLineWidth: enabled ? 4 : 19
                property real boundaryLineWidth: enabled ? 3 : 17
                property var keepInDashPattern: enabled ? [6, 3] : [1, 19]
                property color markerBorderColor: enabled ? "white" : "cyan"
                property real markerBorderWidth: enabled ? 2 : 7
                property color markerTextColor: enabled ? "black" : "yellow"
            }
            Repeater {
                model: testPerimeters
                MissionItemMapVisual {
                    map: perimeterMap; vehicle: null; interactive: false; opacity: 0.5
                    planStyle: perimeterMap.contextMode === 0 ? null : style
                }
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(
        component.createWithInitialProperties({{QStringLiteral("contextMode"), contextMode}}));
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterFill")).size(), 1, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterTraversal")).size(), 1, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterMarker")).size(), 1, TestTimeout::mediumMs());
    for (int i = 0; i < perimeter.perimeterPolygon()->count(); ++i) {
        const QString name = QStringLiteral("perimeterDirectionArrow_%1").arg(i);
        QTRY_COMPARE_WITH_TIMEOUT(named(name).size(), 1, TestTimeout::mediumMs());
        auto* arrow = named(name).first();
        QCOMPARE(arrow->property("fromCoord").value<QGeoCoordinate>(),
                 perimeter.perimeterPolygon()->vertexCoordinate(i));
        QCOMPARE(arrow->property("toCoord").value<QGeoCoordinate>(),
                 perimeter.perimeterPolygon()->vertexCoordinate((i + 1) % perimeter.perimeterPolygon()->count()));
        QCOMPARE(arrow->property("arrowPosition").toInt(), 2);
    }
    QCOMPARE(named(QStringLiteral("perimeterDirectionArrow_4")).size(), 0);
    auto* fill = named(QStringLiteral("planPerimeterFill")).first();
    auto* traversal = named(QStringLiteral("planPerimeterTraversal")).first();
    auto* marker = named(QStringLiteral("planPerimeterMarker")).first();
    const QColor expected(styled ? QStringLiteral("magenta") : QStringLiteral("#00cc44"));
    QCOMPARE(fill->property("color").value<QColor>(), expected);
    QCOMPARE(fill->opacity(), 0.04);
    QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), styled ? 0.0 : 2.0);
    QCOMPARE(QQmlProperty(fill, QStringLiteral("border.color")).read().value<QColor>(),
             QColor(QStringLiteral("#00cc44")));
    QCOMPARE(QQmlProperty(traversal, QStringLiteral("line.width")).read().toReal(), styled ? 4.0 : 2.0);
    QCOMPARE(planStyleColor(traversal, QStringLiteral("line.color")), expected);
    QCOMPARE(traversal->opacity(), 0.5);
    QCOMPARE(fill->z(), 0.0);
    QCOMPARE(traversal->z(), 0.0);
    QCOMPARE(marker->property("index").toInt(), perimeter.sequenceNumber());
    QCOMPARE(marker->property("renderedIndexText").toString(), QString::number(perimeter.sequenceNumber()));
    QCOMPARE(marker->property("renderedBodyColor").value<QColor>(),
             QColor(styled ? QStringLiteral("magenta") : QStringLiteral("green")));
    QCOMPARE(marker->property("renderedBorderWidth").toReal(), styled ? 2.0 : 0.0);
    QCOMPARE(marker->property("renderedBorderColor").value<QColor>(),
             QColor(styled ? QStringLiteral("white") : QStringLiteral("black")));
    QCOMPARE(marker->property("renderedTextColor").value<QColor>(),
             QColor(styled ? QStringLiteral("black") : QStringLiteral("white")));
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterEntry")).size(), 1, TestTimeout::shortMs());
    auto* markerMapItem = named(QStringLiteral("planPerimeterEntry")).first();
    QVERIFY(markerMapItem);
    QCOMPARE(markerMapItem->property("coordinate").value<QGeoCoordinate>(), perimeter.coordinate());
    QCOMPARE(markerMapItem->opacity(), 0.5);
    QCOMPARE(markerMapItem->z(), 51.0);
    for (bool selected : {false, true}) {
        perimeter.setIsCurrentItem(selected);
        QTRY_COMPARE_WITH_TIMEOUT(traversal->isVisible(), selected, TestTimeout::shortMs());
        QTRY_COMPARE_WITH_TIMEOUT(markerMapItem->isVisible(), selected, TestTimeout::shortMs());
        QVERIFY(fill->isVisible());
        QCOMPARE(fill->property("path").toList(), perimeter.perimeterPolygon()->path());
        auto closed = perimeter.perimeterPolygon()->path();
        closed.append(closed.first());
        QCOMPARE(traversal->property("path").toList(), closed);
        QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPatternOutline")).size(), styled ? 1 : 0,
                                  TestTimeout::shortMs());
        if (styled) {
            auto* outline = named(QStringLiteral("planPatternOutline")).first();
            // Both strokes deliberately follow the same ring: selected traversal fills every dash gap.
            QCOMPARE(outline->property("vertices").toList(), fill->property("path").toList());
            QCOMPARE(outline->property("renderedStrokeColor").value<QColor>(), expected);
            QCOMPARE(outline->property("renderedStrokeWidth").toReal(), 3.0);
            QCOMPARE(outline->property("renderedDashPattern").toList(), QVariantList({6.0, 3.0}));
            QCOMPARE(outline->property("renderedStrokeStyle").toInt(), map->property("dashStyle").toInt());
            QCOMPARE(outline->opacity(), 0.5);
            QCOMPARE(outline->z(), 1.0);
            QCOMPARE(outline->acceptedMouseButtons(), Qt::NoButton);
            auto* source = planStyleSource(outline);
            QVERIFY(source);
            int strokes = 0;
            for (auto* child : source->findChildren<QObject*>()) {
                strokes += child->metaObject()->indexOfProperty("strokeWidth") >= 0 ? 1 : 0;
            }
            QCOMPARE(strokes, 1);
        }
    }
    perimeter.perimeterPolygon()->setShowAltColor(true);
    // Perimeter has no collision alternative; preserve the shared component's transparent alternate fill.
    QTRY_COMPARE_WITH_TIMEOUT(fill->property("color").value<QColor>().alpha(), 0, TestTimeout::shortMs());
    perimeter.perimeterPolygon()->setShowAltColor(false);
    QCOMPARE(fill->property("color").value<QColor>(), expected);
    perimeter.perimeterPolygon()->setVertexDrag(true);
    perimeter.perimeterPolygon()->adjustVertex(
        0, perimeter.perimeterPolygon()->vertexCoordinate(0).atDistanceAndAzimuth(10, 270));
    QTRY_COMPARE_WITH_TIMEOUT(fill->property("path").toList(), perimeter.perimeterPolygon()->path(),
                              TestTimeout::shortMs());
    QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), styled ? 0.0 : 3.0);
    if (styled) {
        QCOMPARE(named(QStringLiteral("planPatternOutline")).first()->property("vertices").toList(),
                 perimeter.perimeterPolygon()->path());
    }
    auto* firstArrow = named(QStringLiteral("perimeterDirectionArrow_0")).first();
    QTRY_COMPARE_WITH_TIMEOUT(firstArrow->property("fromCoord").value<QGeoCoordinate>(),
                              perimeter.perimeterPolygon()->vertexCoordinate(0), TestTimeout::shortMs());
    QCOMPARE(firstArrow->property("toCoord").value<QGeoCoordinate>(),
             perimeter.perimeterPolygon()->vertexCoordinate(1));
    perimeter.perimeterPolygon()->setVertexDrag(false);
    perimeter.perimeterPolygon()->appendVertex(QGeoCoordinate(47.001, 8.001));
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("perimeterDirectionArrow_4")).size(), 1, TestTimeout::shortMs());
    auto* closingArrow = named(QStringLiteral("perimeterDirectionArrow_4")).first();
    QTRY_COMPARE_WITH_TIMEOUT(closingArrow->property("toCoord").value<QGeoCoordinate>(),
                              perimeter.perimeterPolygon()->vertexCoordinate(0), TestTimeout::shortMs());
    perimeter.perimeterPolygon()->removeVertex(4);
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("perimeterDirectionArrow_4")).size(), 0, TestTimeout::shortMs());
    perimeter.perimeterPolygon()->clear();
    QTRY_VERIFY_WITH_TIMEOUT(!markerMapItem->isVisible(), TestTimeout::shortMs());
    QVERIFY(traversal->property("path").toList().isEmpty());
}

void PlanStyleUITest::_liveRegistryAndDynamicVisuals()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto* policy = plugin->planStyle();
    QVERIFY(policy);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    planStyleWarmUpScreenTools(engine.get());
    if (QTest::currentTestFailed()) {
        return;
    }
    auto* registry = engine->singletonInstance<COPController*>("QGC", "COPController");
    auto* roles = engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(registry && roles);
    registry->initialize(roles);
    for (quint32 id : {0U, 7501U, 7502U, 7599U}) {
        QVERIFY(!registry->colorForSysid(id).isValid());
        QVERIFY(roles->roleForSysid(id).isEmpty());
    }
    const QStringList availableRoles = roles->availableRoles();
    QVERIFY(!availableRoles.isEmpty());
    const QString role = availableRoles.first();
    const int initialEntries = registry->vehicles()->count();
    const int initialRoleEntries = roles->roleEntries()->count();
    const auto restoreOfflineSettings =
        PlanStylePatternTest::planStyleOfflineVehicleSettings(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_FIXED_WING);
    PlanMasterController master(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_FIXED_WING);
    master.setFlyView(true);
    master.start();
    QVERIFY(master.controllerVehicle());
    QCOMPARE(master.controllerVehicle()->firmwareType(), MAV_AUTOPILOT_ARDUPILOTMEGA);
    QCOMPARE(master.controllerVehicle()->vehicleType(), MAV_TYPE_FIXED_WING);
    auto* home = master.missionController()->visualItems()->value<MissionSettingsItem*>(0);
    QVERIFY(home);
    const QGeoCoordinate base(47, 8, 100);
    home->setCoordinate(base);
    SimpleMissionItem waypoint(&master, true, false);
    waypoint.setCommand(MAV_CMD_NAV_LOITER_UNLIM);
    waypoint.missionItem().setParam3(80);
    waypoint.setCoordinate(base.atDistanceAndAzimuth(300, 90));
    waypoint.setSequenceNumber(1);
    TakeoffMissionItem takeoff(MAV_CMD_NAV_TAKEOFF, &master, true, home, false);
    takeoff.setCoordinate(base.atDistanceAndAzimuth(100, 90));
    takeoff.setSequenceNumber(2);
    FixedWingLandingComplexItem fixedWing(&master, true);
    fixedWing.setSequenceNumber(3);
    fixedWing.setLandingCoordinate(base.atDistanceAndAzimuth(600, 90));
    fixedWing.setIsCurrentItem(true);
    VTOLLandingComplexItem vtol(&master, true);
    vtol.setSequenceNumber(20);
    vtol.setLandingCoordinate(base.atDistanceAndAzimuth(900, 90));
    vtol.setIsCurrentItem(true);
    SurveyComplexItem survey(&master, false, QString());
    CorridorScanComplexItem corridor(&master, false, QString());
    StructureScanComplexItem structure(&master, false, QString());
    int sequence = 100;
    for (auto* pattern : QList<VisualMissionItem*>{&survey, &corridor, &structure}) {
        PlanStylePatternTest::seed(pattern, base);
        pattern->setSequenceNumber(sequence);
        pattern->setIsCurrentItem(true);
        sequence += 100;
    }
    PerimeterScanComplexItem perimeter(&master, true);
    perimeter.perimeterPolygon()->appendVertices(PlanStylePatternTest::square(base.atDistanceAndAzimuth(1200, 90)));
    perimeter.setSequenceNumber(400);
    perimeter.setIsCurrentItem(true);
    QmlObjectListModel visualItems;
    visualItems.append(home);
    visualItems.append(&waypoint);
    visualItems.append(&takeoff);
    visualItems.append(&fixedWing);
    visualItems.append(&vtol);
    visualItems.append(&survey);
    visualItems.append(&corridor);
    visualItems.append(&structure);
    visualItems.append(&perimeter);
    QmlObjectListModel segments;
    FlightPathSegment segment(FlightPathSegment::SegmentTypeGeneric, base, 100, base.atDistanceAndAzimuth(300, 90), 100,
                              false, nullptr);
    segments.append(&segment);
    engine->rootContext()->setContextProperty(QStringLiteral("testVisualItems"), &visualItems);
    engine->rootContext()->setContextProperty(QStringLiteral("testSegments"), &segments);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        FlightMap {
            id: testMap
            width: 640
            height: 480
            center: QtPositioning.coordinate(47, 8)
            zoomLevel: 16
            allowGCSLocationCenter: false
            allowVehicleLocationCenter: false
            property rect centerViewport: Qt.rect(0, 0, width, height)
            planView: false
            isSatelliteMap: false
            property real styleId: 7501
            property bool useSecondContext: false
            property var style: useSecondContext ? secondStyle : firstStyle
            readonly property color neutral: QGroundControl.globalPalette.mapMissionTrajectory
            PlanStyleContext { id: firstStyle; vehicleId: testMap.styleId }
            PlanStyleContext { id: secondStyle; vehicleId: 7502 }
            MissionLineView { model: testSegments; planStyle: testMap.style }
            Repeater {
                model: testVisualItems
                MissionItemMapVisual {
                    map: testMap; vehicle: null; planStyle: testMap.style
                    interactive: object.sequenceNumber < 100
                    opacity: object.sequenceNumber < 100 ? 1 : 0.5
                }
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QQuickWindow patternWindow;
    patternWindow.resize(800, 600);
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    map->setParentItem(patternWindow.contentItem());
    patternWindow.show();
    QString colourStage = QStringLiteral("fixture startup");
    const auto liveContext = [&] {
        QStringList found;
        for (const auto& name :
             {QStringLiteral("planPatternMarker"), QStringLiteral("planPatternOutline"),
              QStringLiteral("planTransectFull"), QStringLiteral("planPolyline"), QStringLiteral("planPatternFill"),
              QStringLiteral("planPolygonFill"), QStringLiteral("planSimpleLoiterVisual")}) {
            found.append(planStyleLookupContext(map, name, -1));
        }
        return QStringLiteral("live registry %1 styleId=%2; %3")
            .arg(colourStage, map->property("styleId").toString(), found.join(QStringLiteral("; ")));
    };
    QTRY_VERIFY2_WITH_TIMEOUT(patternWindow.isExposed() && map->property("mapReady").toBool(),
                              qPrintable(liveContext()), TestTimeout::longMs());
    auto* firstContext = qvariant_cast<QObject*>(root->property("style"));
    QVERIFY2(firstContext, qPrintable(liveContext()));
    QCOMPARE(firstContext->property("vehicleId").toUInt(), 7501U);
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planMissionRoute")).size()) == (1), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planHomeAccent")).size()) == (1), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    // Simple, takeoff, launch, two FW endpoints and two VTOL endpoints all use the real async dispatch.
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planWaypoint")).size()) == (7), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planPatternLine")).size()) == (2), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planLoiterRing")).size()) == (2), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planSimpleLoiterVisual")).size()) == (1),
                              qPrintable(liveContext()), TestTimeout::mediumMs());
    const auto simpleLoiter = [&]() -> QQuickItem* {
        const auto visuals = named(QStringLiteral("planSimpleLoiterVisual"));
        return visuals.size() == 1 ? visuals.first() : nullptr;
    };
    const auto loiterVisualRoot = [&]() -> QQuickItem* {
        for (auto* item : map->childItems()) {
            if (qvariant_cast<VisualMissionItem*>(item->property("_missionItem")) == &waypoint) {
                return item;
            }
        }
        return nullptr;
    };
    const auto loiterContext = [&] {
        auto* owner = loiterVisualRoot();
        return QStringLiteral("simple loiter: sequence=%1 isSimpleItem=%2 showLoiterRadius=%3 rootInteractive=%4; %5")
            .arg(waypoint.sequenceNumber())
            .arg(waypoint.isSimpleItem())
            .arg(waypoint.showLoiterRadius())
            .arg(owner ? owner->property("interactive").toString() : QStringLiteral("missing root"))
            .arg(planStyleItemChain(simpleLoiter()));
    };
    QTRY_VERIFY2_WITH_TIMEOUT(loiterVisualRoot(), qPrintable(loiterContext()), TestTimeout::shortMs());
    QVERIFY2(
        loiterVisualRoot()->property("interactive").toBool() && waypoint.isSimpleItem() && waypoint.showLoiterRadius(),
        qPrintable(loiterContext()));
    // The source is a zero-sized visual manager. Give this rendered fixture an extent so QtLocation cannot cull it.
    QVERIFY2(simpleLoiter(), qPrintable(loiterContext()));
    simpleLoiter()->setSize(QSizeF(1, 1));
    QVERIFY2(map->setProperty("center", QVariant::fromValue(waypoint.coordinate())), qPrintable(loiterContext()));
    const auto loiterCircle = [&] {
        auto* visuals = simpleLoiter();
        return visuals ? qvariant_cast<QQuickItem*>(visuals->property("_circleComponent")) : nullptr;
    };
    QTRY_VERIFY2_WITH_TIMEOUT(planStyleRenderMap(map) && loiterCircle(), qPrintable(loiterContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planLandingArea")).size()) == (1), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planGlideSlope")).size()) == (1), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    planStyleWaitForVisualBindings(map, &visualItems, firstContext);
    QVERIFY(!QTest::currentTestFailed());
    const QPointer<QQuickItem> landingArea = named(QStringLiteral("planLandingArea")).first();
    const QPointer<QQuickItem> glideSlope = named(QStringLiteral("planGlideSlope")).first();
    QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planPatternMarker")).size()) == (6), qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT(
        (named(QStringLiteral("planPatternOutline")).size()) == (5 + corridor.corridorBoundaries().size()),
        qPrintable(liveContext()), TestTimeout::mediumMs());
    planStyleWaitForVisualBindings(map, &visualItems, firstContext);
    QVERIFY(!QTest::currentTestFailed());
    QList<QPointer<QQuickItem>> retainedPatterns;
    for (const auto& name :
         {QStringLiteral("planPatternMarker"), QStringLiteral("planTransectFull"), QStringLiteral("planTransectEntry"),
          QStringLiteral("planTransectExit"), QStringLiteral("planPolyline"), QStringLiteral("planPatternFill"),
          QStringLiteral("planPolygonFill")}) {
        for (auto* visual : named(name)) {
            retainedPatterns.append(visual);
        }
    }
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterFill")).size(), 1, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterTraversal")).size(), 1, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterMarker")).size(), 1, TestTimeout::mediumMs());
    planStyleWaitForVisualBindings(map, &visualItems, firstContext);
    QVERIFY(!QTest::currentTestFailed());
    QList<QPointer<QQuickItem>> retainedC3b;
    for (const auto& name : {QStringLiteral("planLandingArea"), QStringLiteral("planGlideSlope"),
                             QStringLiteral("planPatternLine"), QStringLiteral("planLoiterRing"),
                             QStringLiteral("planPerimeterFill"), QStringLiteral("planPerimeterTraversal"),
                             QStringLiteral("planPerimeterMarker"), QStringLiteral("planPerimeterEntry")}) {
        for (auto* primitive : named(name)) {
            retainedC3b.append(primitive);
        }
    }
    const auto markers = named(QStringLiteral("planWaypoint"));
    const auto route = QPointer<QQuickItem>(named(QStringLiteral("planMissionRoute")).first());
    const auto accent = QPointer<QQuickItem>(named(QStringLiteral("planHomeAccent")).first());
    QList<QPointer<QQuickItem>> retainedMarkers;
    for (auto* marker : markers) {
        retainedMarkers.append(marker);
    }
    const auto verifyColour = [&](const QColor& expected, quint32 id) {
        colourStage = QStringLiteral("vehicle=%1 expectedColour=%2").arg(id).arg(expected.name());
        planStyleWaitForVisualBindings(map, &visualItems, qvariant_cast<QObject*>(map->property("style")));
        QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY2_WITH_TIMEOUT(
            named(QStringLiteral("planPatternOutline")).size() == 5 + corridor.corridorBoundaries().size(),
            qPrintable(liveContext()), TestTimeout::shortMs());
        for (const auto& visual : retainedPatterns) {
            QVERIFY2(visual, "live registry: a stable pattern visual was destroyed during recolouring");
            QVERIFY2(named(visual->objectName()).contains(visual.data()),
                     qPrintable(QStringLiteral("live registry: missing retained %1").arg(planStyleItemChain(visual))));
            const auto name = visual->objectName();
            if (name == QStringLiteral("planPatternMarker")) {
                QTRY_VERIFY2_WITH_TIMEOUT(visual && visual->property("renderedBodyColor").value<QColor>() == expected,
                                          qPrintable(liveContext()), TestTimeout::shortMs());
                QVERIFY2((qvariant_cast<QObject*>(visual->property("planStyle"))) ==
                             (qvariant_cast<QObject*>(root->property("style"))),
                         qPrintable(liveContext()));
                QCOMPARE(qvariant_cast<QObject*>(visual->property("planStyle")),
                         qvariant_cast<QObject*>(root->property("style")));
                QVERIFY2((visual->property("renderedBorderColor").value<QColor>()) == (QColor(Qt::white)),
                         qPrintable(liveContext()));
                QCOMPARE(visual->property("renderedBorderColor").value<QColor>(), QColor(Qt::white));
                QVERIFY2((visual->property("renderedBorderWidth").toReal()) == (2.0), qPrintable(liveContext()));
                QCOMPARE(visual->property("renderedBorderWidth").toReal(), 2.0);
                QVERIFY2((visual->property("renderedTextColor").value<QColor>()) == (QColor(Qt::black)),
                         qPrintable(liveContext()));
                QCOMPARE(visual->property("renderedTextColor").value<QColor>(), QColor(Qt::black));
                const auto halos = planStyleItems(visual, QStringLiteral("planWaypointSelectionHalo"));
                QVERIFY2((halos.size()) == (1), qPrintable(liveContext()));
                QCOMPARE(halos.size(), 1);
                QVERIFY2(halos.first()->isVisible(), qPrintable(liveContext()));
            } else if (name.startsWith(QStringLiteral("planTransect")) || name == QStringLiteral("planPolyline")) {
                QTRY_VERIFY2_WITH_TIMEOUT(visual && planStyleColor(visual, QStringLiteral("line.color")) == expected,
                                          qPrintable(liveContext()), TestTimeout::shortMs());
                QVERIFY2((QQmlProperty(visual, QStringLiteral("line.width")).read().toReal()) == (4.0),
                         qPrintable(liveContext()));
                QCOMPARE(QQmlProperty(visual, QStringLiteral("line.width")).read().toReal(), 4.0);
                QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(visual, map), 0.5), qPrintable(liveContext()));
                QCOMPARE(planStyleOpacityBelowMap(visual, map), 0.5);
            } else if (visual->property("path").toList() == structure.flightPolygon()->path()) {
                QTRY_VERIFY2_WITH_TIMEOUT(visual && planStyleColor(visual, QStringLiteral("border.color")) == expected,
                                          qPrintable(liveContext()), TestTimeout::shortMs());
                QVERIFY2((QQmlProperty(visual, QStringLiteral("border.width")).read().toReal()) == (4.0),
                         qPrintable(liveContext()));
                QCOMPARE(QQmlProperty(visual, QStringLiteral("border.width")).read().toReal(), 4.0);
                QVERIFY2((visual->property("color").value<QColor>().alpha()) == (0), qPrintable(liveContext()));
                QCOMPARE(visual->property("color").value<QColor>().alpha(), 0);
                QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(visual, map), 0.5), qPrintable(liveContext()));
                QCOMPARE(planStyleOpacityBelowMap(visual, map), 0.5);
            } else {
                QTRY_VERIFY2_WITH_TIMEOUT(visual && visual->property("color").value<QColor>() == expected,
                                          qPrintable(liveContext()), TestTimeout::shortMs());
                QVERIFY2((QQmlProperty(visual, QStringLiteral("border.width")).read().toReal()) == (0.0),
                         qPrintable(liveContext()));
                QCOMPARE(QQmlProperty(visual, QStringLiteral("border.width")).read().toReal(), 0.0);
                QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(visual, map), 0.25), qPrintable(liveContext()));
                QCOMPARE(planStyleOpacityBelowMap(visual, map), 0.25);
            }
        }
        for (const auto& primitive : retainedC3b) {
            QVERIFY2(primitive && named(primitive->objectName()).contains(primitive.data()),
                     "C3b live registry: stable landing/perimeter primitive replaced");
        }
        auto* perimeterFill = named(QStringLiteral("planPerimeterFill")).first();
        auto* perimeterLine = named(QStringLiteral("planPerimeterTraversal")).first();
        auto* perimeterLabel = named(QStringLiteral("planPerimeterMarker")).first();
        QTRY_COMPARE_WITH_TIMEOUT(perimeterFill->property("color").value<QColor>(), expected, TestTimeout::shortMs());
        QCOMPARE(QQmlProperty(perimeterFill, QStringLiteral("border.width")).read().toReal(), 0.0);
        QCOMPARE(perimeterFill->opacity(), 0.04);
        QTRY_COMPARE_WITH_TIMEOUT(planStyleColor(perimeterLine, QStringLiteral("line.color")), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(QQmlProperty(perimeterLine, QStringLiteral("line.width")).read().toReal(), 4.0);
        QCOMPARE(perimeterLine->opacity(), 0.5);
        QTRY_COMPARE_WITH_TIMEOUT(perimeterLabel->property("renderedBodyColor").value<QColor>(), expected,
                                  TestTimeout::shortMs());
        QCOMPARE(qvariant_cast<QObject*>(perimeterLabel->property("planStyle")),
                 qvariant_cast<QObject*>(root->property("style")));
        QCOMPARE(perimeterLabel->property("renderedBorderWidth").toReal(), 2.0);
        QCOMPARE(perimeterLabel->property("renderedTextColor").value<QColor>(), QColor(Qt::black));
        QVariantList currentRings{QVariant(survey.surveyAreaPolygon()->path()),
                                  QVariant(structure.structurePolygon()->path())};
        currentRings.append(corridor.corridorBoundaries());
        currentRings.append(QVariant(perimeter.perimeterPolygon()->path()));
        currentRings.append(QVariant(landingArea->property("path").toList()));
        currentRings.append(QVariant(glideSlope->property("path").toList()));
        for (const auto& ring : currentRings) {
            const auto resolveOutline = [&]() -> QQuickItem* {
                for (auto* outline : named(QStringLiteral("planPatternOutline"))) {
                    if (outline->property("vertices").toList() == ring.toList()) {
                        return outline;
                    }
                }
                return nullptr;
            };
            QTRY_VERIFY2_WITH_TIMEOUT(
                resolveOutline() && resolveOutline()->property("renderedStrokeColor").value<QColor>() == expected,
                qPrintable(liveContext()), TestTimeout::shortMs());
            auto* outline = resolveOutline();
            QVERIFY2(outline, qPrintable(liveContext()));
            QVERIFY2((outline->property("renderedStrokeWidth").toReal()) == (3.0), qPrintable(liveContext()));
            QCOMPARE(outline->property("renderedStrokeWidth").toReal(), 3.0);
            QVERIFY2((outline->property("renderedDashPattern").toList()) == (QVariantList({6.0, 3.0})),
                     qPrintable(liveContext()));
            QCOMPARE(outline->property("renderedDashPattern").toList(), QVariantList({6.0, 3.0}));
            const bool landingOutline = ring.toList() == landingArea->property("path").toList() ||
                                        ring.toList() == glideSlope->property("path").toList();
            QCOMPARE(planStyleOpacityBelowMap(planStyleSource(outline), map), landingOutline ? 1.0 : 0.5);
        }
        QVERIFY2(route && accent, qPrintable(liveContext()));
        QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(route, QStringLiteral("line.color"))) == (expected),
                                  qPrintable(liveContext()), TestTimeout::shortMs());
        QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 4.0);
        QCOMPARE(route->property("vehicleId").toUInt(), id);
        QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(accent, QStringLiteral("border.color"))) == (expected),
                                  qPrintable(liveContext()), TestTimeout::shortMs());
        QCOMPARE(accent->property("vehicleId").toUInt(), id);
        for (const auto& marker : retainedMarkers) {
            QVERIFY2(marker, qPrintable(liveContext()));
            auto* label = planStyleSource(marker);
            QVERIFY2(label, qPrintable(liveContext()));
            auto* body = qvariant_cast<QQuickItem*>(label->property("renderedIndicator"));
            QVERIFY2(body, qPrintable(liveContext()));
            QTRY_VERIFY2_WITH_TIMEOUT((marker->property("vehicleId").toUInt()) == (id), qPrintable(liveContext()),
                                      TestTimeout::shortMs());
            QTRY_VERIFY2_WITH_TIMEOUT((marker->property("renderedBodyColor").value<QColor>()) == (expected),
                                      qPrintable(liveContext()), TestTimeout::shortMs());
            QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), policy->markerBorderColor());
            QCOMPARE(marker->property("renderedBorderWidth").toReal(), policy->markerBorderWidth());
            QCOMPARE(marker->property("renderedTextColor").value<QColor>(), policy->markerTextColor());
            if (marker->property("current").toBool()) {
                const auto halos = planStyleItems(label, QStringLiteral("planWaypointSelectionHalo"));
                QCOMPARE(halos.size(), 1);
                QVERIFY2(halos.first()->isVisible(), qPrintable(liveContext()));
                QTRY_VERIFY2_WITH_TIMEOUT((halos.first()->width()) == (body->width() * 2), qPrintable(liveContext()),
                                          TestTimeout::shortMs());
            }
        }
        QTRY_VERIFY2_WITH_TIMEOUT(loiterCircle(), qPrintable(loiterContext()), TestTimeout::shortMs());
        QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(loiterCircle(), QStringLiteral("border.color"))) == (expected),
                                  qPrintable(liveContext()), TestTimeout::shortMs());
        QCOMPARE(QQmlProperty(loiterCircle(), QStringLiteral("border.width")).read().toReal(), 3.0);
        QTRY_COMPARE_WITH_TIMEOUT(landingArea->property("color").value<QColor>(), expected, TestTimeout::shortMs());
        QCOMPARE(landingArea->opacity(), 0.5);
        QTRY_COMPARE_WITH_TIMEOUT(glideSlope->property("color").value<QColor>(), expected, TestTimeout::shortMs());
        QCOMPARE(glideSlope->opacity(), 0.5);
        QCOMPARE(named(QStringLiteral("planPatternLine")).size(), 2);
        for (auto* path : named(QStringLiteral("planPatternLine"))) {
            QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(path, QStringLiteral("line.color"))) == (expected),
                                      qPrintable(liveContext()), TestTimeout::shortMs());
            QCOMPARE(path->property("vehicleId").toUInt(), id);
            QCOMPARE(QQmlProperty(path, QStringLiteral("line.width")).read().toReal(), 4.0);
        }
        QCOMPARE(named(QStringLiteral("planLoiterRing")).size(), 2);
        for (auto* ring : named(QStringLiteral("planLoiterRing"))) {
            QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(ring, QStringLiteral("border.color"))) == (expected),
                                      qPrintable(liveContext()), TestTimeout::shortMs());
            QCOMPARE(ring->property("vehicleId").toUInt(), id);
            QCOMPARE(QQmlProperty(ring, QStringLiteral("border.width")).read().toReal(), 4.0);
            QCOMPARE(ring->property("color").value<QColor>(), QColor(QStringLiteral("transparent")));
        }
    };
    const QColor neutral = root->property("neutral").value<QColor>();
    QVERIFY2(neutral.isValid(), qPrintable(liveContext()));
    verifyColour(neutral, 7501);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    QCOMPARE(registry->vehicles()->count(), initialEntries);
    QVERIFY2(!roles->isNicknameTaken(QStringLiteral("Other vehicle"), 7502), qPrintable(liveContext()));
    roles->addEntry(7502, role, QStringLiteral("Other vehicle"), 0);
    QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 1);
    auto* otherRole = roles->roleEntries()->value<VehicleRoleEntry*>(initialRoleEntries);
    QVERIFY2(otherRole, qPrintable(liveContext()));
    QCOMPARE(otherRole->sysid(), 7502U);
    QCOMPARE(otherRole->role(), role);
    QCOMPARE(otherRole->name(), QStringLiteral("Other vehicle"));
    QCOMPARE(otherRole->port(), 0);
    QCOMPARE(registry->vehicles()->count(), initialEntries + 1);
    // _remember gives 7502 colour index initialEntries and 7501 the next index, including palette wrap.
    auto* other = registry->vehicles()->value<COPVehicle*>(initialEntries);
    QVERIFY2(other, qPrintable(liveContext()));
    QCOMPARE(other->sysid(), 7502U);
    QVERIFY2(other->color().isValid(), qPrintable(liveContext()));
    registry->selectVehicle(7502);
    QCOMPARE(registry->selected(), other);
    QSignalSpy revisions(policy, &QGCPlanStyle::revisionChanged);
    QVERIFY2(revisions.isValid(), qPrintable(liveContext()));
    const int revisionBeforeEntry = policy->revision();
    QVERIFY2(!registry->colorForSysid(7501).isValid(), qPrintable(liveContext()));
    QVERIFY2(!roles->isNicknameTaken(QStringLiteral("Late vehicle"), 7501), qPrintable(liveContext()));
    roles->addEntry(7501, role, QStringLiteral("Late vehicle"), 0);
    QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 2);
    auto* lateRole = roles->roleEntries()->value<VehicleRoleEntry*>(initialRoleEntries + 1);
    QVERIFY2(lateRole, qPrintable(liveContext()));
    QCOMPARE(lateRole->sysid(), 7501U);
    QCOMPARE(lateRole->role(), role);
    QCOMPARE(lateRole->name(), QStringLiteral("Late vehicle"));
    QCOMPARE(lateRole->port(), 0);
    QCOMPARE(registry->vehicles()->count(), initialEntries + 2);
    auto* lateEntry = registry->vehicles()->value<COPVehicle*>(initialEntries + 1);
    QVERIFY2(lateEntry, qPrintable(liveContext()));
    QCOMPARE(lateEntry->sysid(), 7501U);
    QTRY_VERIFY2_WITH_TIMEOUT(revisions.count() > 0, qPrintable(liveContext()), TestTimeout::shortMs());
    QVERIFY2(policy->revision() != revisionBeforeEntry, qPrintable(liveContext()));
    QTRY_VERIFY2_WITH_TIMEOUT(registry->colorForSysid(7501).isValid(), qPrintable(liveContext()),
                              TestTimeout::shortMs());
    QColor late = registry->colorForSysid(7501);
    QCOMPARE(late, lateEntry->color());
    QCOMPARE(registry->selected(), other);
    QVERIFY2(late != other->color(), qPrintable(liveContext()));
    QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(route, QStringLiteral("line.color"))) == (late),
                              qPrintable(liveContext()), TestTimeout::shortMs());
    verifyColour(late, 7501);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    lateEntry->setColorIndex(initialEntries + 4);
    late = lateEntry->color();
    verifyColour(lateEntry->color(), 7501);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    // Change the context object itself after every async visual has already been created.
    QVERIFY2(root->setProperty("useSecondContext", true), qPrintable(liveContext()));
    auto* secondContext = qvariant_cast<QObject*>(root->property("style"));
    QVERIFY2(secondContext, qPrintable(liveContext()));
    QVERIFY2(secondContext != firstContext, qPrintable(liveContext()));
    QCOMPARE(secondContext->property("vehicleId").toUInt(), 7502U);
    QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(route, QStringLiteral("line.color"))) == (other->color()),
                              qPrintable(liveContext()), TestTimeout::shortMs());
    verifyColour(other->color(), 7502);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    QVERIFY2(root->setProperty("useSecondContext", false), qPrintable(liveContext()));
    QCOMPARE(qvariant_cast<QObject*>(root->property("style")), firstContext);
    verifyColour(late, 7501);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    const int revisionBeforeLookups = policy->revision();
    const auto orderBeforeLookups = QSettings().value(QStringLiteral("COP/VehicleOrder")).toList();
    for (quint32 id : {0U, 7599U, std::numeric_limits<quint32>::max()}) {
        QVERIFY2(root->setProperty("styleId", id), qPrintable(liveContext()));
        QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(route, QStringLiteral("line.color"))) == (neutral),
                                  qPrintable(liveContext()), TestTimeout::shortMs());
        verifyColour(neutral, id);
        QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
        QVERIFY2(!registry->colorForSysid(id).isValid(), qPrintable(liveContext()));
        QVERIFY2(!policy->vehicleColor(id).isValid(), qPrintable(liveContext()));
        for (int i = 0; i < registry->vehicles()->count(); ++i) {
            auto* entry = registry->vehicles()->value<COPVehicle*>(i);
            QVERIFY2(entry, qPrintable(liveContext()));
            QVERIFY2(entry->sysid() != id, qPrintable(liveContext()));
        }
        QVERIFY2(roles->roleForSysid(id).isEmpty(), qPrintable(liveContext()));
        QCOMPARE(registry->vehicles()->count(), initialEntries + 2);
        QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 2);
        QCOMPARE(policy->revision(), revisionBeforeLookups);
        QCOMPARE(QSettings().value(QStringLiteral("COP/VehicleOrder")).toList(), orderBeforeLookups);
    }
    const auto maximumId = std::numeric_limits<quint32>::max();
    roles->addEntry(maximumId, role, QStringLiteral("Full width pattern owner"), 0);
    QVERIFY2(registry->colorForSysid(maximumId).isValid(), qPrintable(liveContext()));
    QVERIFY2(root->setProperty("styleId", static_cast<double>(maximumId)), qPrintable(liveContext()));
    verifyColour(registry->colorForSysid(maximumId), maximumId);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    auto* customPolicy = qobject_cast<CustomPlanStyle*>(policy);
    QVERIFY2(customPolicy, qPrintable(liveContext()));
    customPolicy->attachRegistry(nullptr);
    verifyColour(neutral, maximumId);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    customPolicy->attachRegistry(registry);
    verifyColour(registry->colorForSysid(maximumId), maximumId);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    QVERIFY2(root->setProperty("styleId", 0), qPrintable(liveContext()));
    verifyColour(neutral, 0);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    const auto checkCorridorRings = [&] {
        const auto boundaries = corridor.corridorBoundaries();
        QTRY_VERIFY2_WITH_TIMEOUT((named(QStringLiteral("planPatternOutline")).size()) == (5 + boundaries.size()),
                                  qPrintable(liveContext()), TestTimeout::shortMs());
        for (const auto& boundary : boundaries) {
            int matches = 0;
            for (auto* outline : named(QStringLiteral("planPatternOutline"))) {
                matches += outline->property("vertices").toList() == boundary.toList() ? 1 : 0;
            }
            QVERIFY2(matches == 1, qPrintable(liveContext()));
            QCOMPARE(matches, 1);
        }
        for (auto* boundary : named(QStringLiteral("planCorridorBoundary"))) {
            QCOMPARE(QQmlProperty(boundary, QStringLiteral("line.width")).read().toReal(), 0.0);
            QVERIFY2(!boundary->isVisible(), qPrintable(liveContext()));
        }
    };
    checkCorridorRings();
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    const auto square = PlanStylePatternTest::square(base);
    corridor.corridorPolyline()->setPath(QList<QGeoCoordinate>{square[0], square[1], square[2], square[3], square[0]});
    QCOMPARE(corridor.corridorBoundaries().size(), 2);
    checkCorridorRings();
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    corridor.corridorPolyline()->setPath(QList<QGeoCoordinate>{square[0], square[2], square[1], square[3]});
    checkCorridorRings();
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(liveContext()));
    // The simple loiter renderer retains its error colour and recovers its neutral fallback.
    waypoint.setTerrainCollision(true);
    QTRY_VERIFY2_WITH_TIMEOUT(
        (planStyleColor(loiterCircle(), QStringLiteral("border.color"))) == (QColor(QStringLiteral("red"))),
        qPrintable(liveContext()), TestTimeout::shortMs());
    waypoint.setTerrainCollision(false);
    QTRY_VERIFY2_WITH_TIMEOUT((planStyleColor(loiterCircle(), QStringLiteral("border.color"))) == (neutral),
                              qPrintable(liveContext()), TestTimeout::shortMs());
    QTRY_VERIFY2_WITH_TIMEOUT(fixedWing.flightPathSegments()->count() > 0, qPrintable(liveContext()),
                              TestTimeout::mediumMs());
    auto* approach = fixedWing.flightPathSegments()->value<FlightPathSegment*>(0);
    QVERIFY2(approach, qPrintable(liveContext()));
    QVERIFY2(approach->totalDistance() > 0.0, qPrintable(liveContext()));
    QVERIFY2(approach->setProperty("distanceBetween", approach->totalDistance()), qPrintable(liveContext()));
    QVERIFY2(approach->setProperty("finalDistanceBetween", approach->totalDistance()), qPrintable(liveContext()));
    QVERIFY2(approach->setProperty("amslTerrainHeights", QVariantList{1.0e6, 1.0e6}), qPrintable(liveContext()));
    approach->setCoord1AMSLAlt(-100);
    approach->setCoord2AMSLAlt(-100);
    QVERIFY2(approach->terrainCollision(), qPrintable(liveContext()));
    QVERIFY2(fixedWing.terrainCollision(), qPrintable(liveContext()));
    QTRY_VERIFY2_WITH_TIMEOUT((glideSlope->property("color").value<QColor>()) == (QColor(QStringLiteral("red"))),
                              qPrintable(liveContext()), TestTimeout::shortMs());
    QCOMPARE(landingArea->property("color").value<QColor>(), neutral);
    approach->setCoord1AMSLAlt(2.0e6);
    approach->setCoord2AMSLAlt(2.0e6);
    QVERIFY2(!approach->terrainCollision(), qPrintable(liveContext()));
    QVERIFY2(!fixedWing.terrainCollision(), qPrintable(liveContext()));
    QTRY_VERIFY2_WITH_TIMEOUT((glideSlope->property("color").value<QColor>()) == (neutral), qPrintable(liveContext()),
                              TestTimeout::shortMs());
    QCOMPARE(registry->vehicles()->count(), initialEntries + 3);
    QCOMPARE(roles->roleEntries()->count(), initialRoleEntries + 3);
    QCOMPARE(named(QStringLiteral("planMissionRoute")).size(), 1);
    QCOMPARE(named(QStringLiteral("planMissionRoute")).first(), route.data());
    QCOMPARE(named(QStringLiteral("planHomeAccent")).size(), 1);
    QCOMPARE(named(QStringLiteral("planHomeAccent")).first(), accent.data());
    QCOMPARE(named(QStringLiteral("planWaypoint")).size(), retainedMarkers.size());
    for (const auto& marker : retainedMarkers) {
        QVERIFY2(marker, qPrintable(liveContext()));
        QVERIFY2(named(QStringLiteral("planWaypoint")).contains(marker.data()), qPrintable(liveContext()));
    }
}

void PlanStyleUITest::_liveBoundaryVisuals()
{
    auto* plugin = QGCCorePlugin::instance();
    QVERIFY(plugin);
    auto destroyEngine = [plugin](QQmlApplicationEngine* engine) { plugin->destroyQmlApplicationEngine(engine); };
    std::unique_ptr<QQmlApplicationEngine, decltype(destroyEngine)> engine(plugin->createQmlApplicationEngine(nullptr),
                                                                           destroyEngine);
    QVERIFY(engine);
    planStyleWarmUpScreenTools(engine.get());
    if (QTest::currentTestFailed()) {
        return;
    }
    auto* registry = engine->singletonInstance<COPController*>("QGC", "COPController");
    auto* roles = engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
    QVERIFY(registry && roles);
    registry->initialize(roles);
    QVERIFY(!registry->colorForSysid(7601).isValid());
    QVERIFY(!registry->colorForSysid(7602).isValid());
    QVERIFY(!roles->availableRoles().isEmpty());
    const auto role = roles->availableRoles().first();
    const QGeoCoordinate base(47, 8, 120);
    QmlObjectListModel polygons;
    QmlObjectListModel circles;
    for (bool inclusion : {true, false}) {
        auto* polygon = new QGCFencePolygon(inclusion, &polygons);
        polygon->setInteractive(false);
        for (double azimuth : {0.0, 120.0, 240.0}) {
            polygon->appendVertex(base.atDistanceAndAzimuth(inclusion ? 250 : 80, azimuth));
        }
        polygons.append(polygon);
        auto* circle =
            new QGCFenceCircle(base.atDistanceAndAzimuth(150, inclusion ? 90 : 180), 45, inclusion, &circles);
        circle->setInteractive(false);
        circles.append(circle);
    }
    const auto restoreOfflineSettings =
        PlanStylePatternTest::planStyleOfflineVehicleSettings(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR);
    PlanMasterController master(MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_TYPE_QUADROTOR);
    master.start();
    QVERIFY(master.controllerVehicle());
    QCOMPARE(master.controllerVehicle()->firmwareType(), MAV_AUTOPILOT_ARDUPILOTMEGA);
    QCOMPARE(master.controllerVehicle()->vehicleType(), MAV_TYPE_QUADROTOR);
    auto* rallyController = master.rallyPointController();
    QVERIFY(rallyController);
    rallyController->addPoint(base.atDistanceAndAzimuth(50, 90));
    rallyController->addPoint(base.atDistanceAndAzimuth(100, 90));
    QCOMPARE(rallyController->points()->count(), 2);
    auto* rally = rallyController->points()->value<RallyPoint*>(0);
    QVERIFY(rally);
    rally->setCoordinate(QGeoCoordinate(47, 8.001, 137));
    rallyController->setCurrentRallyPoint(rally);
    engine->rootContext()->setContextProperty(QStringLiteral("testPolygons"), &polygons);
    engine->rootContext()->setContextProperty(QStringLiteral("testCircles"), &circles);
    engine->rootContext()->setContextProperty(QStringLiteral("testRallyController"), rallyController);
    QQmlComponent component(engine.get());
    component.setData(R"(
        import QtQuick
        import QtLocation
        import QtPositioning
        import QGroundControl
        import QGroundControl.Controls
        import QGroundControl.FlightMap
        import QGroundControl.PlanView
        FlightMap {
            id: testMap
            width: 640
            height: 480
            center: QtPositioning.coordinate(47, 8)
            zoomLevel: 16
            planView: true
            isSatelliteMap: false
            allowGCSLocationCenter: false
            allowVehicleLocationCenter: false
            property int _editingLayer: 2
            property int _layerRally: 2
            property real layerOpacity: 1
            property bool visualsEnabled: true
            property bool styleSupported: true
            property real styleId: 7601
            readonly property color neutral: QGroundControl.globalPalette.mapMissionTrajectory
            readonly property var style: fenceStyle
            QtObject { id: mainWindow; function allowViewSwitch() { return true } }
            PlanStyleContext { id: fenceStyle; vehicleId: testMap.styleId; supported: testMap.styleSupported }
            property QtObject fence: QtObject {
                property var polygons: testPolygons
                property var circles: testCircles
                property real paramCircularFence: 350
                property var breachReturnPoint: QtPositioning.coordinate(47, 8, 120)
            }
            Loader {
                active: testMap.visualsEnabled
                sourceComponent: GeoFenceMapVisuals {
                    map: testMap
                    myGeoFenceController: testMap.fence
                    homePosition: QtPositioning.coordinate(47, 8)
                    planStyle: fenceStyle
                    opacity: testMap.layerOpacity
                }
            }
            Loader {
                active: testMap.visualsEnabled
                sourceComponent: RallyPointMapVisuals {
                    objectName: "rallyVisuals"
                    map: testMap
                    myRallyPointController: testRallyController
                    planStyle: fenceStyle
                    interactive: true
                    planView: true
                    opacity: testMap.layerOpacity
                }
            }
        }
    )",
                      QUrl());
    QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, TestTimeout::shortMs());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QQuickWindow boundaryWindow;
    boundaryWindow.resize(800, 600);
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto* map = qobject_cast<QQuickItem*>(root.get());
    QVERIFY(map);
    map->setParentItem(boundaryWindow.contentItem());
    boundaryWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(boundaryWindow.isExposed(), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(map->property("mapReady").toBool(), TestTimeout::longMs());
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planFenceFill")).size(), 5, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepInOutline")).size(), 3, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepOutOutline")).size(), 2, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planRallyPoint")).size(), 2, TestTimeout::mediumMs());
    const auto rallyPositionsReady = [&] {
        if (!planStyleRenderMap(map)) {
            return false;
        }
        const auto markers = named(QStringLiteral("planRallyPoint"));
        if (markers.size() != rallyController->points()->count()) {
            return false;
        }
        for (auto* marker : markers) {
            if (!planStyleMapItemProjected(map, marker)) {
                return false;
            }
        }
        for (auto* item : map->childItems()) {
            if (!item->isVisible() || item->metaObject()->indexOfProperty("itemCoordinate") < 0) {
                continue;
            }
            auto* indicator = qvariant_cast<QQuickItem*>(item->property("itemIndicator"));
            if (!indicator) {
                return false;
            }
            const auto anchor = indicator->property("anchorPoint").toPointF() +
                                QPointF(item->property("_touchMarginHorizontal").toReal(),
                                        item->property("_touchMarginVertical").toReal());
            if (!planStyleProjectedPosition(map, item->property("itemCoordinate").value<QGeoCoordinate>(),
                                            item->mapToScene(anchor))) {
                return false;
            }
        }
        return true;
    };
    const auto check = [&](const QColor& color) {
        QTRY_VERIFY_WITH_TIMEOUT(rallyPositionsReady(), TestTimeout::mediumMs());
        planStyleCheckBoundaries(map, 7601, color, 2, 2);
        QVERIFY(!QTest::currentTestFailed());
        int selected = 0;
        for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
            planStyleCheckRally(map, marker, 7601, color);
            QVERIFY(!QTest::currentTestFailed());
            selected += marker->property("checked").toBool() ? 1 : 0;
            QCOMPARE(marker->opacity(), root->property("layerOpacity").toReal());
        }
        QCOMPARE(selected, 1);
        const auto breach = named(QStringLiteral("planBreachReturnPoint"));
        QCOMPARE(breach.size(), 1);
        QCOMPARE(breach.first()->property("markerColor").value<QColor>(), color);
        QCOMPARE(breach.first()->property("label").toString(), QStringLiteral("B"));
        QCOMPARE(breach.first()->property("vehicleId").toUInt(), 7601U);
        QCOMPARE(breach.first()->property("coordinate").value<QGeoCoordinate>(), base);
        QCOMPARE(breach.first()->property("markerBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
        QCOMPARE(breach.first()->property("markerBorderWidth").toReal(), 2.0);
        QCOMPARE(breach.first()->property("markerTextColor").value<QColor>(), QColor(QStringLiteral("black")));
    };
    const int registryCount = registry->vehicles()->count();
    check(root->property("neutral").value<QColor>());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(registry->vehicles()->count(), registryCount);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QList<QPointer<QQuickItem>> retained;
    for (const auto& name : {QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"),
                             QStringLiteral("planKeepOutOutline"), QStringLiteral("planRallyPoint")}) {
        for (auto* item : named(name)) {
            retained.append(item);
        }
    }
    roles->addEntry(7602, role, QStringLiteral("Browsed boundary test vehicle"), 0);
    QTRY_VERIFY_WITH_TIMEOUT(registry->colorForSysid(7602).isValid(), TestTimeout::shortMs());
    registry->selectVehicle(7602);
    QVERIFY(registry->selected());
    QCOMPARE(registry->selected()->sysid(), 7602U);
    QVERIFY(registry->selected()->sysid() != 0);
    roles->addEntry(7601, role, QStringLiteral("Late boundary test vehicle"), 0);
    QTRY_VERIFY_WITH_TIMEOUT(registry->colorForSysid(7601).isValid(), TestTimeout::shortMs());
    COPVehicle* entry = nullptr;
    for (int i = 0; i < registry->vehicles()->count(); ++i) {
        auto* candidate = registry->vehicles()->value<COPVehicle*>(i);
        QVERIFY(candidate);
        if (candidate->sysid() == 7601) {
            entry = candidate;
        }
    }
    QVERIFY(entry);
    QVERIFY(entry->sysid() != 0);
    QVERIFY(entry->color() != registry->selected()->color());
    QTRY_COMPARE_WITH_TIMEOUT(
        named(QStringLiteral("planKeepInOutline")).first()->property("renderedStrokeColor").value<QColor>(),
        entry->color(), TestTimeout::shortMs());
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    for (const auto& item : retained) {
        QVERIFY(item);
    }
    QVERIFY(root->setProperty("layerOpacity", 0.5));
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        QCOMPARE(fill->opacity(), 0.5);
    }
    auto* polygon = polygons.value<QGCFencePolygon*>(0);
    QVERIFY(polygon);
    QQuickItem* polygonOutline = nullptr;
    for (auto* item : named(QStringLiteral("planKeepInOutline"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon")) {
            polygonOutline = item;
        }
    }
    QVERIFY(polygonOutline);
    const auto beforeDrag = polygonOutline->property("vertices").toList();
    polygon->setVertexDrag(true);
    QTRY_VERIFY_WITH_TIMEOUT(!polygonOutline->property("_projectionPending").toBool(), TestTimeout::shortMs());
    QSignalSpy projections(polygonOutline, SIGNAL(_screenPointsChanged()));
    QVERIFY(projections.isValid());
    const auto dragged = base.atDistanceAndAzimuth(600, 45);
    polygon->adjustVertex(0, base.atDistanceAndAzimuth(400, 45));
    polygon->adjustVertex(0, base.atDistanceAndAzimuth(500, 45));
    polygon->adjustVertex(0, dragged);
    QVERIFY(polygon->vertexDrag());
    QTRY_COMPARE_WITH_TIMEOUT(polygonOutline->property("vertices").toList().first().value<QGeoCoordinate>(), dragged,
                              TestTimeout::shortMs());
    QVERIFY(polygonOutline->property("vertices").toList() != beforeDrag);
    polygon->setShowAltColor(true);
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(projections.count(), 1);
    polygon->setVertexDrag(false);
    auto* circle = circles.value<QGCFenceCircle*>(0);
    QVERIFY(circle);
    circle->setCenter(base.atDistanceAndAzimuth(400, 225));
    circle->radius()->setRawValue(123);
    auto* fence = qvariant_cast<QObject*>(root->property("fence"));
    QVERIFY(fence);
    QVERIFY(fence->setProperty("paramCircularFence", 480));
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        if (fill->property("boundaryKind").toString() == QStringLiteral("circle") &&
            fill->property("inclusion").toBool()) {
            QCOMPARE(fill->property("center").value<QGeoCoordinate>(), circle->center());
            QCOMPARE(fill->property("radius").toDouble(), 123.0);
        } else if (fill->property("boundaryKind").toString() == QStringLiteral("parameterCircle")) {
            QCOMPARE(fill->property("radius").toDouble(), 480.0);
        }
    }
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    {
        const auto boundaryOutlines =
            named(QStringLiteral("planKeepInOutline")) + named(QStringLiteral("planKeepOutOutline"));
        QCOMPARE(boundaryOutlines.size(), 5);
        const auto projectionsSettled = [&] {
            for (auto* outline : boundaryOutlines) {
                if (outline->property("_projectionPending").toBool()) {
                    return false;
                }
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(projectionsSettled(), TestTimeout::shortMs());
        std::vector<std::unique_ptr<QSignalSpy>> projectionSpies;
        for (auto* outline : boundaryOutlines) {
            projectionSpies.push_back(std::make_unique<QSignalSpy>(outline, SIGNAL(_screenPointsChanged())));
            QVERIFY(projectionSpies.back()->isValid());
        }
        QSignalSpy zoomChanges(map, QQmlProperty(map, QStringLiteral("zoomLevel")).property().notifySignal());
        QSignalSpy widthChanges(map, &QQuickItem::widthChanged);
        QSignalSpy heightChanges(map, &QQuickItem::heightChanged);
        QVERIFY(zoomChanges.isValid());
        QVERIFY(widthChanges.isValid());
        QVERIFY(heightChanges.isValid());
        QCOMPARE(map->property("zoomLevel").toReal(), 16.0);
        QCOMPARE(map->width(), 640.0);
        QCOMPARE(map->height(), 480.0);
        // Unlike vertex drags, these independent inputs are not coalesced by the boundary models.
        QVERIFY(root->setProperty("zoomLevel", 17));
        QVERIFY(root->setProperty("width", 800));
        QVERIFY(root->setProperty("height", 600));
        QCOMPARE(map->property("zoomLevel").toReal(), 17.0);
        QCOMPARE(map->width(), 800.0);
        QCOMPARE(map->height(), 600.0);
        QCOMPARE(zoomChanges.count(), 1);
        QCOMPARE(widthChanges.count(), 1);
        QCOMPARE(heightChanges.count(), 1);
        check(entry->color());
        QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY_WITH_TIMEOUT(projectionsSettled(), TestTimeout::shortMs());
        for (const auto& spy : projectionSpies) {
            QCOMPARE(spy->count(), 1);
        }
    }
    QVERIFY(root->setProperty("center", QVariant::fromValue(base.atDistanceAndAzimuth(100, 90))));
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    // Exercise the existing selected-point drag object and its altitude-preserving assignment.
    const auto rallyVisualItems = named(QStringLiteral("rallyVisuals"));
    QCOMPARE(rallyVisualItems.size(), 1);
    auto* rallyVisuals = rallyVisualItems.first();
    auto* repeater = qvariant_cast<QObject*>(rallyVisuals->property("_rallyPointsComponent"));
    QVERIFY(repeater);
    QObject* drag = nullptr;
    for (auto* item : map->childItems()) {
        if (qvariant_cast<QObject*>(item->property("rallyPointObject")) == rally &&
            item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            drag = item;
        }
    }
    QVERIFY(drag);
    QVERIFY(drag->property("visible").toBool());
    const double altitude = rally->coordinate().altitude();
    QVERIFY(drag->setProperty("itemCoordinate", QVariant::fromValue(QGeoCoordinate(47.001, 8.002, -500))));
    QCOMPARE(rally->coordinate(), QGeoCoordinate(47.001, 8.002, altitude));
    rallyController->setCurrentRallyPoint(rallyController->points()->get(1));
    QVERIFY(!drag->property("visible").toBool());
    QQuickItem* clickMarker = nullptr;
    for (auto* item : named(QStringLiteral("planRallyPoint"))) {
        if (item->property("coordinate").value<QGeoCoordinate>() == rally->coordinate()) {
            clickMarker = item;
        }
    }
    QVERIFY(clickMarker);
    QVERIFY(!clickMarker->property("checked").toBool());
    auto* clickLoader = planStyleSource(clickMarker);
    QVERIFY(clickLoader);
    QPointer<QQuickItem> clickSource = qvariant_cast<QQuickItem*>(clickLoader->property("item"));
    QVERIFY(clickSource);
    QVERIFY(root->setProperty("center", QVariant::fromValue(rally->coordinate())));
    const auto pointerReady = [&] {
        // A correct target position alone can hide another marker's stale drag overlay.
        if (!rallyPositionsReady() || !clickSource) {
            return false;
        }
        const auto position = clickSource->mapToScene(QPointF(clickSource->width() / 2, clickSource->height() / 2));
        const auto mapCenter = map->mapToScene(QPointF(map->width() / 2, map->height() / 2));
        return clickSource->window() == &boundaryWindow && clickSource->isVisible() && clickSource->width() > 0 &&
               position.x() >= 0 && position.x() < boundaryWindow.width() && position.y() >= 0 &&
               position.y() < boundaryWindow.height() && (position - mapCenter).manhattanLength() < 2;
    };
    QTRY_VERIFY_WITH_TIMEOUT(pointerReady(), TestTimeout::mediumMs());
    const auto clickPosition =
        clickSource->mapToScene(QPointF(clickSource->width() / 2, clickSource->height() / 2)).toPoint();
    QVERIFY(planStyleMouseItemAt(clickSource, clickPosition));
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        if (marker != clickMarker) {
            QVERIFY2(!planStyleMouseItemAt(planStyleSource(marker), clickPosition),
                     "Another rally marker accepts the target click");
        }
    }
    for (auto* item : map->childItems()) {
        if (item->isVisible() && item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            QVERIFY2(!planStyleMouseItemAt(item, clickPosition), "A visible drag item accepts the target click");
        }
    }
    QTest::mouseClick(&boundaryWindow, Qt::LeftButton, Qt::NoModifier, clickPosition);
    QTRY_COMPARE_WITH_TIMEOUT(rallyController->currentRallyPoint(), static_cast<QObject*>(rally),
                              TestTimeout::shortMs());
    QVERIFY(clickMarker->property("checked").toBool());

    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    // Disabling the context tears down outlines and returns every actual fill to its stock branch.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QList<QPointer<QQuickItem>> outlines;
    QList<QPointer<QObject>> outlineLoaders;
    for (auto* item : named(QStringLiteral("planKeepInOutline")) + named(QStringLiteral("planKeepOutOutline"))) {
        outlines.append(item);
        auto* loader = item->parent();
        QVERIFY(loader && qvariant_cast<QQuickItem*>(loader->property("item")) == item);
        outlineLoaders.append(loader);
    }
    QList<QPointer<QQuickItem>> styledMarkers;
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        auto* loader = planStyleSource(marker);
        QVERIFY(loader);
        styledMarkers.append(qvariant_cast<QQuickItem*>(loader->property("item")));
        QVERIFY(styledMarkers.last());
    }
    polygon->setShowAltColor(false);
    for (const auto& outline : outlines) {
        QVERIFY(outline && outline->property("coalesceProjection").toBool());
        QVERIFY(!outline->property("_projectionPending").toBool());
    }
    // Clear the real Loaders with projections queued, before returning to the event loop.
    QVERIFY(root->setProperty("width", 780));
    QVERIFY(root->setProperty("height", 580));
    for (const auto& outline : outlines) {
        QVERIFY(outline->property("_projectionPending").toBool());
    }
    QVERIFY(root->setProperty("styleSupported", false));
    for (const auto& loader : outlineLoaders) {
        QVERIFY(loader && !loader->property("active").toBool());
        QVERIFY(!qvariant_cast<QQuickItem*>(loader->property("item")));
    }
    QObject projectionBarrier;
    bool projectionsDrained = false;
    QTimer::singleShot(0, &projectionBarrier, [&] { projectionsDrained = true; });
    QTRY_VERIFY_WITH_TIMEOUT(projectionsDrained, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(
        named(QStringLiteral("planKeepInOutline")).isEmpty() && named(QStringLiteral("planKeepOutOutline")).isEmpty(),
        TestTimeout::shortMs());
    for (const auto& outline : outlines) {
        QTRY_VERIFY_WITH_TIMEOUT(outline.isNull(), TestTimeout::shortMs());
    }
    for (const auto& marker : styledMarkers) {
        QTRY_VERIFY_WITH_TIMEOUT(marker.isNull(), TestTimeout::shortMs());
    }
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        const bool inclusion = fill->property("inclusion").toBool();
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), inclusion ? 2.0 : 0.0);
        QCOMPARE(fill->property("color").value<QColor>(),
                 inclusion ? QColor(QStringLiteral("transparent")) : QColor(QStringLiteral("orange")));
        QCOMPARE(fill->opacity(), inclusion ? 0.5 : 0.1);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QList<QPointer<QQuickItem>> stockMarkers;
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        QTRY_COMPARE_WITH_TIMEOUT(marker->property("markerShape").toString(), QStringLiteral("circle"),
                                  TestTimeout::shortMs());
        QCOMPARE(marker->property("markerShape").toString(), QStringLiteral("circle"));
        QCOMPARE(marker->property("label").toString(), QStringLiteral("R"));
        QCOMPARE(marker->property("markerBorderWidth").toReal(), 0.0);
        auto* loader = planStyleSource(marker);
        QVERIFY(loader);
        auto* stock = qvariant_cast<QQuickItem*>(loader->property("item"));
        QVERIFY(stock);
        stockMarkers.append(stock);
        QCOMPARE(marker->property("anchorPoint").toPointF(),
                 QPointF(stock->property("anchorPointX").toReal(), stock->property("anchorPointY").toReal()));
        QCOMPARE(marker->property("markerColor").value<QColor>(), stock->property("renderedBodyColor").value<QColor>());
        QCOMPARE(marker->property("markerBorderColor").value<QColor>(),
                 stock->property("renderedBorderColor").value<QColor>());
        QCOMPARE(marker->property("checked").toBool(), stock->property("checked").toBool());
    }
    QVERIFY(root->setProperty("styleSupported", true));
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepInOutline")).size(), 3, TestTimeout::shortMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planKeepOutOutline")).size(), 2, TestTimeout::shortMs());
    for (const auto& marker : stockMarkers) {
        QTRY_VERIFY_WITH_TIMEOUT(marker.isNull(), TestTimeout::shortMs());
    }
    check(entry->color());
    QVERIFY(!QTest::currentTestFailed());
    // Removing the authoritative boundary must delete both its fill and its Loader-owned outline.
    QPointer<QQuickItem> removedOutline;
    // Locate the replacement created when the style was enabled again.
    for (auto* item : named(QStringLiteral("planKeepInOutline"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon")) {
            removedOutline = item;
        }
    }
    QVERIFY(removedOutline);
    QPointer<QQuickItem> removedFill;
    for (auto* item : named(QStringLiteral("planFenceFill"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon") &&
            item->property("inclusion").toBool()) {
            removedFill = item;
        }
    }
    QVERIFY(removedFill);
    polygons.removeAt(0);
    polygon->deleteLater();
    QTRY_VERIFY_WITH_TIMEOUT(removedFill.isNull() && removedOutline.isNull(), TestTimeout::shortMs());
    planStyleCheckBoundaries(map, 7601, entry->color(), 1, 2);
    QVERIFY(!QTest::currentTestFailed());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QList<QPointer<QQuickItem>> remaining;
    for (const auto& name :
         {QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline"),
          QStringLiteral("planRallyPoint"), QStringLiteral("planBreachReturnPoint")}) {
        for (auto* item : named(name)) {
            remaining.append(item);
        }
    }
    for (auto* item : map->childItems()) {
        if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
            remaining.append(item);
        }
    }
    QVERIFY(root->setProperty("visualsEnabled", false));
    for (const auto& item : remaining) {
        QTRY_VERIFY_WITH_TIMEOUT(item.isNull(), TestTimeout::shortMs());
    }
    root.reset();
    polygons.clearAndDeleteContents();
    circles.clearAndDeleteContents();
}

void PlanStyleUITest::_uploadPlan(Vehicle* vehicle, int count)
{
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->isInitialConnectComplete() && vehicle->initialPlanRequestComplete(),
                             TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->homePosition().isValid(), TestTimeout::longMs());
    const auto home = vehicle->homePosition();
    QList<MissionItem*> items;
    for (int i = 0; i < count; ++i) {
        const auto coordinate = home.atDistanceAndAzimuth(i * 180, count == 4 ? 90 : 180);
        items.append(new MissionItem(i, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, 0,
                                     coordinate.latitude(), coordinate.longitude(), 100 + i * 10, true, false, this));
    }
    QSignalSpy missionSent(vehicle->missionManager(), &PlanManager::sendComplete);
    QVERIFY(missionSent.isValid());
    vehicle->missionManager()->writeMissionItems(items);
    QTRY_COMPARE_WITH_TIMEOUT(missionSent.count(), 1, TestTimeout::longMs());
    QVERIFY(!missionSent.first().first().toBool());
    QmlObjectListModel polygons;
    QmlObjectListModel circles;
    for (bool inclusion : {true, false}) {
        auto* polygon = new QGCFencePolygon(inclusion);
        for (double azimuth : {0.0, 120.0, 240.0}) {
            polygon->appendVertex(home.atDistanceAndAzimuth(inclusion ? 400 + count * 10 : 80 + count * 10, azimuth));
        }
        polygons.append(polygon);
        circles.append(new QGCFenceCircle(home.atDistanceAndAzimuth(count * 50, inclusion ? 90 : 180),
                                          inclusion ? 90 : 30, inclusion));
    }
    QSignalSpy fenceSent(vehicle->geoFenceManager(), &GeoFenceManager::sendComplete);
    QVERIFY(fenceSent.isValid());
    vehicle->geoFenceManager()->sendToVehicle(home, polygons, circles);
    polygons.clearAndDeleteContents();
    circles.clearAndDeleteContents();
    QTRY_COMPARE_WITH_TIMEOUT(fenceSent.count(), 1, TestTimeout::longMs());
    QVERIFY(!fenceSent.first().first().toBool());
    QList<RallyPointData> points;
    for (int i = 0; i < count - 1; ++i) {
        auto coordinate = home.atDistanceAndAzimuth(150 + i * 100, 315);
        coordinate.setAltitude(80 + i * 10);
        points.append({coordinate, MAV_FRAME_GLOBAL_RELATIVE_ALT});
    }
    QSignalSpy rallySent(vehicle->rallyPointManager(), &RallyPointManager::sendComplete);
    QVERIFY(rallySent.isValid());
    vehicle->rallyPointManager()->sendToVehicle(points);
    QTRY_COMPARE_WITH_TIMEOUT(rallySent.count(), 1, TestTimeout::longMs());
    QVERIFY(!rallySent.first().first().toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!vehicle->missionManager()->inProgress() && !vehicle->geoFenceManager()->inProgress() &&
                                 !vehicle->rallyPointManager()->inProgress(),
                             TestTimeout::longMs());
}

void PlanStyleUITest::_clickStableItem(const QString& description, const std::function<QQuickItem*()>& resolve)
{
    QPointer<QQuickItem> previous;
    QPointer<QQuickItem> button;
    QTRY_VERIFY2_WITH_TIMEOUT(planStyleResolveStableItem(resolve, previous, button),
                              qPrintable(QStringLiteral("%1: no stable live button; last resolution: %2")
                                             .arg(description, planStyleItemChain(button))),
                              TestTimeout::mediumMs());
    QVERIFY(_clickItemAt(button, 0.5, 0.5, description));
}

void PlanStyleUITest::_clickStableButton(const QString& name)
{
    _clickStableItem(name, [&] { return findVisibleItem(_rootItem, name, 0); });
}

void PlanStyleUITest::_setPlanLayer(QQuickItem* view, int layer)
{
    if (view->property("_useNewPlanPanel").toBool()) {
        const QStringList tabs{QStringLiteral("plan"), QStringLiteral("mission"), QStringLiteral("fence"),
                               QStringLiteral("rally")};
        QVERIFY(layer >= 0 && layer < tabs.size());
        _clickStableButton(QStringLiteral("planPanel_tab_") + tabs[layer]);
        QVERIFY(!QTest::currentTestFailed());
    } else if (view->property("_editingLayer").toInt() != layer) {
        const QStringList groups{QString(), QStringLiteral("missionGroup"), QStringLiteral("fenceGroup"),
                                 QStringLiteral("rallyGroup")};
        QVERIFY(layer > 0 && layer < groups.size());
        _clickStableButton(QStringLiteral("layerSwitcher_activeButton"));
        QVERIFY(!QTest::currentTestFailed());
        _clickStableButton(QStringLiteral("layerSwitcher_choice_") + groups[layer]);
        QVERIFY(!QTest::currentTestFailed());
    }
    QTRY_COMPARE_WITH_TIMEOUT(view->property("_editingLayer").toInt(), layer, TestTimeout::shortMs());
}

void PlanStyleUITest::_checkPlan(QQuickItem* view, quint32 id, const QColor& color, int itemCount, int rallyCount)
{
    auto* map = qvariant_cast<QQuickItem*>(view->property("editorMap"));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY(map && master);
    QVERIFY(!map->property("camera").isValid());
    QCOMPARE(master->planVehicleId(), id);
    const auto contexts = map->findChildren<QObject*>(QStringLiteral("planViewPlanStyleContext"));
    QCOMPARE(contexts.size(), 1);
    auto* context = contexts.first();
    QCOMPARE(context->property("vehicleId").toUInt(), id);
    QVERIFY(context->property("enabled").toBool());
    QCOMPARE(context->property("lineColor").value<QColor>(), color);
    const int layer = view->property("_editingLayer").toInt();
    const qreal missionOpacity = layer == 1 ? 1 : 0.5;
    const qreal fenceOpacity = layer == 2 ? 1 : 0.5;
    const qreal rallyOpacity = layer == 3 ? 1 : 0.5;
    auto* mission = master->missionController();
    planStyleWaitForVisualBindings(map, mission->visualItems(), context);
    QVERIFY(!QTest::currentTestFailed());
    int patternCount = 0;
    int scanCount = 0;
    int landingCount = 0;
    int fixedWingCount = 0;
    int perimeterCount = 0;
    QVariantList expectedOutlinePaths;
    for (int i = 0; i < mission->visualItems()->count(); ++i) {
        auto* item = mission->visualItems()->get(i);
        const bool scan =
            qobject_cast<TransectStyleComplexItem*>(item) || qobject_cast<StructureScanComplexItem*>(item);
        const bool landing = qobject_cast<LandingComplexItem*>(item);
        const bool perimeter = qobject_cast<PerimeterScanComplexItem*>(item);
        scanCount += scan ? 1 : 0;
        landingCount += landing ? 1 : 0;
        fixedWingCount += qobject_cast<FixedWingLandingComplexItem*>(item) ? 1 : 0;
        perimeterCount += perimeter ? 1 : 0;
        patternCount += scan || landing || perimeter ? 1 : 0;
        if (auto* perimeterItem = qobject_cast<PerimeterScanComplexItem*>(item)) {
            expectedOutlinePaths.append(QVariant(perimeterItem->perimeterPolygon()->path()));
        } else if (auto* corridorItem = qobject_cast<CorridorScanComplexItem*>(item)) {
            expectedOutlinePaths.append(corridorItem->corridorBoundaries());
        } else if (auto* transectItem = qobject_cast<TransectStyleComplexItem*>(item)) {
            expectedOutlinePaths.append(QVariant(transectItem->surveyAreaPolygon()->path()));
        } else if (auto* structureItem = qobject_cast<StructureScanComplexItem*>(item)) {
            expectedOutlinePaths.append(QVariant(structureItem->structurePolygon()->path()));
        }
    }
    QCOMPARE(mission->visualItems()->count(), itemCount + patternCount);
    auto* segments = mission->simpleFlightPathSegments();
    const auto named = [map](const QString& name) { return planStyleItems(map, name); };
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planMissionRoute")).size(), segments->count(),
                              TestTimeout::mediumMs());
    QVERIFY(segments->count() > 0);
    QSet<int> matchedSegments;
    for (auto* route : named(QStringLiteral("planMissionRoute"))) {
        QCOMPARE(route->property("vehicleId").toUInt(), id);
        QCOMPARE(planStyleColor(route, QStringLiteral("line.color")), color);
        QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 4.0);
        QCOMPARE(planStyleOpacityBelowMap(route, map), missionOpacity);
        const auto path = route->property("path").toList();
        QCOMPARE(path.size(), 2);
        int matched = -1;
        for (int i = 0; i < segments->count(); ++i) {
            auto* segment = segments->value<FlightPathSegment*>(i);
            if (!matchedSegments.contains(i) && path[0].value<QGeoCoordinate>() == segment->coordinate1() &&
                path[1].value<QGeoCoordinate>() == segment->coordinate2()) {
                matched = i;
                break;
            }
        }
        QVERIFY(matched >= 0);
        matchedSegments.insert(matched);
    }
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planWaypoint")).size(), itemCount - 1 + landingCount * 2,
                              TestTimeout::mediumMs());
    QSet<int> sequences;
    for (auto* marker : named(QStringLiteral("planWaypoint"))) {
        if (!qvariant_cast<VisualMissionItem*>(marker->property("missionItem"))) {
            auto* label = planStyleSource(marker);
            QVERIFY(label);
            QCOMPARE(qvariant_cast<QObject*>(label->property("planStyle")), context);
            QCOMPARE(marker->property("vehicleId").toUInt(), id);
            QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), color);
            QCOMPARE(marker->property("renderedBorderWidth").toReal(), 2.0);
            QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), QColor(Qt::white));
            QCOMPARE(marker->property("renderedTextColor").value<QColor>(), QColor(Qt::black));
            QCOMPARE(planStyleOpacityBelowMap(marker, map), missionOpacity);
            continue;
        }
        QCOMPARE(qvariant_cast<QObject*>(marker->property("planStyle")), context);
        QCOMPARE(marker->property("vehicleId").toUInt(), id);
        QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), color);
        QCOMPARE(marker->property("renderedBorderColor").value<QColor>(), QColor(QStringLiteral("white")));
        QCOMPARE(marker->property("renderedBorderWidth").toReal(), 2.0);
        QCOMPARE(marker->property("renderedTextColor").value<QColor>(), QColor(QStringLiteral("black")));
        QCOMPARE(marker->property("interactive").toBool(), layer == 1);
        const int sequence = marker->property("sequenceNumber").toInt();
        QVERIFY(sequence > 0 && !sequences.contains(sequence));
        sequences.insert(sequence);
        auto* item = qvariant_cast<VisualMissionItem*>(marker->property("missionItem"));
        QVERIFY(item && item->isSimpleItem() && mission->visualItems()->contains(item));
        QCOMPARE(item->sequenceNumber(), sequence);
        QCOMPARE(marker->property("coordinate").value<QGeoCoordinate>(), item->coordinate());
        QCOMPARE(marker->property("checked").toBool(), item->isCurrentItem());
        QCOMPARE(marker->property("index").toInt(), sequence);
        QCOMPARE(marker->property("renderedIndexText").toString(), QString::number(sequence));
        auto* source = planStyleSource(marker);
        QVERIFY(source);
        auto* body = qvariant_cast<QQuickItem*>(source->property("renderedIndicator"));
        QVERIFY(body);
        QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map) && body->width() > 0, TestTimeout::shortMs());
        QCOMPARE(body->width(), source->property("_indicatorRadius").toReal() * 2);
        QCOMPARE(source->property("small").toBool(), !item->isCurrentItem());
        QCOMPARE(source->property("highlightSelected").toBool(), true);
        QCOMPARE(planStyleOpacityBelowMap(body, map), missionOpacity);
        const auto halos = named(QStringLiteral("planWaypointSelectionHalo"));
        const auto ownHalos = planStyleItems(marker, QStringLiteral("planWaypointSelectionHalo"));
        QCOMPARE(ownHalos.size(), 1);
        QVERIFY(halos.contains(ownHalos.first()));
        QCOMPARE(ownHalos.first()->isVisible(), item->isCurrentItem() && marker->isVisible());
        QCOMPARE(ownHalos.first()->width(), body->width() * 2);
    }
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planHomeMarker")).size(), 1, TestTimeout::mediumMs());
    auto* home = named(QStringLiteral("planHomeMarker")).first();
    QCOMPARE(home->property("vehicleId").toUInt(), id);
    QCOMPARE(home->property("coordinate").value<QGeoCoordinate>(), mission->plannedHomePosition());
    auto* house = planStyleSource(home);
    QVERIFY(house && house->inherits("QQuickImage"));
    QCOMPARE(house->property("source").toUrl(), QUrl(QStringLiteral("qrc:///qmlimages/MapHome.svg")));
    const auto accents = planStyleItems(home, QStringLiteral("planHomeAccent"));
    QCOMPARE(accents.size(), 1);
    QVERIFY(accents.first()->isVisible());
    QCOMPARE(planStyleColor(accents.first(), QStringLiteral("border.color")), color);
    QCOMPARE(planStyleOpacityBelowMap(accents.first(), map), missionOpacity);

    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPatternLine")).size(), landingCount, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planLoiterRing")).size(), landingCount, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterFill")).size(), perimeterCount,
                              TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterTraversal")).size(), perimeterCount,
                              TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planPerimeterMarker")).size(), perimeterCount,
                              TestTimeout::mediumMs());
    for (auto* line : named(QStringLiteral("planPatternLine")) + named(QStringLiteral("planPerimeterTraversal"))) {
        QCOMPARE(planStyleColor(line, QStringLiteral("line.color")), color);
        QCOMPARE(QQmlProperty(line, QStringLiteral("line.width")).read().toReal(), 4.0);
        QCOMPARE(planStyleOpacityBelowMap(line, map), missionOpacity);
    }
    for (auto* ring : named(QStringLiteral("planLoiterRing"))) {
        QCOMPARE(planStyleColor(ring, QStringLiteral("border.color")), color);
        QCOMPARE(QQmlProperty(ring, QStringLiteral("border.width")).read().toReal(), 4.0);
        QCOMPARE(ring->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(planStyleOpacityBelowMap(ring, map), missionOpacity);
    }
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planLandingArea")).size(), fixedWingCount, TestTimeout::mediumMs());
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planGlideSlope")).size(), fixedWingCount, TestTimeout::mediumMs());
    for (auto* fill : named(QStringLiteral("planLandingArea")) + named(QStringLiteral("planGlideSlope"))) {
        QCOMPARE(fill->property("color").value<QColor>(), color);
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), 0.0);
        QCOMPARE(planStyleOpacityBelowMap(fill, map), 0.5 * missionOpacity);
        expectedOutlinePaths.append(QVariant(fill->property("path").toList()));
    }
    for (auto* fill : named(QStringLiteral("planPerimeterFill"))) {
        QCOMPARE(fill->property("color").value<QColor>(), color);
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), 0.0);
        QCOMPARE(planStyleOpacityBelowMap(fill, map), 0.08 * missionOpacity);
    }
    for (auto* label : named(QStringLiteral("planPerimeterMarker"))) {
        QCOMPARE(qvariant_cast<QObject*>(label->property("planStyle")), context);
        QCOMPARE(label->property("renderedBodyColor").value<QColor>(), color);
        QCOMPARE(label->property("renderedBorderColor").value<QColor>(), QColor(Qt::white));
        QCOMPARE(label->property("renderedBorderWidth").toReal(), 2.0);
        QCOMPARE(label->property("renderedTextColor").value<QColor>(), QColor(Qt::black));
        QTRY_VERIFY_WITH_TIMEOUT(planStyleRenderMap(map), TestTimeout::shortMs());
        QCOMPARE(planStyleOpacityBelowMap(label, map), missionOpacity);
    }
    QTRY_VERIFY2_WITH_TIMEOUT(
        named(QStringLiteral("planPatternMarker")).size() == scanCount * 2,
        qPrintable(planStyleLookupContext(map, QStringLiteral("planPatternMarker"), scanCount * 2)),
        TestTimeout::mediumMs());
    QTRY_VERIFY2_WITH_TIMEOUT(
        named(QStringLiteral("planPatternOutline")).size() == expectedOutlinePaths.size(),
        qPrintable(planStyleLookupContext(map, QStringLiteral("planPatternOutline"), expectedOutlinePaths.size())),
        TestTimeout::mediumMs());
    for (const auto& expectedPath : expectedOutlinePaths) {
        qsizetype matches = 0;
        for (auto* outline : named(QStringLiteral("planPatternOutline"))) {
            QVERIFY2(outline, "Plan pattern: null outline");
            matches += outline->property("vertices").toList() == expectedPath.toList() ? 1 : 0;
        }
        QVERIFY2(
            matches == 1,
            qPrintable(QStringLiteral("Plan layer=%1: expected exactly one pattern outline for ring %2; matches=%3")
                           .arg(layer)
                           .arg(expectedPath.toList().size())
                           .arg(matches)));
    }
    for (auto* label : named(QStringLiteral("planPatternMarker"))) {
        QVERIFY2(label, "Plan pattern: null label");
        QVERIFY2((qvariant_cast<QObject*>(label->property("planStyle"))) == (context),
                 qPrintable(planStyleItemChain(label)));
        QCOMPARE(qvariant_cast<QObject*>(label->property("planStyle")), context);
        QVERIFY2((label->property("renderedBodyColor").value<QColor>()) == (color),
                 qPrintable(planStyleItemChain(label)));
        QCOMPARE(label->property("renderedBodyColor").value<QColor>(), color);
        QVERIFY2((label->property("renderedBorderColor").value<QColor>()) == (QColor(Qt::white)),
                 qPrintable(planStyleItemChain(label)));
        QCOMPARE(label->property("renderedBorderColor").value<QColor>(), QColor(Qt::white));
        QVERIFY2((label->property("renderedBorderWidth").toReal()) == (2.0), qPrintable(planStyleItemChain(label)));
        QCOMPARE(label->property("renderedBorderWidth").toReal(), 2.0);
        QVERIFY2((label->property("renderedTextColor").value<QColor>()) == (QColor(Qt::black)),
                 qPrintable(planStyleItemChain(label)));
        QCOMPARE(label->property("renderedTextColor").value<QColor>(), QColor(Qt::black));
        QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(label, map), missionOpacity),
                 qPrintable(planStyleOpacityContext(label, map, missionOpacity)));
        QCOMPARE(planStyleOpacityBelowMap(label, map), missionOpacity);
    }
    for (auto* outline : named(QStringLiteral("planPatternOutline"))) {
        QVERIFY2(outline, "Plan pattern: null outline");
        QVERIFY2((outline->property("renderedStrokeColor").value<QColor>()) == (color),
                 qPrintable(planStyleItemChain(outline)));
        QCOMPARE(outline->property("renderedStrokeColor").value<QColor>(), color);
        QVERIFY2((outline->property("renderedStrokeWidth").toReal()) == (3.0), qPrintable(planStyleItemChain(outline)));
        QCOMPARE(outline->property("renderedStrokeWidth").toReal(), 3.0);
        QVERIFY2((outline->property("renderedDashPattern").toList()) == (QVariantList({6.0, 3.0})),
                 qPrintable(planStyleItemChain(outline)));
        QCOMPARE(outline->property("renderedDashPattern").toList(), QVariantList({6.0, 3.0}));
        QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(planStyleSource(outline), map), missionOpacity),
                 qPrintable(planStyleOpacityContext(planStyleSource(outline), map, missionOpacity)));
        QCOMPARE(planStyleOpacityBelowMap(planStyleSource(outline), map), missionOpacity);
        QVERIFY2(outline->z() < 50, qPrintable(planStyleItemChain(outline)));
    }
    for (const auto& name : {QStringLiteral("planTransectFull"), QStringLiteral("planTransectEntry"),
                             QStringLiteral("planTransectExit"), QStringLiteral("planPolyline")}) {
        for (auto* line : named(name)) {
            QVERIFY2(line, "Plan pattern: null line");
            QVERIFY2((planStyleColor(line, QStringLiteral("line.color"))) == (color),
                     qPrintable(planStyleItemChain(line)));
            QCOMPARE(planStyleColor(line, QStringLiteral("line.color")), color);
            QVERIFY2((QQmlProperty(line, QStringLiteral("line.width")).read().toReal()) == (4.0),
                     qPrintable(planStyleItemChain(line)));
            QCOMPARE(QQmlProperty(line, QStringLiteral("line.width")).read().toReal(), 4.0);
            QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(line, map), missionOpacity),
                     qPrintable(planStyleOpacityContext(line, map, missionOpacity)));
            QCOMPARE(planStyleOpacityBelowMap(line, map), missionOpacity);
        }
    }
    for (auto* fill : named(QStringLiteral("planPatternFill")) + named(QStringLiteral("planPolygonFill"))) {
        QVERIFY2(fill, "Plan pattern: null fill");
        const bool flight = fill->property("color").value<QColor>().alpha() == 0;
        QVERIFY2(qFuzzyCompare(planStyleOpacityBelowMap(fill, map), flight ? missionOpacity : 0.5 * missionOpacity),
                 qPrintable(planStyleOpacityContext(fill, map, flight ? missionOpacity : 0.5 * missionOpacity)));
        QCOMPARE(planStyleOpacityBelowMap(fill, map), flight ? missionOpacity : 0.5 * missionOpacity);
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), flight ? 4.0 : 0.0);
        QCOMPARE(
            flight ? planStyleColor(fill, QStringLiteral("border.color")) : fill->property("color").value<QColor>(),
            color);
    }

    auto* fence = master->geoFenceController();
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planFenceFill")).size(),
                              fence->polygons()->count() + fence->circles()->count() + 1, TestTimeout::mediumMs());
    auto* fenceVisuals = findItem(map, QStringLiteral("planView_geoFenceMapVisuals"));
    auto* rallyVisuals = findItem(map, QStringLiteral("planView_rallyPointMapVisuals"));
    QVERIFY(fenceVisuals && rallyVisuals);
    QCOMPARE(qvariant_cast<QObject*>(fenceVisuals->property("planStyle")), context);
    QCOMPARE(qvariant_cast<QObject*>(rallyVisuals->property("planStyle")), context);
    QCOMPARE(fenceVisuals->property("interactive").toBool(), layer == 2);
    QCOMPARE(rallyVisuals->property("interactive").toBool(), layer == 3);
    planStyleCheckBoundaries(map, id, color, fence->polygons()->count(), fence->circles()->count());
    QVERIFY(!QTest::currentTestFailed());
    QSet<QObject*> matchedFences;
    for (auto* fill : named(QStringLiteral("planFenceFill"))) {
        QCOMPARE(fill->opacity(), fenceOpacity);
        QCOMPARE(planStyleOpacityBelowMap(fill, map), fenceOpacity);
        if (!fill->property("inclusion").toBool()) {
            QVERIFY(qAbs(fill->property("color").value<QColor>().alphaF() * planStyleOpacityBelowMap(fill, map) -
                         0.35 * fenceOpacity) < 0.005);
        }
        const auto kind = fill->property("boundaryKind").toString();
        if (kind == QStringLiteral("parameterCircle")) {
            QCOMPARE(fill->property("center").value<QGeoCoordinate>(), mission->plannedHomePosition());
            QCOMPARE(fill->property("radius").toReal(), fence->paramCircularFence());
            continue;
        }
        QObject* matched = nullptr;
        auto* model = kind == QStringLiteral("polygon") ? fence->polygons() : fence->circles();
        for (int i = 0; i < model->count(); ++i) {
            auto* candidate = model->get(i);
            const bool geometryMatches =
                kind == QStringLiteral("polygon")
                    ? fill->property("path").toList() == candidate->property("dragPath").toList()
                    : fill->property("center") == candidate->property("center");
            if (!matchedFences.contains(candidate) && geometryMatches &&
                fill->property("inclusion") == candidate->property("inclusion")) {
                matched = candidate;
                break;
            }
        }
        QVERIFY(matched);
        matchedFences.insert(matched);
    }
    for (auto* outline : named(QStringLiteral("planKeepInOutline")) + named(QStringLiteral("planKeepOutOutline"))) {
        QCOMPARE(planStyleOpacityBelowMap(planStyleSource(outline), map), fenceOpacity);
    }
    auto* rally = master->rallyPointController();
    QCOMPARE(rally->points()->count(), rallyCount);
    QTRY_COMPARE_WITH_TIMEOUT(named(QStringLiteral("planRallyPoint")).size(), rallyCount, TestTimeout::mediumMs());
    QSet<QObject*> matchedPoints;
    for (auto* marker : named(QStringLiteral("planRallyPoint"))) {
        planStyleCheckRally(map, marker, id, color);
        QVERIFY(!QTest::currentTestFailed());
        auto* point = qvariant_cast<QObject*>(marker->property("rallyPointObject"));
        QVERIFY(point && rally->points()->contains(point) && !matchedPoints.contains(point));
        matchedPoints.insert(point);
        QCOMPARE(marker->property("coordinate"), point->property("coordinate"));
        QCOMPARE(marker->property("checked").toBool(), layer == 3 && rally->currentRallyPoint() == point);
        auto* loader = planStyleSource(marker);
        auto* source = qvariant_cast<QQuickItem*>(loader->property("item"));
        QVERIFY(source);
        QCOMPARE(source->property("interactive").toBool(), layer == 3);
        QCOMPARE(planStyleOpacityBelowMap(source, map), rallyOpacity);
    }
}

void PlanStyleUITest::_insertPatterns(PlanMasterController* master)
{
    auto* mission = master->missionController();
    const auto base = mission->plannedHomePosition().atDistanceAndAzimuth(450, 270);
    int offset = 0;
    for (const auto& name : {QString::fromUtf8(SurveyComplexItem::canonicalName),
                             QString::fromUtf8(CorridorScanComplexItem::canonicalName),
                             QString::fromUtf8(StructureScanComplexItem::canonicalName),
                             QString::fromUtf8(FixedWingLandingComplexItem::canonicalName),
                             QString::fromUtf8(VTOLLandingComplexItem::canonicalName),
                             QString::fromUtf8(PerimeterScanComplexItem::canonicalName)}) {
        auto* item = mission->insertComplexMissionItem(name, base, mission->visualItems()->count(), false);
        QVERIFY2(item, qPrintable(QStringLiteral("pattern fixture: could not insert %1").arg(name)));
        const auto coordinate = base.atDistanceAndAzimuth(offset, 180);
        if (auto* landing = qobject_cast<LandingComplexItem*>(item)) {
            landing->setLandingCoordinate(coordinate);
            landing->useLoiterToAlt()->setRawValue(true);
        } else if (auto* perimeter = qobject_cast<PerimeterScanComplexItem*>(item)) {
            perimeter->perimeterPolygon()->appendVertices(PlanStylePatternTest::square(coordinate));
        } else {
            PlanStylePatternTest::seed(item, coordinate);
        }
        offset += 300;
    }
}

void PlanStyleUITest::_editPatterns(QQuickItem* view)
{
    auto* map = qvariant_cast<QQuickItem*>(view->property("editorMap"));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY2(map && master, "pattern editor: missing editorMap or PlanMasterController");
    auto* mission = master->missionController();
    SurveyComplexItem* survey = nullptr;
    CorridorScanComplexItem* corridor = nullptr;
    StructureScanComplexItem* structure = nullptr;
    for (int i = 0; i < mission->visualItems()->count(); ++i) {
        auto* item = mission->visualItems()->get(i);
        if (auto* surveyItem = qobject_cast<SurveyComplexItem*>(item)) {
            survey = surveyItem;
        } else if (auto* corridorItem = qobject_cast<CorridorScanComplexItem*>(item)) {
            corridor = corridorItem;
        } else if (auto* structureItem = qobject_cast<StructureScanComplexItem*>(item)) {
            structure = structureItem;
        }
    }
    QVERIFY2(survey && corridor && structure, "pattern editor: missing survey, corridor or structure model");
    _setPlanLayer(view, 1);
    QVERIFY2(!QTest::currentTestFailed(), "pattern editor: could not select the mission layer");
    planStyleWaitForVisualBindings(map, mission->visualItems(),
                                   map->findChild<QObject*>(QStringLiteral("planViewPlanStyleContext")));
    QVERIFY(!QTest::currentTestFailed());
    const auto belongsTo = [](QQuickItem* item, QObject* geometry) {
        auto* context = qmlContext(item);
        auto* visuals = context ? qvariant_cast<QObject*>(context->contextProperty(QStringLiteral("_root"))) : nullptr;
        return visuals && (qvariant_cast<QObject*>(visuals->property("mapPolygon")) == geometry ||
                           qvariant_cast<QObject*>(visuals->property("mapPolyline")) == geometry);
    };
    const auto dragFor = [&](QObject* geometry, const QString& kind) -> QQuickItem* {
        for (auto* drag : map->childItems()) {
            if (drag->metaObject()->indexOfProperty("itemCoordinate") < 0 || !belongsTo(drag, geometry)) {
                continue;
            }
            auto* handle = qvariant_cast<QQuickItem*>(drag->property("itemIndicator"));
            if (!handle) {
                continue;
            }
            const auto vertex =
                drag->property(qobject_cast<QGCMapPolyline*>(geometry) ? "polylineVertex" : "polygonVertex");
            if ((kind == QStringLiteral("vertex") && vertex.isValid() && vertex.toInt() == 0) ||
                (kind == QStringLiteral("center") && !vertex.isValid() && handle->z() == 53) ||
                (kind == QStringLiteral("radius") && !vertex.isValid() && handle->z() == 52)) {
                return drag;
            }
        }
        return nullptr;
    };
    const auto markerFor = [&](QObject* geometry, const QString& kind) -> QQuickItem* {
        auto* drag = dragFor(geometry, kind);
        return drag ? qvariant_cast<QQuickItem*>(drag->property("itemIndicator")) : nullptr;
    };
    const auto geometryName = [&](QObject* geometry) {
        if (geometry == survey->surveyAreaPolygon()) {
            return QStringLiteral("survey");
        }
        if (geometry == corridor->corridorPolyline()) {
            return QStringLiteral("corridor");
        }
        return geometry == structure->structurePolygon() ? QStringLiteral("structure") : QStringLiteral("unknown");
    };
    const auto diagnostic = [&](const QString& description) {
        const auto projectionMismatch = planStyleEditorProjectionMismatch(map);
        QStringList handles;
        for (auto* candidate : map->childItems()) {
            if (candidate->metaObject()->indexOfProperty("itemCoordinate") < 0) {
                continue;
            }
            auto* indicator = qvariant_cast<QQuickItem*>(candidate->property("itemIndicator"));
            handles.append(QStringLiteral("survey=%1 corridor=%2 structure=%3 polygonVertex=%4 polylineVertex=%5 "
                                          "dragZ=%6 handleZ=%7; %8")
                               .arg(belongsTo(candidate, survey->surveyAreaPolygon()))
                               .arg(belongsTo(candidate, corridor->corridorPolyline()))
                               .arg(belongsTo(candidate, structure->structurePolygon()))
                               .arg(candidate->property("polygonVertex").toString(),
                                    candidate->property("polylineVertex").toString())
                               .arg(candidate->z())
                               .arg(indicator ? indicator->z() : -1)
                               .arg(planStyleItemChain(indicator)));
        }
        return QStringLiteral("%1: projection=%2 currentSequence=%3 layer=%4; %5; handles count=%6 [%7]")
            .arg(description)
            .arg(projectionMismatch.isEmpty() ? QStringLiteral("settled") : projectionMismatch)
            .arg(mission->currentPlanViewItem() ? mission->currentPlanViewItem()->sequenceNumber() : -1)
            .arg(view->property("_editingLayer").toInt())
            .arg(planStyleLookupContext(map, QStringLiteral("planPatternEntryMarker"), 3))
            .arg(handles.size())
            .arg(handles.join(QStringLiteral("; ")));
    };
    const auto prepare = [&](const QString& description, const auto& resolve) {
        QVERIFY2(map->setProperty("zoomLevel", 17), qPrintable(diagnostic(description + " zoom")));
        // Resolve by identity before projection: a newly selected pattern's handles may still be outside the viewport.
        QPointer<QQuickItem> previous;
        QPointer<QQuickItem> marker;
        QTRY_VERIFY2_WITH_TIMEOUT(planStyleResolveStableItem(resolve, previous, marker) &&
                                      marker->property("coordinate").value<QGeoCoordinate>().isValid(),
                                  qPrintable(diagnostic(description + " live stable resolve")),
                                  TestTimeout::mediumMs());
        QVERIFY2(map->setProperty("center", marker->property("coordinate")),
                 qPrintable(diagnostic(description + " center")));
        QTRY_VERIFY2_WITH_TIMEOUT(
            planStyleEditorProjected(map) && planStyleResolveStableItem(resolve, previous, marker) &&
                marker->isVisible() && planStyleSource(marker) && planStyleSource(marker)->isVisible() &&
                planStyleMapItemProjected(map, marker),
            qPrintable(diagnostic(description + " live stable projection")), TestTimeout::mediumMs());
    };
    const auto selectPattern = [&](VisualMissionItem* pattern) {
        const auto resolve = [&]() -> QQuickItem* {
            for (auto* marker : planStyleItems(map, QStringLiteral("planPatternEntryMarker"))) {
                auto* label = planStyleSource(marker);
                if (label && label->property("index").toInt() == pattern->sequenceNumber()) {
                    return marker;
                }
            }
            return nullptr;
        };
        const QString description = QStringLiteral("%1 entry marker sequence=%2")
                                        .arg(QString::fromLatin1(pattern->metaObject()->className()))
                                        .arg(pattern->sequenceNumber());
        prepare(description, resolve);
        QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(description)));
        QPoint position;
        {
            auto* marker = resolve();
            QVERIFY2(marker && planStyleSource(marker), qPrintable(diagnostic(description)));
            auto* label = planStyleSource(marker);
            position = marker->mapToScene(marker->property("anchorPoint").toPointF()).toPoint();
            QVERIFY2(planStyleMouseItemAt(label, position), qPrintable(diagnostic(description)));
        }
        QTest::mouseClick(_window, Qt::LeftButton, Qt::NoModifier, position);
        QTRY_VERIFY2_WITH_TIMEOUT((mission->currentPlanViewItem()) == (pattern), qPrintable(diagnostic(description)),
                                  TestTimeout::shortMs());
    };
    const auto dragGeometry = [&](QObject* geometry, const QString& kind) {
        const auto resolve = [&] { return markerFor(geometry, kind); };
        const QString description = geometryName(geometry) + ' ' + kind + QStringLiteral(" handle");
        prepare(description, resolve);
        QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(description)));
        const auto before = geometry->property("path").toList();
        QSignalSpy committed(geometry, SIGNAL(pathChanged()));
        QVERIFY2(committed.isValid(), qPrintable(diagnostic(description)));
        QPoint start;
        {
            auto* marker = resolve();
            auto* drag = dragFor(geometry, kind);
            QVERIFY2(drag && drag->isVisible(), qPrintable(diagnostic(description + " drag overlay visibility")));
            QVERIFY2(marker && drag && planStyleSource(marker), qPrintable(diagnostic(description)));
            start = marker->mapToScene(marker->property("anchorPoint").toPointF()).toPoint();
            QVERIFY2(planStyleMouseItemAt(drag, start), qPrintable(diagnostic(description)));
            for (auto* other : map->childItems()) {
                if (other != drag && other->metaObject()->indexOfProperty("itemCoordinate") >= 0) {
                    QVERIFY2(!planStyleMouseItemAt(other, start),
                             qPrintable(diagnostic(description + " competing drag: " + planStyleItemChain(other))));
                }
            }
            for (auto* outline : planStyleItems(map, QStringLiteral("planPatternOutline"))) {
                QVERIFY2(outline->z() < marker->z(), qPrintable(diagnostic(description)));
                QCOMPARE(outline->acceptedMouseButtons(), Qt::NoButton);
                QVERIFY2(planStyleSource(outline), qPrintable(diagnostic(description + " outline source")));
                QCOMPARE(planStyleSource(outline)->acceptedMouseButtons(), Qt::NoButton);
            }
        }
        const int threshold = QGuiApplication::styleHints()->startDragDistance() + 4;
        const QPoint activation = start + QPoint(threshold + 4, threshold + 4);
        const QPoint delta(60, 40);
        QTest::mousePress(_window, Qt::LeftButton, Qt::NoModifier, start);
        bool pressed = true;
        const auto releasePointer = qScopeGuard([&] {
            if (pressed) {
                QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activation + delta);
            }
        });
        QTest::mouseMove(_window, start + QPoint(threshold, threshold));
        QTest::mouseMove(_window, activation);
        QTRY_VERIFY2_WITH_TIMEOUT(([&] {
                                      if (!planStyleRenderMap(map)) {
                                          return false;
                                      }
                                      auto* drag = dragFor(geometry, kind);
                                      return drag && drag->property("_dragStartSignalled").toBool();
                                  }()),
                                  qPrintable(diagnostic(description + " drag activation")), TestTimeout::shortMs());
        QPointF activeAnchor;
        {
            auto* marker = resolve();
            QVERIFY2(marker && planStyleSource(marker), qPrintable(diagnostic(description)));
            activeAnchor = marker->mapToScene(marker->property("anchorPoint").toPointF());
        }
        for (int i = 1; i <= 10; ++i) {
            QTest::mouseMove(_window, activation + delta * i / 10);
        }
        const auto previewFollowsPointer = [&] {
            if (!planStyleRenderMap(map) || geometry->property("path").toList() == before || !resolve()) {
                return false;
            }
            if (qobject_cast<QGCMapPolyline*>(geometry)) {
                const auto lines = planStyleItems(map, QStringLiteral("planPolyline"));
                return lines.size() == 1 &&
                       lines.first()->property("path").toList() == geometry->property("path").toList();
            }
            for (auto* outline : planStyleItems(map, QStringLiteral("planPatternOutline"))) {
                if (outline->property("vertices").toList() == geometry->property("path").toList()) {
                    return !outline->property("_projectionPending").toBool();
                }
            }
            return false;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(previewFollowsPointer(),
                                  qPrintable(diagnostic(description + " preview follows held pointer")),
                                  TestTimeout::shortMs());
        QPointF heldAnchor;
        {
            auto* marker = resolve();
            QVERIFY2(marker && planStyleSource(marker), qPrintable(diagnostic(description + " held anchor")));
            heldAnchor = marker->mapToScene(marker->property("anchorPoint").toPointF());
        }
        const auto heldPath = geometry->property("path").toList();
        const auto commitsWhileHeld = committed.count();
        if (qobject_cast<QGCMapPolygon*>(geometry)) {
            for (auto* fill : planStyleItems(map, QStringLiteral("planPatternFill"))) {
                if (fill->property("path").toList() == heldPath) {
                    QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), 0.0);
                }
            }
        }
        if (qobject_cast<QGCMapPolyline*>(geometry)) {
            for (auto* line : planStyleItems(map, QStringLiteral("planPolyline"))) {
                QCOMPARE(planStyleColor(line, QStringLiteral("line.color")),
                         QGCCorePlugin::instance()->planStyle()->vehicleColor(master->planVehicleId()));
                QCOMPARE(QQmlProperty(line, QStringLiteral("line.width")).read().toReal(), 4.0);
            }
            for (auto* fill : planStyleItems(map, QStringLiteral("planPolygonFill"))) {
                if (fill->property("path").toList() == corridor->surveyAreaPolygon()->path()) {
                    QVERIFY2(!fill->isVisible(), qPrintable(diagnostic(description)));
                }
            }
            for (auto* outline : planStyleItems(map, QStringLiteral("planPatternOutline"))) {
                if (corridor->corridorBoundaries().contains(QVariant(outline->property("vertices").toList()))) {
                    QVERIFY2(!outline->isVisible(), qPrintable(diagnostic(description)));
                }
            }
            for (auto* marker : planStyleItems(map, QStringLiteral("planPatternEntryMarker")) +
                                    planStyleItems(map, QStringLiteral("planPatternExitMarker"))) {
                auto* label = planStyleSource(marker);
                const int index = label ? label->property("index").toInt() : -1;
                if (index == corridor->sequenceNumber() || index == corridor->lastSequenceNumber()) {
                    QVERIFY2(!marker->isVisible(), qPrintable(diagnostic(description)));
                }
            }
        }
        pressed = false;
        QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activation + delta);
        QVERIFY2(heldPath != before, qPrintable(diagnostic(description)));
        if (kind != QStringLiteral("radius")) {
            QVERIFY2(commitsWhileHeld == 0,
                     qPrintable(diagnostic(description + " committed path changed before release")));
            QCOMPARE(commitsWhileHeld, 0);
        }
        QVERIFY2((heldAnchor - activeAnchor - delta).manhattanLength() < 5, qPrintable(diagnostic(description)));
        QTRY_VERIFY2_WITH_TIMEOUT(!geometry->property("vertexDrag").toBool() && planStyleEditorProjected(map),
                                  qPrintable(diagnostic(description)), TestTimeout::mediumMs());
    };
    const auto split = [&](QObject* geometry) {
        const auto resolve = [&]() -> QQuickItem* {
            for (auto* item : map->childItems()) {
                if (belongsTo(item, geometry) && item->property("vertexIndex").isValid() &&
                    item->property("vertexIndex").toInt() == 0 && planStyleSource(item) &&
                    planStyleSource(item)->metaObject()->indexOfSignal("clicked()") >= 0) {
                    return item;
                }
            }
            return nullptr;
        };
        const QString description = geometryName(geometry) + QStringLiteral(" split handle vertex=0");
        prepare(description, resolve);
        QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(description)));
        const int count = geometry->property("count").toInt();
        QPoint position;
        {
            auto* item = resolve();
            QVERIFY2(item && planStyleSource(item), qPrintable(diagnostic(description)));
            QVERIFY2(item->z() >= 52, qPrintable(diagnostic(description)));
            auto* source = planStyleSource(item);
            position = source->mapToScene(QPointF(source->width() / 2, source->height() / 2)).toPoint();
            QVERIFY2(planStyleMouseItemAt(source, position), qPrintable(diagnostic(description)));
        }
        QTest::mouseClick(_window, Qt::LeftButton, Qt::NoModifier, position);
        QTRY_VERIFY2_WITH_TIMEOUT((geometry->property("count").toInt()) == (count + 1),
                                  qPrintable(diagnostic(description)), TestTimeout::shortMs());
    };
    selectPattern(survey);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    dragGeometry(survey->surveyAreaPolygon(), QStringLiteral("vertex"));
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    dragGeometry(survey->surveyAreaPolygon(), QStringLiteral("center"));
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    split(survey->surveyAreaPolygon());
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    _clickStableItem(QStringLiteral("Circular pattern tools"),
                     [&] { return planStyleTab(map, QStringLiteral("Circular")); });
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("survey Circular tool lookup/click"))));
    QTRY_VERIFY2_WITH_TIMEOUT((survey->surveyAreaPolygon()->count()) == (16),
                              qPrintable(diagnostic(QStringLiteral("survey vertex count after Circular/Trace"))),
                              TestTimeout::shortMs());
    dragGeometry(survey->surveyAreaPolygon(), QStringLiteral("radius"));
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    _clickStableItem(QStringLiteral("Trace pattern"), [&] { return planStyleTab(map, QStringLiteral("Trace")); });
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("survey Trace tool lookup/click"))));
    QTRY_VERIFY2_WITH_TIMEOUT(survey->surveyAreaPolygon()->traceMode(),
                              qPrintable(diagnostic(QStringLiteral("survey trace starts"))), TestTimeout::shortMs());
    const auto viewport = map->property("centerViewport").toRectF();
    for (const auto& fraction : {QPointF(0.3, 0.35), QPointF(0.65, 0.4), QPointF(0.5, 0.7)}) {
        const auto position = map->mapToScene(viewport.topLeft() + QPointF(viewport.width() * fraction.x(),
                                                                           viewport.height() * fraction.y()))
                                  .toPoint();
        QTest::mouseClick(_window, Qt::LeftButton, Qt::NoModifier, position);
    }
    QTRY_VERIFY2_WITH_TIMEOUT((survey->surveyAreaPolygon()->count()) == (3),
                              qPrintable(diagnostic(QStringLiteral("survey vertex count after Circular/Trace"))),
                              TestTimeout::shortMs());
    _clickStableItem(QStringLiteral("Finish pattern trace"),
                     [&] { return planStyleTab(map, QStringLiteral("Done Tracing")); });
    QVERIFY2(!QTest::currentTestFailed(),
             qPrintable(diagnostic(QStringLiteral("survey Done Tracing tool lookup/click"))));
    QTRY_VERIFY2_WITH_TIMEOUT(!survey->surveyAreaPolygon()->traceMode(),
                              qPrintable(diagnostic(QStringLiteral("survey trace finishes"))), TestTimeout::shortMs());
    selectPattern(structure);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    dragGeometry(structure->structurePolygon(), QStringLiteral("vertex"));
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    selectPattern(corridor);
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    dragGeometry(corridor->corridorPolyline(), QStringLiteral("vertex"));
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    split(corridor->corridorPolyline());
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("pattern edit operation failed"))));
    const auto beforeWidth = corridor->corridorBoundaries();
    QPointer<QQuickItem> field;
    const auto findWidth = [&](QQuickItem* root, auto&& self) -> QQuickItem* {
        if (root->isVisible() && qvariant_cast<QObject*>(root->property("fact")) == corridor->corridorWidth() &&
            root->metaObject()->indexOfProperty("text") >= 0) {
            return root;
        }
        for (auto* child : root->childItems()) {
            if (auto* match = self(child, self)) {
                return match;
            }
        }
        return nullptr;
    };
    QPointer<QQuickItem> previousField;
    QTRY_VERIFY2_WITH_TIMEOUT(
        planStyleResolveStableItem([&] { return findWidth(view, findWidth); }, previousField, field),
        qPrintable(diagnostic(QStringLiteral("corridor width Fact field live stable lookup"))), TestTimeout::shortMs());
    QVERIFY2(scrollIntoView(field, view->property("_useNewPlanPanel").toBool()
                                       ? QStringLiteral("planPanel_inspectorFlickable")
                                       : QStringLiteral("planView_planTree")),
             qPrintable(diagnostic(QStringLiteral("corridor width field scroll"))));
    _clickStableItem(QStringLiteral("Corridor width"), [&] { return findWidth(view, findWidth); });
    QVERIFY2(!QTest::currentTestFailed(), qPrintable(diagnostic(QStringLiteral("corridor width field click"))));
    QTest::keyClick(_window, Qt::Key_A, Qt::ControlModifier);
    for (auto key : {Qt::Key_1, Qt::Key_2, Qt::Key_0}) {
        QTest::keyClick(_window, key);
    }
    QTest::keyClick(_window, Qt::Key_Return);
    QTRY_VERIFY2_WITH_TIMEOUT(corridor->corridorBoundaries() != beforeWidth,
                              qPrintable(diagnostic(QStringLiteral("corridor width updates boundary rings"))),
                              TestTimeout::shortMs());
    for (auto* line : planStyleItems(map, QStringLiteral("planPolyline"))) {
        QVERIFY2(line->property("path").toList() == corridor->corridorPolyline()->path(),
                 qPrintable(diagnostic(QStringLiteral("corridor centreline model path"))));
        QCOMPARE(line->property("path").toList(), corridor->corridorPolyline()->path());
        QVERIFY2(line->property("path").toList().first() != line->property("path").toList().last(),
                 qPrintable(diagnostic(QStringLiteral("corridor centreline remains open"))));
    }
    mission->setCurrentPlanViewSeqNum(1, true);
    QTRY_VERIFY2_WITH_TIMEOUT(!survey->isCurrentItem() && !corridor->isCurrentItem() && !structure->isCurrentItem(),
                              qPrintable(diagnostic(QStringLiteral("deselect all patterns"))), TestTimeout::shortMs());
}

void PlanStyleUITest::_editPlan(QQuickItem* view)
{
    auto* map = qvariant_cast<QQuickItem*>(view->property("editorMap"));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY(map && master);
    auto* mission = master->missionController();
    auto* fence = master->geoFenceController();
    auto* rally = master->rallyPointController();
    const QColor color = QGCCorePlugin::instance()->planStyle()->vehicleColor(master->planVehicleId());
    planStyleWaitForVisualBindings(map, mission->visualItems(),
                                   map->findChild<QObject*>(QStringLiteral("planViewPlanStyleContext")));
    QVERIFY(!QTest::currentTestFailed());
    const auto dragFor = [map](QQuickItem* marker) -> QQuickItem* {
        if (!marker) {
            return nullptr;
        }
        for (auto* item : map->childItems()) {
            if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0 &&
                qvariant_cast<QQuickItem*>(item->property("itemIndicator")) == marker) {
                return item;
            }
        }
        return nullptr;
    };
    const auto preparePointer = [&](const auto& resolve, bool sourceCenter = false) {
        QVERIFY(map->setProperty("zoomLevel", 17));
        QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
        QPointer<QQuickItem> previous;
        QPointer<QQuickItem> marker;
        QTRY_VERIFY2_WITH_TIMEOUT(planStyleResolveStableItem(resolve, previous, marker) && marker->isVisible(),
                                  "Plan pointer target did not resolve to a stable live item", TestTimeout::shortMs());
        const auto coordinate = marker->property("coordinate");
        QVERIFY(map->setProperty("center", coordinate));
        // Rendering processes pending model edits; resolve Repeater handles after every wait.
        QTRY_VERIFY2_WITH_TIMEOUT(planStyleEditorProjected(map) &&
                                      planStyleResolveStableItem(resolve, previous, marker) && marker->isVisible() &&
                                      planStyleMapItemProjected(map, marker),
                                  "Plan pointer target did not stay live and projected", TestTimeout::mediumMs());
        auto* source = planStyleSource(marker);
        QVERIFY(source);
        const auto position = sourceCenter ? source->mapToScene(QPointF(source->width() / 2, source->height() / 2))
                                           : marker->mapToScene(marker->property("anchorPoint").toPointF());
        QVERIFY(map->property("centerViewport").toRectF().contains(map->mapFromScene(position)));
        QVERIFY(position.x() >= 0 && position.x() < _window->width() && position.y() >= 0 &&
                position.y() < _window->height());
        QVERIFY(planStyleMouseItemAt(source, position) || planStyleMouseItemAt(dragFor(marker), position));
        for (auto* item : map->childItems()) {
            if (item->metaObject()->indexOfProperty("itemCoordinate") >= 0 && item != dragFor(marker)) {
                QVERIFY2(!planStyleMouseItemAt(item, position), "A competing drag overlay accepts the target click");
            }
        }
    };
    const auto clickPoint = [&](const auto& resolve) {
        preparePointer(resolve, true);
        QVERIFY(!QTest::currentTestFailed());
        QPoint position;
        {
            auto* marker = resolve();
            QVERIFY(marker);
            auto* source = planStyleSource(marker);
            QVERIFY(source);
            position = source->mapToScene(QPointF(source->width() / 2, source->height() / 2)).toPoint();
        }
        // No polish wait between resolving a rebuildable handle and delivering its click.
        QTest::mouseClick(_window, Qt::LeftButton, Qt::NoModifier, position);
    };
    const auto dragPoint = [&](const auto& resolve, QObject* point, Fact* altitude) {
        preparePointer(resolve);
        QVERIFY(!QTest::currentTestFailed());
        QPointer<QQuickItem> marker = resolve();
        QVERIFY(marker);
        QPointer<QQuickItem> drag = dragFor(marker);
        QVERIFY(drag && drag->isVisible());
        QPointer<QQuickItem> source = planStyleSource(marker);
        QVERIFY(source);
        QVERIFY(drag->width() > 0 && drag->height() > 0);
        QCOMPARE(drag->property("_itemIndicatorWidth").toReal(), marker->width());
        QCOMPARE(drag->property("_itemIndicatorHeight").toReal(), marker->height());
        const auto before = point->property("coordinate").value<QGeoCoordinate>();
        QVERIFY2(before.isValid() && altitude && qIsFinite(altitude->rawValue().toDouble()),
                 "Plan point drag requires a valid coordinate and a finite altitude Fact");
        const auto beforeAltitude = altitude->rawValue().toDouble();
        const auto sameCoordinateAltitude = [&](const QGeoCoordinate& coordinate) {
            return qIsNaN(before.altitude()) ? qIsNaN(coordinate.altitude())
                                             : coordinate.altitude() == before.altitude();
        };
        QVERIFY2(sameCoordinateAltitude(drag->property("itemCoordinate").value<QGeoCoordinate>()),
                 "The selected drag item must start with its model's coordinate altitude");
        const auto start = marker->mapToScene(marker->property("anchorPoint").toPointF()).toPoint();
        const QPoint delta(-70, 50);
        const int activationDistance = QGuiApplication::styleHints()->startDragDistance() + 4;
        const QPoint activePointer = start + QPoint(-activationDistance - 4, activationDistance + 4);
        QTest::mousePress(_window, Qt::LeftButton, Qt::NoModifier, start);
        // The threshold-crossing event rebases Qt's drag; the next move activates it.
        QTest::mouseMove(_window, start + QPoint(-activationDistance, activationDistance));
        QTest::mouseMove(_window, activePointer);
        const bool active = waitForCondition(
            [&] {
                return planStyleRenderMap(map) && marker && drag && source &&
                       drag->property("_dragStartSignalled").toBool() && planStyleMapItemProjected(map, marker);
            },
            TestTimeout::shortMs(), QStringLiteral("Plan point drag active and projected"));
        if (!active) {
            QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activePointer);
            QVERIFY(active);
        }
        const auto activeAnchor = marker->mapToScene(marker->property("anchorPoint").toPointF());
        for (int i = 1; i <= 10; ++i) {
            QTest::mouseMove(_window, activePointer + delta * i / 10);
        }
        const bool moved = waitForCondition(
            [&] {
                return planStyleRenderMap(map) && marker && drag && source &&
                       point->property("coordinate").value<QGeoCoordinate>().distanceTo(before) > 1 &&
                       planStyleMapItemProjected(map, marker);
            },
            TestTimeout::shortMs(), QStringLiteral("Plan point moved while held"));
        if (!moved || !marker || !drag || !source) {
            QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activePointer + delta);
            QVERIFY(moved);
            QVERIFY(marker && drag && source);
        }
        const auto held = point->property("coordinate").value<QGeoCoordinate>();
        const auto heldDragCoordinate = drag->property("itemCoordinate").value<QGeoCoordinate>();
        const auto heldAltitude = altitude->rawValue().toDouble();
        const auto heldAnchor = marker->mapToScene(marker->property("anchorPoint").toPointF());
        QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, activePointer + delta);
        QVERIFY(moved);
        QVERIFY((heldAnchor - activeAnchor - delta).manhattanLength() < 5);
        // SimpleMissionItem coordinates are 2D; altitude lives in a separate Fact. Rally coordinates are 3D.
        QVERIFY(sameCoordinateAltitude(held));
        QVERIFY(sameCoordinateAltitude(heldDragCoordinate));
        QVERIFY(sameCoordinateAltitude(point->property("coordinate").value<QGeoCoordinate>()));
        QCOMPARE(heldAltitude, beforeAltitude);
        QCOMPARE(altitude->rawValue().toDouble(), beforeAltitude);
        QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
        marker = resolve();
        source = planStyleSource(marker);
        QVERIFY(marker && source);
        const auto end = marker->mapToScene(marker->property("anchorPoint").toPointF());
        QVERIFY((end - activeAnchor - delta).manhattanLength() < 5);
        QVERIFY((end - heldAnchor).manhattanLength() < 5);
    };

    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    mission->setCurrentPlanViewSeqNum(1, true);
    auto* waypoint = mission->visualItems()->value<SimpleMissionItem*>(2);
    QVERIFY2(waypoint && waypoint->command() == MAV_CMD_NAV_WAYPOINT && waypoint->specifiesAltitude() &&
                 qIsFinite(waypoint->altitude()->rawValue().toDouble()),
             "Mission drag fixture must select a NAV_WAYPOINT with a finite altitude Fact");
    QVERIFY(qIsNaN(waypoint->coordinate().altitude()));
    const auto waypointMarkerFor = [&]() -> QQuickItem* {
        for (auto* marker : planStyleItems(map, QStringLiteral("planWaypoint"))) {
            if (qvariant_cast<QObject*>(marker->property("missionItem")) == waypoint) {
                return marker;
            }
        }
        return nullptr;
    };
    clickPoint(waypointMarkerFor);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_COMPARE_WITH_TIMEOUT(mission->currentPlanViewItem(), waypoint, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(dragFor(waypointMarkerFor()), TestTimeout::shortMs());
    {
        auto* waypointMarker = waypointMarkerFor();
        QVERIFY(waypointMarker);
        const auto waypointHalos = planStyleItems(waypointMarker, QStringLiteral("planWaypointSelectionHalo"));
        QCOMPARE(waypointHalos.size(), 1);
        QVERIFY(waypointHalos.first()->isVisible());
        QCOMPARE(waypointMarker->property("renderedBodyColor").value<QColor>(), color);
    }
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("planPanel_inspectorLoader")));
    dragPoint(waypointMarkerFor, waypoint, waypoint->altitude());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(mission->visualItems()->count(), 10);

    _setPlanLayer(view, 3);
    QVERIFY(!QTest::currentTestFailed());
    rally->setCurrentRallyPoint(rally->points()->get(0));
    auto* point = rally->points()->value<RallyPoint*>(1);
    QVERIFY(point);
    const auto rallyMarkerFor = [&]() -> QQuickItem* {
        for (auto* marker : planStyleItems(map, QStringLiteral("planRallyPoint"))) {
            if (qvariant_cast<QObject*>(marker->property("rallyPointObject")) == point) {
                return marker;
            }
        }
        return nullptr;
    };
    clickPoint(rallyMarkerFor);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_COMPARE_WITH_TIMEOUT(rally->currentRallyPoint(), static_cast<QObject*>(point), TestTimeout::shortMs());
    {
        auto* rallyMarker = rallyMarkerFor();
        QVERIFY(rallyMarker);
        QVERIFY(rallyMarker->property("checked").toBool());
        const auto rallyHalos = planStyleItems(rallyMarker, QStringLiteral("planRallySelectionHalo"));
        QCOMPARE(rallyHalos.size(), 1);
        QVERIFY(rallyHalos.first()->isVisible());
    }
    QCOMPARE(point->altitudeFrame(), QGroundControlQmlGlobal::AltitudeFrameRelative);
    dragPoint(rallyMarkerFor, point, point->altitude());
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(rally->points()->count(), 3);
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    {
        auto* rallyMarker = rallyMarkerFor();
        QVERIFY(rallyMarker && planStyleSource(rallyMarker));
        auto* rallyDrag = dragFor(rallyMarker);
        QVERIFY(rallyDrag && !rallyDrag->isVisible());
        auto* rallySource = qvariant_cast<QQuickItem*>(planStyleSource(rallyMarker)->property("item"));
        QVERIFY(rallySource && !rallySource->property("interactive").toBool());
        QVERIFY(!planStyleMouseItemAt(rallySource, rallySource->mapToScene(QPointF(0, 0))));
    }

    _setPlanLayer(view, 2);
    QVERIFY(!QTest::currentTestFailed());
    const bool tabbed = view->property("_useNewPlanPanel").toBool();
    bool fenceSelected = false;
    const auto selectFenceRow = [&](const QString& key) {
        if (tabbed && fenceSelected) {
            _clickStableButton(QStringLiteral("planPanel_fenceInspector_back"));
            QVERIFY(!QTest::currentTestFailed());
        }
        QTRY_VERIFY_WITH_TIMEOUT(findVisibleItem(_rootItem, QStringLiteral("planPanel_fenceList"), 0),
                                 TestTimeout::shortMs());
        _clickStableButton(QStringLiteral("planPanel_fenceRow_") + key);
        QVERIFY(!QTest::currentTestFailed());
        fenceSelected = true;
    };
    selectFenceRow(QStringLiteral("polygon_0"));
    QVERIFY(!QTest::currentTestFailed());
    auto* polygon = fence->polygons()->value<QGCFencePolygon*>(0);
    QVERIFY(polygon && polygon->interactive());
    QPointer<QQuickItem> polygonFill;
    QPointer<QQuickItem> polygonOutline;
    for (auto* item : planStyleItems(map, QStringLiteral("planFenceFill"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon") &&
            item->property("path").toList() == polygon->path()) {
            polygonFill = item;
        }
    }
    for (auto* item : planStyleItems(map, QStringLiteral("planKeepInOutline"))) {
        if (item->property("boundaryKind").toString() == QStringLiteral("polygon")) {
            polygonOutline = item;
        }
    }
    QVERIFY(polygonFill && polygonOutline);
    const auto belongsToPolygon = [polygon](QQuickItem* item) {
        // Handles are parented to the map; their creation context identifies the owning polygon visuals.
        auto* context = qmlContext(item);
        auto* visuals = context ? qvariant_cast<QObject*>(context->contextProperty(QStringLiteral("_root"))) : nullptr;
        return visuals && qvariant_cast<QObject*>(visuals->property("mapPolygon")) == polygon;
    };
    const auto findVertexDrag = [&]() -> QQuickItem* {
        for (auto* item : map->childItems()) {
            if (item->property("polygonVertex").isValid() && item->property("polygonVertex").toInt() == 0 &&
                item->metaObject()->indexOfProperty("itemCoordinate") >= 0 && belongsToPolygon(item)) {
                return item;
            }
        }
        return nullptr;
    };
    const auto vertexHandleFor = [&]() -> QQuickItem* {
        auto* drag = findVertexDrag();
        return drag ? qvariant_cast<QQuickItem*>(drag->property("itemIndicator")) : nullptr;
    };
    preparePointer(vertexHandleFor);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(polygonFill && polygonOutline);
    const auto committed = polygonFill->property("path").toList();
    QSignalSpy committedChanges(polygon, &QGCMapPolygon::pathChanged);
    QVERIFY(committedChanges.isValid());
    QPoint start;
    {
        auto* vertexDrag = findVertexDrag();
        auto* vertexHandle = vertexHandleFor();
        QVERIFY(vertexDrag && vertexHandle);
        QVERIFY(vertexDrag->z() >= 51 && vertexDrag->z() <= 53);
        QVERIFY(vertexHandle->z() >= 51 && vertexHandle->z() <= 53);
        QVERIFY(polygonOutline->z() < vertexDrag->z());
        start = vertexHandle->mapToScene(vertexHandle->property("anchorPoint").toPointF()).toPoint();
    }
    const QPoint delta(-60, 45);
    QTest::mousePress(_window, Qt::LeftButton, Qt::NoModifier, start);
    for (int i = 1; i <= 10; ++i) {
        QTest::mouseMove(_window, start + delta * i / 10);
    }
    const bool live = waitForCondition(
        [&] {
            return planStyleRenderMap(map) && polygonOutline && polygonFill && polygon->vertexDrag() &&
                   polygonOutline->property("vertices").toList() != committed &&
                   polygonOutline->property("vertices").toList() == polygon->path();
        },
        TestTimeout::shortMs(), QStringLiteral("Plan vertex preview follows held pointer"));
    if (!live || !polygonOutline || !polygonFill) {
        QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, start + delta);
        QVERIFY(live);
        QVERIFY(polygonOutline && polygonFill);
    }
    const auto heldPath = polygonOutline->property("vertices").toList();
    const qreal heldBorder = QQmlProperty(polygonFill, QStringLiteral("border.width")).read().toReal();
    const int commitsWhileHeld = committedChanges.count();
    QTest::mouseRelease(_window, Qt::LeftButton, Qt::NoModifier, start + delta);
    QVERIFY(live);
    QVERIFY(heldPath != committed);
    QCOMPARE(heldBorder, 0.0);
    QCOMPARE(commitsWhileHeld, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!polygon->vertexDrag(), TestTimeout::shortMs());
    planStyleCheckBoundaries(map, master->planVehicleId(), color, 2, 2);
    QVERIFY(!QTest::currentTestFailed());

    // The same native editor also previews a whole-polygon centre drag before its committed-path notification.
    QVERIFY(polygonOutline && polygonFill);
    const auto beforeCenter = polygonOutline->property("vertices").toList();
    polygon->setCenterDrag(true);
    polygon->setVertexDrag(true);
    polygon->setCenter(polygon->center().atDistanceAndAzimuth(40, 180));
    QTRY_VERIFY_WITH_TIMEOUT(polygonOutline && polygonOutline->property("vertices").toList() != beforeCenter,
                             TestTimeout::shortMs());
    QCOMPARE(polygonOutline->property("vertices").toList(), polygon->path());
    QVERIFY(polygonFill);
    QCOMPARE(QQmlProperty(polygonFill, QStringLiteral("border.width")).read().toReal(), 0.0);
    polygon->setCenterDrag(false);
    polygon->setVertexDrag(false);

    const auto splitHandleFor = [&]() -> QQuickItem* {
        for (auto* item : map->childItems()) {
            if (item->property("vertexIndex").isValid() && item->property("vertexIndex").toInt() == 0 &&
                !item->property("distance").isValid() && planStyleSource(item) && belongsToPolygon(item)) {
                return item;
            }
        }
        return nullptr;
    };
    clickPoint(splitHandleFor);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), 4, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
    QTRY_VERIFY_WITH_TIMEOUT(findVertexDrag(), TestTimeout::shortMs());
    {
        auto* vertexDrag = findVertexDrag();
        QVERIFY(vertexDrag && vertexDrag->isVisible());
        QVERIFY(QMetaObject::invokeMethod(vertexDrag, "clicked"));
    }
    _clickStableItem(QStringLiteral("remove fence vertex"),
                     [&] { return planStyleTab(_rootItem, QStringLiteral("Remove vertex")); });
    QVERIFY(!QTest::currentTestFailed());
    QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), 3, TestTimeout::shortMs());
    QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());

    _clickStableItem(QStringLiteral("trace fence"), [&] { return planStyleTab(map, QStringLiteral("Trace")); });
    QVERIFY(!QTest::currentTestFailed());
    QTRY_VERIFY_WITH_TIMEOUT(polygon->traceMode(), TestTimeout::shortMs());
    QCOMPARE(polygon->count(), 0);
    const auto viewport = map->property("centerViewport").toRectF();
    int tracedVertices = 0;
    for (const QPointF& fraction : {QPointF(0.3, 0.4), QPointF(0.7, 0.45), QPointF(0.5, 0.7)}) {
        QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());
        const auto position =
            viewport.topLeft() + QPointF(viewport.width() * fraction.x(), viewport.height() * fraction.y());
        QTest::mouseClick(_window, Qt::LeftButton, Qt::NoModifier, map->mapToScene(position).toPoint());
        ++tracedVertices;
        QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), tracedVertices, TestTimeout::shortMs());
    }
    QTRY_COMPARE_WITH_TIMEOUT(polygon->count(), 3, TestTimeout::shortMs());
    _clickStableItem(QStringLiteral("finish tracing"),
                     [&] { return planStyleTab(map, QStringLiteral("Done Tracing")); });
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(!polygon->traceMode());
    QTRY_VERIFY_WITH_TIMEOUT(planStyleEditorProjected(map), TestTimeout::mediumMs());

    selectFenceRow(QStringLiteral("circle_0"));
    QVERIFY(!QTest::currentTestFailed());
    auto* circle = fence->circles()->value<QGCFenceCircle*>(0);
    QVERIFY(circle && circle->interactive());
    QPointer<QObject> oldHandles;
    {
        QQuickItem* circleVisual = nullptr;
        for (auto* item : planStyleItems(map, QString())) {
            if (qvariant_cast<QObject*>(item->property("mapCircle")) == circle &&
                item->property("_dragHandlesComponent").isValid()) {
                circleVisual = item;
                break;
            }
        }
        QVERIFY(circleVisual);
        auto* handles = qvariant_cast<QObject*>(circleVisual->property("_dragHandlesComponent"));
        QVERIFY(handles);
        auto* centerDrag = qvariant_cast<QQuickItem*>(handles->property("centerDragArea"));
        auto* radiusDrag = qvariant_cast<QQuickItem*>(handles->property("radiusDragArea"));
        QVERIFY(centerDrag && radiusDrag && centerDrag->isVisible() && radiusDrag->isVisible());
        const auto center = circle->center().atDistanceAndAzimuth(30, 45);
        QVERIFY(centerDrag->setProperty("itemCoordinate", QVariant::fromValue(center)));
        QCOMPARE(circle->center(), center);
        QVERIFY(radiusDrag->setProperty("itemCoordinate", QVariant::fromValue(center.atDistanceAndAzimuth(175, 90))));
        QVERIFY(qAbs(circle->radius()->rawValue().toDouble() - 175) < 0.01);
        for (const auto& property : {"centerDragHandle", "radiusDragHandle"}) {
            auto* handle = qvariant_cast<QQuickItem*>(handles->property(property));
            QVERIFY(handle && handle->z() == 52);
        }
        oldHandles = handles;
    }
    planStyleCheckBoundaries(map, master->planVehicleId(), color, 2, 2);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(oldHandles);
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_VERIFY_WITH_TIMEOUT(oldHandles.isNull(), TestTimeout::shortMs());
}

void PlanStyleUITest::_planEditor_data()
{
    QTest::addColumn<bool>("tabbed");
    QTest::addColumn<bool>("lateRegistry");
    QTest::newRow("tabbed") << true << false;
    QTest::newRow("tree") << false << false;
    QTest::newRow("late-registry") << true << true;
}

void PlanStyleUITest::_planEditor()
{
    QFETCH(bool, tabbed);
    QFETCH(bool, lateRegistry);
    setUseTabbedPlanPanel(tabbed);
    setUseGeoMapPlanEngine(false);
    if (lateRegistry) {
        // Delay registry initialization, while keeping the real MainWindow, Plan view and vehicle downloads.
        const auto replacement = _directory.filePath(QStringLiteral("DelayedNavigation.qml"));
        QFile file(replacement);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray qml = "import QtQuick\nItem { property var hostWindow }\n";
        QCOMPARE(file.write(qml), qml.size());
        file.close();
        overrideQmlFile(QUrl(QStringLiteral("qrc:/qml/Custom/COP/COPNavigation.qml")),
                        QUrl::fromLocalFile(replacement));
    }
    startUI();
    QVERIFY(!QTest::currentTestFailed());
    auto* registry = _controller();
    QVERIFY(registry);
    auto* policy = QGCCorePlugin::instance()->planStyle();
    QVERIFY(policy);
    LinkManager::instance()->setConnectionsAllowed();
    QPointer<MockLink> first = _start(MAV_TYPE_GROUND_ROVER);
    QPointer<MockLink> second = _start(MAV_TYPE_QUADROTOR);
    QVERIFY(first && second);
    auto* manager = MultiVehicleManager::instance();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 2, TestTimeout::longMs());
    const auto vehicleFor = [manager](quint32 id) -> Vehicle* {
        for (int i = 0; i < manager->vehicles()->count(); ++i) {
            auto* vehicle = manager->vehicles()->value<Vehicle*>(i);
            if (vehicle && vehicle->id() == id) {
                return vehicle;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(vehicleFor(first->vehicleId()) && vehicleFor(second->vehicleId()), TestTimeout::longMs());
    auto* a = vehicleFor(first->vehicleId());
    auto* b = vehicleFor(second->vehicleId());
    QVERIFY(a && b && a != b);
    const quint32 aId = a->id();
    const quint32 bId = b->id();
    _uploadPlan(a, 4);
    QVERIFY(!QTest::currentTestFailed());
    _uploadPlan(b, 6);
    QVERIFY(!QTest::currentTestFailed());
    for (auto* vehicle : {a, b}) {
        auto* parameters = vehicle->parameterManager();
        QVERIFY(parameters);
        if (parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE")) &&
            parameters->parameterExists(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))) {
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_RADIUS"))
                ->setRawValue(vehicle == a ? 650 : 750);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_TYPE"))
                ->setRawValue(2);
            parameters->getParameter(ParameterManager::defaultComponentId, QStringLiteral("FENCE_ENABLE"))
                ->setRawValue(1);
        }
    }
    manager->setActiveVehicle(a);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewPlan")));
    auto* view = findVisibleItem(_rootItem, QStringLiteral("mainView_plan"));
    auto* map = findVisibleItem(_rootItem, QStringLiteral("planView_map"));
    QVERIFY(view && map);
    QVERIFY(!map->property("camera").isValid());
    QCOMPARE(view->property("_useNewPlanPanel").toBool(), tabbed);
    QVERIFY(findVisibleItem(_rootItem, tabbed ? QStringLiteral("planPanel") : QStringLiteral("planView_planTree")));
    auto* master = qvariant_cast<PlanMasterController*>(view->property("_planMasterController"));
    QVERIFY(master);
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), a, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(!master->syncInProgress(), TestTimeout::longMs());
    master->removeAll();
    QCOMPARE(master->planVehicleId(), 0U);
    auto* source = findVisibleItem(_rootItem, QStringLiteral("planToolbarPlanSource"), TestTimeout::longMs());
    auto* target = findItem(_rootItem, QStringLiteral("planToolbarUploadTarget"));
    QVERIFY(source && target);
    QCOMPARE(source->property("text").toString(), QStringLiteral("Plan: not from a vehicle"));
    QVERIFY(target->property("visible").toBool());
    QCOMPARE(target->property("text").toString(),
             QStringLiteral("Upload to: %1")
                 .arg(_entryFor(aId) ? _entryFor(aId)->label() : QStringLiteral("Vehicle %1").arg(aId)));
    QSignalSpy loaded(master->rallyPointController(), &RallyPointController::loadComplete);
    QVERIFY(loaded.isValid());
    master->loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty() && !master->syncInProgress(), TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->planVehicleId(), aId, TestTimeout::longMs());
    auto* mission = master->missionController();
    auto* fence = master->geoFenceController();
    auto* rally = master->rallyPointController();
    QCOMPARE(mission->visualItems()->count(), 4);
    QCOMPARE(fence->polygons()->count(), 2);
    QCOMPARE(fence->circles()->count(), 2);
    QCOMPARE(rally->points()->count(), 3);
    auto* tracker = qvariant_cast<QObject*>(map->property("positionTracker"));
    QVERIFY(tracker);
    QVERIFY(tracker->setProperty("userInteracting", true));
    QTRY_VERIFY_WITH_TIMEOUT(!tracker->property("animating").toBool(), TestTimeout::mediumMs());
    QVERIFY(map->setProperty("zoomLevel", 16));
    QVERIFY(map->setProperty("center", QVariant::fromValue(mission->plannedHomePosition())));
    _insertPatterns(master);
    QVERIFY(!QTest::currentTestFailed());
    const auto contexts = map->findChildren<QObject*>(QStringLiteral("planViewPlanStyleContext"));
    QCOMPARE(contexts.size(), 1);
    QPointer<QObject> context = contexts.first();
    QColor aColor = policy->vehicleColor(aId);
    if (lateRegistry) {
        QVERIFY(!registry->colorForSysid(aId).isValid());
        QVERIFY(!registry->colorForSysid(bId).isValid());
        aColor = context->property("lineColor").value<QColor>();
        QCOMPARE(aColor, QColor(QStringLiteral("#ffa000")));
        QCOMPARE(source->property("text").toString(), QStringLiteral("Editing plan for: Vehicle %1").arg(aId));
        QCOMPARE(source->property("cueColor").value<QColor>(), aColor);
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
        auto* segments = mission->simpleFlightPathSegments();
        QVERIFY(segments);
        const auto liveRouteCount = [map] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return planStyleItems(map, QStringLiteral("planMissionRoute")).size();
        };
        QTRY_COMPARE_WITH_TIMEOUT(liveRouteCount(), segments->count(), TestTimeout::mediumMs());
        QSignalSpy segmentResets(segments, &QAbstractItemModel::modelReset);
        QSignalSpy segmentInsertions(segments, &QAbstractItemModel::rowsInserted);
        QSignalSpy segmentRemovals(segments, &QAbstractItemModel::rowsRemoved);
        QSignalSpy segmentMoves(segments, &QAbstractItemModel::rowsMoved);
        QSignalSpy segmentLayouts(segments, &QAbstractItemModel::layoutChanged);
        QVERIFY(segmentResets.isValid() && segmentInsertions.isValid() && segmentRemovals.isValid() &&
                segmentMoves.isValid() && segmentLayouts.isValid());
        const QPointer<QmlObjectListModel> retainedModel = mission->visualItems();
        QList<QPointer<VisualMissionItem>> retainedPatternModels;
        for (int i = 0; i < retainedModel->count(); ++i) {
            auto* patternModel = retainedModel->value<VisualMissionItem*>(i);
            if (qobject_cast<TransectStyleComplexItem*>(patternModel) ||
                qobject_cast<StructureScanComplexItem*>(patternModel) ||
                qobject_cast<LandingComplexItem*>(patternModel) ||
                qobject_cast<PerimeterScanComplexItem*>(patternModel)) {
                retainedPatternModels.append(patternModel);
            }
        }
        planStyleWaitForVisualBindings(map, mission->visualItems(), context);
        QVERIFY(!QTest::currentTestFailed());
        QList<QPair<QString, QPointer<QQuickItem>>> retained;
        for (const auto& name :
             {QStringLiteral("planMissionRoute"), QStringLiteral("planWaypoint"), QStringLiteral("planHomeAccent"),
              QStringLiteral("planFenceFill"), QStringLiteral("planKeepInOutline"),
              QStringLiteral("planKeepOutOutline"), QStringLiteral("planRallyPoint"),
              QStringLiteral("planPatternMarker"), QStringLiteral("planTransectFull"), QStringLiteral("planPolyline"),
              QStringLiteral("planPatternLine"), QStringLiteral("planLoiterRing"), QStringLiteral("planLandingArea"),
              QStringLiteral("planGlideSlope"), QStringLiteral("planPerimeterFill"),
              QStringLiteral("planPerimeterTraversal"), QStringLiteral("planPerimeterMarker"),
              QStringLiteral("planPerimeterEntry")}) {
            for (auto* item : planStyleItems(map, name)) {
                retained.append({name, item});
            }
        }
        auto* roles = _engine->singletonInstance<VehicleRoleController*>("QGC", "VehicleRoleController");
        QVERIFY(roles && !roles->availableRoles().isEmpty());
        roles->addEntry(bId, roles->availableRoles().first(), QStringLiteral("Late Plan target"), 0);
        QCOMPARE(roles->roleForSysid(bId), roles->availableRoles().first());
        registry->initialize(roles);
        QTRY_VERIFY2_WITH_TIMEOUT(registry->colorForSysid(aId).isValid() && registry->colorForSysid(bId).isValid(),
                                  "late registry: owner A and active B colours were not registered",
                                  TestTimeout::shortMs());
        aColor = registry->colorForSysid(aId);
        QVERIFY(aColor != QColor(QStringLiteral("#ffa000")));
        QVERIFY(_entryFor(aId));
        QCOMPARE(source->property("text").toString(),
                 QStringLiteral("Editing plan for: %1").arg(_entryFor(aId)->label()));
        QCOMPARE(source->property("cueColor").value<QColor>(), aColor);
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(mission->visualItems(), retainedModel.data());
        for (const auto& patternModel : retainedPatternModels) {
            QVERIFY2(patternModel && retainedModel->contains(patternModel),
                     "late registry: a retained pattern model was destroyed or replaced");
        }
        // Corridor boundary changes destroy/recreate outline Loaders. _checkPlan resolves every current model ring.
        for (const auto& retainedVisual : retained) {
            // Segment model changes legitimately rebuild MapItemView route delegates during registry attachment.
            if (retainedVisual.first == QStringLiteral("planMissionRoute") &&
                (!segmentResets.isEmpty() || !segmentInsertions.isEmpty() || !segmentRemovals.isEmpty() ||
                 !segmentMoves.isEmpty() || !segmentLayouts.isEmpty())) {
                continue;
            }
            QVERIFY2(retainedVisual.second,
                     qPrintable(QStringLiteral("late registry destroyed stable %1").arg(retainedVisual.first)));
            QVERIFY2(planStyleItems(map, retainedVisual.first).contains(retainedVisual.second.data()),
                     qPrintable(planStyleLookupContext(map, retainedVisual.first, 1)));
        }
    }
    QVERIFY(aColor.isValid());
    const QColor bColor = registry->colorForSysid(bId);
    QVERIFY(bColor.isValid() && aColor != bColor);
    QVERIFY(_entryFor(aId) && _entryFor(bId));
    mission->setCurrentPlanViewSeqNum(1, true);
    rally->setCurrentRallyPoint(rally->points()->get(0));
    for (int layer : {1, 2, 3, 1, 2, 3, 1}) {
        _setPlanLayer(view, layer);
        QVERIFY(!QTest::currentTestFailed());
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
    }
    if (tabbed) {
        _setPlanLayer(view, 0);
        QVERIFY(!QTest::currentTestFailed());
        _checkPlan(view, aId, aColor, 4, 3);
        QVERIFY(!QTest::currentTestFailed());
    }
    if (!lateRegistry) {
        _editPatterns(view);
        QVERIFY(!QTest::currentTestFailed());
    }
    if (tabbed && !lateRegistry) {
        _editPlan(view);
        QVERIFY(!QTest::currentTestFailed());
    }

    auto* waypoint = mission->visualItems()->value<VisualMissionItem*>(1);
    QVERIFY(waypoint);
    waypoint->setCoordinate(waypoint->coordinate().atDistanceAndAzimuth(30, 45));
    QVERIFY(master->dirtyForSave());
    QList<QGeoCoordinate> keptCoordinates;
    for (int i = 0; i < mission->visualItems()->count(); ++i) {
        keptCoordinates.append(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate());
    }
    const auto settledRouteCount = [map] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return planStyleItems(map, QStringLiteral("planMissionRoute")).size();
    };
    auto* keptSegments = mission->simpleFlightPathSegments();
    QTRY_COMPARE_WITH_TIMEOUT(settledRouteCount(), keptSegments->count(), TestTimeout::mediumMs());
    QSignalSpy keptResets(keptSegments, &QAbstractItemModel::modelReset);
    QSignalSpy keptInsertions(keptSegments, &QAbstractItemModel::rowsInserted);
    QSignalSpy keptRemovals(keptSegments, &QAbstractItemModel::rowsRemoved);
    QSignalSpy keptMoves(keptSegments, &QAbstractItemModel::rowsMoved);
    QSignalSpy keptLayouts(keptSegments, &QAbstractItemModel::layoutChanged);
    QVERIFY(keptResets.isValid() && keptInsertions.isValid() && keptRemovals.isValid() && keptMoves.isValid() &&
            keptLayouts.isValid());
    planStyleWaitForVisualBindings(map, mission->visualItems(), context);
    QVERIFY(!QTest::currentTestFailed());
    QList<QPair<QString, QPointer<QQuickItem>>> keptObjects;
    for (const auto& name :
         {QStringLiteral("planMissionRoute"), QStringLiteral("planWaypoint"), QStringLiteral("planFenceFill"),
          QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline"), QStringLiteral("planRallyPoint"),
          QStringLiteral("planPatternMarker"), QStringLiteral("planPatternOutline"), QStringLiteral("planTransectFull"),
          QStringLiteral("planPolyline"), QStringLiteral("planPatternLine"), QStringLiteral("planLoiterRing"),
          QStringLiteral("planLandingArea"), QStringLiteral("planGlideSlope"), QStringLiteral("planPerimeterFill"),
          QStringLiteral("planPerimeterTraversal"), QStringLiteral("planPerimeterMarker"),
          QStringLiteral("planPerimeterEntry")}) {
        for (auto* item : planStyleItems(map, name)) {
            keptObjects.append({name, item});
        }
    }
    QSignalSpy prompted(master, &PlanMasterController::promptForPlanUsageOnVehicleChange);
    QVERIFY(prompted.isValid());
    const auto choosePrompt = [&](bool keep) {
        QTRY_VERIFY_WITH_TIMEOUT(view->property("_promptForPlanUsageShowing").toBool(), TestTimeout::mediumMs());
        const QString text = keep ? (master->offline() ? QStringLiteral("Keep Current Plan")
                                                       : QStringLiteral("Keep Current Plan, Don't Update From Vehicle"))
                                  : QStringLiteral("Discard Unsaved Changes, Load New Plan From Vehicle");
        _clickStableItem(text, [&] { return planStyleTab(_rootItem, text); });
        QVERIFY(!QTest::currentTestFailed());
        QTRY_VERIFY_WITH_TIMEOUT(!view->property("_promptForPlanUsageShowing").toBool(), TestTimeout::shortMs());
    };
    manager->setActiveVehicle(b);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), b, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), b, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(prompted.count(), 1, TestTimeout::mediumMs());
    QCOMPARE(master->planVehicleId(), aId);
    choosePrompt(true);
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(source->property("text").toString(), QStringLiteral("Plan: %1").arg(_entryFor(aId)->label()));
    QCOMPARE(source->property("cueColor").value<QColor>(), aColor);
    QVERIFY(target->property("visible").toBool());
    QCOMPARE(target->property("text").toString(), QStringLiteral("Upload to: %1").arg(_entryFor(bId)->label()));
    QCOMPARE(target->property("cueColor").value<QColor>(), bColor);
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    _checkPlan(view, aId, aColor, 4, 3);
    QVERIFY(!QTest::currentTestFailed());
    for (int i = 0; i < keptCoordinates.size(); ++i) {
        QCOMPARE(mission->visualItems()->value<VisualMissionItem*>(i)->coordinate(), keptCoordinates[i]);
    }
    for (const auto& item : keptObjects) {
        // A changed segment model may replace route delegates; other primitives remain stable.
        if (item.first == QStringLiteral("planMissionRoute") &&
            (!keptResets.isEmpty() || !keptInsertions.isEmpty() || !keptRemovals.isEmpty() || !keptMoves.isEmpty() ||
             !keptLayouts.isEmpty())) {
            continue;
        }
        QVERIFY2(item.second, qPrintable(QStringLiteral("Keep replaced stable %1").arg(item.first)));
        QVERIFY(planStyleItems(map, item.first).contains(item.second.data()));
    }

    // Reopen the real decision by switching back and then to B; Keep itself deliberately performs no load.
    manager->setActiveVehicle(a);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), a, TestTimeout::longMs());
    choosePrompt(true);
    QVERIFY(!QTest::currentTestFailed());
    manager->setActiveVehicle(b);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), b, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->managerVehicle(), b, TestTimeout::longMs());
    choosePrompt(false);
    QVERIFY(!QTest::currentTestFailed());
    QTRY_COMPARE_WITH_TIMEOUT(master->planVehicleId(), bId, TestTimeout::longMs());
    QCOMPARE(source->property("text").toString(), QStringLiteral("Editing plan for: %1").arg(_entryFor(bId)->label()));
    QVERIFY(!target->property("visible").toBool());
    _checkPlan(view, bId, bColor, 6, 5);
    QVERIFY(!QTest::currentTestFailed());
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(fence->polygons()->value<QGCFencePolygon*>(i)->path(), b->geoFenceManager()->polygons()[i].path());
        QCOMPARE(fence->circles()->value<QGCFenceCircle*>(i)->center(), b->geoFenceManager()->circles()[i].center());
    }
    QCOMPARE(rally->points()->value<RallyPoint*>(0)->coordinate(), b->rallyPointManager()->points()[0].coordinate);
    QVERIFY(mission->visualItems()->value<VisualMissionItem*>(1)->coordinate().distanceTo(keptCoordinates[1]) > 10);

    manager->setActiveVehicle(a);
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(master->planVehicleId(), aId, TestTimeout::longMs());
    _checkPlan(view, aId, aColor, 4, 3);
    QVERIFY(!QTest::currentTestFailed());
    _insertPatterns(master);
    QVERIFY(!QTest::currentTestFailed());
    waypoint = mission->visualItems()->value<VisualMissionItem*>(1);
    waypoint->setCoordinate(waypoint->coordinate().atDistanceAndAzimuth(40, 45));
    const auto dirtyCoordinate = waypoint->coordinate();
    const auto offlineHome = mission->plannedHomePosition();
    QVERIFY(master->dirtyForSave());
    second->disconnect();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 1, TestTimeout::longMs());
    QTRY_COMPARE_WITH_TIMEOUT(manager->activeVehicle(), a, TestTimeout::longMs());
    first->disconnect();
    QTRY_COMPARE_WITH_TIMEOUT(manager->vehicles()->count(), 0, TestTimeout::longMs());
    QTRY_VERIFY_WITH_TIMEOUT(manager->activeVehicle() == nullptr && master->offline(), TestTimeout::longMs());
    choosePrompt(true);
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(master->planVehicleId(), aId);
    QCOMPARE(source->property("text").toString(), QStringLiteral("Plan: %1").arg(_entryFor(aId)->label()));
    QCOMPARE(source->property("cueColor").value<QColor>(), aColor);
    QVERIFY(!target->property("visible").toBool());
    QCOMPARE(waypoint->coordinate(), dirtyCoordinate);
    _checkPlan(view, aId, aColor, 4, 3);
    QVERIFY(!QTest::currentTestFailed());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QList<QPointer<QQuickItem>> removed;
    for (const auto& name : {QStringLiteral("planKeepInOutline"), QStringLiteral("planKeepOutOutline"),
                             QStringLiteral("planRallyPoint")}) {
        for (auto* item : planStyleItems(map, name)) {
            if (item->property("boundaryKind").toString() != QStringLiteral("parameterCircle")) {
                removed.append(item);
            }
        }
    }
    master->removeAll();
    QCOMPARE(master->planVehicleId(), 0U);
    QVERIFY(!source->property("visible").toBool());
    QVERIFY(!target->property("visible").toBool());
    for (const auto& item : removed) {
        QTRY_VERIFY_WITH_TIMEOUT(item.isNull(), TestTimeout::shortMs());
    }
    mission->setHomePosition(offlineHome);
    for (int i = 1; i <= 2; ++i) {
        QVERIFY(mission->insertSimpleMissionItem(offlineHome.atDistanceAndAzimuth(200 * i, 90), i, true));
    }
    const auto topLeft = offlineHome.atDistanceAndAzimuth(300, 315);
    const auto bottomRight = offlineHome.atDistanceAndAzimuth(300, 135);
    for (bool inclusion : {true, false}) {
        fence->addInclusionPolygon(topLeft, bottomRight);
        fence->polygons()->value<QGCFencePolygon*>(fence->polygons()->count() - 1)->setInclusion(inclusion);
        fence->addInclusionCircle(topLeft, bottomRight);
        auto* circle = fence->circles()->value<QGCFenceCircle*>(fence->circles()->count() - 1);
        circle->setInclusion(inclusion);
        circle->setCenter(offlineHome.atDistanceAndAzimuth(150, inclusion ? 180 : 270));
    }
    rally->addPoint(offlineHome.atDistanceAndAzimuth(200, 315));
    rally->addPoint(offlineHome.atDistanceAndAzimuth(300, 315));
    _setPlanLayer(view, 1);
    QVERIFY(!QTest::currentTestFailed());
    const QColor neutral(QStringLiteral("#ffa000"));
    _checkPlan(view, 0, neutral, 3, 2);
    QVERIFY(!QTest::currentTestFailed());
    QCOMPARE(context.data(), map->findChild<QObject*>(QStringLiteral("planViewPlanStyleContext")));
    QVERIFY(context->setProperty("supported", false));
    QTRY_VERIFY_WITH_TIMEOUT(planStyleItems(map, QStringLiteral("planKeepInOutline")).isEmpty() &&
                                 planStyleItems(map, QStringLiteral("planKeepOutOutline")).isEmpty(),
                             TestTimeout::shortMs());
    for (auto* route : planStyleItems(map, QStringLiteral("planMissionRoute"))) {
        QCOMPARE(planStyleColor(route, QStringLiteral("line.color")), neutral);
        QCOMPARE(QQmlProperty(route, QStringLiteral("line.width")).read().toReal(), 3.0);
    }
    for (auto* fill : planStyleItems(map, QStringLiteral("planFenceFill"))) {
        const bool inclusion = fill->property("inclusion").toBool();
        QCOMPARE(QQmlProperty(fill, QStringLiteral("border.width")).read().toReal(), inclusion ? 2.0 : 0.0);
        QCOMPARE(fill->opacity(), inclusion ? 0.5 : 0.1);
    }
    for (auto* marker : planStyleItems(map, QStringLiteral("planWaypoint"))) {
        QCOMPARE(marker->property("renderedBorderWidth").toReal(), 0.0);
        QCOMPARE(marker->property("renderedTextColor").value<QColor>(), QColor(QStringLiteral("white")));
        if (marker->property("checked").toBool()) {
            QCOMPARE(marker->property("renderedBodyColor").value<QColor>(), QColor(QStringLiteral("green")));
        }
    }
    for (auto* marker : planStyleItems(map, QStringLiteral("planRallyPoint"))) {
        QTRY_COMPARE_WITH_TIMEOUT(marker->property("markerShape").toString(), QStringLiteral("circle"),
                                  TestTimeout::shortMs());
        QCOMPARE(marker->property("markerBorderWidth").toReal(), 0.0);
    }
    for (auto* accent : planStyleItems(map, QStringLiteral("planHomeAccent"))) {
        QVERIFY(!accent->isVisible());
    }
    QVERIFY(context->setProperty("supported", true));
    _checkPlan(view, 0, neutral, 3, 2);
    QVERIFY(!QTest::currentTestFailed());
}

UT_REGISTER_TEST(PlanStyleUITest, TestLabel::Integration, TestLabel::Vehicle)
