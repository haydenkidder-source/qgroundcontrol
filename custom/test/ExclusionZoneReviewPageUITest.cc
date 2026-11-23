#include "ExclusionZoneReviewPageUITest.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QTextOption>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickItem>

#include "ExclusionZoneController.h"

UT_REGISTER_TEST(ExclusionZoneReviewPageUITest, TestLabel::Integration)

void ExclusionZoneReviewPageUITest::_importMessages()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString valid = directory.filePath(QStringLiteral("valid.kml"));
    QFile file(valid);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray content =
        "<kml><Document><Placemark><Polygon><outerBoundaryIs><LinearRing>"
        "<coordinates>0,0 1,0 1,1 0,0</coordinates>"
        "</LinearRing></outerBoundaryIs></Polygon></Placemark></Document></kml>";
    QCOMPARE(file.write(content), content.size());
    file.close();

    startUI();
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewAnalyze")));
    QVERIFY(clickButton(QStringLiteral("analyzeButton_Exclusion Zones")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("importZonesButton"), TestTimeout::mediumMs()));
    auto* controller = _engine->singletonInstance<ExclusionZoneController*>("QGC", "ExclusionZoneController");
    QVERIFY(controller);
    QVERIFY(!findVisibleItem(_rootItem, QStringLiteral("importErrorLabel"), 0));
    QVERIFY(!findVisibleItem(_rootItem, QStringLiteral("importSummaryLabel"), 0));

    QVERIFY(!controller->importFromFile(directory.filePath(QStringLiteral("missing.kml"))));
    auto* error = findVisibleItem(_rootItem, QStringLiteral("importErrorLabel"), TestTimeout::mediumMs());
    QVERIFY(error);
    QCOMPARE(error->property("text").toString(), controller->importError());
    QVERIFY(error->property("readOnly").toBool());
    QVERIFY(error->property("selectByMouse").toBool());
    QCOMPARE(error->property("wrapMode").toInt(), static_cast<int>(QTextOption::WrapAtWordBoundaryOrAnywhere));
    QVERIFY(!controller->importError().isEmpty());
    QVERIFY(!findVisibleItem(_rootItem, QStringLiteral("importSummaryLabel"), 0));

    QVERIFY(controller->importFromFile(valid));
    auto* summary = findVisibleItem(_rootItem, QStringLiteral("importSummaryLabel"), TestTimeout::mediumMs());
    QVERIFY(summary);
    QCOMPARE(summary->property("text").toString(), QStringLiteral("Imported 1 zone(s) from valid.kml"));
    QCOMPARE(summary->property("wrapMode").toInt(), static_cast<int>(QTextOption::WordWrap));
    QVERIFY(!findVisibleItem(_rootItem, QStringLiteral("importErrorLabel"), 0));
}
