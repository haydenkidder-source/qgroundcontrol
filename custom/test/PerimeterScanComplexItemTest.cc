#include "PerimeterScanComplexItemTest.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include "MissionItem.h"
#include "PerimeterScanComplexItem.h"
#include "PlanMasterController.h"

void PerimeterScanComplexItemTest::_vertexOrder()
{
    const QList<QGeoCoordinate> vertices = {QGeoCoordinate(0, 0), QGeoCoordinate(0, 2), QGeoCoordinate(1, 1),
                                            QGeoCoordinate(1, 0)};
    PerimeterScanComplexItem item(planController(), false);
    item.perimeterPolygon()->setPath(
        QList<QGeoCoordinate>{vertices[0], QGeoCoordinate(0, 1), vertices[2], vertices[3]});
    item.perimeterPolygon()->adjustVertex(1, vertices[1]);
    item.perimeterPolygon()->verifyClockwiseWinding();
    QCOMPARE(item.coordinate(), vertices[0]);
    QCOMPARE(item.perimeterPolygon()->coordinateList(), vertices);

    QJsonArray saved;
    item.save(saved);
    PerimeterScanComplexItem loaded(planController(), false);
    QString error;
    QVERIFY2(loaded.load(saved[0].toObject(), 0, error), qPrintable(error));
    QCOMPARE(loaded.coordinate(), vertices[0]);
    QCOMPARE(loaded.perimeterPolygon()->coordinateList(), vertices);

    for (PerimeterScanComplexItem* scan : {&item, &loaded}) {
        QObject parent;
        QList<MissionItem*> missionItems;
        scan->appendMissionItems(missionItems, &parent);
        QCOMPARE(missionItems.size(), vertices.size() + 1);
        for (int i = 0; i < missionItems.size(); ++i) {
            QCOMPARE(missionItems[i]->param5(), vertices[i % vertices.size()].latitude());
            QCOMPARE(missionItems[i]->param6(), vertices[i % vertices.size()].longitude());
        }
    }
}

UT_REGISTER_TEST(PerimeterScanComplexItemTest, TestLabel::Unit, TestLabel::MissionManager)
