#include "AppLogViewerUITest.h"

#include <QtQuick/QQuickItem>

UT_REGISTER_TEST(AppLogViewerUITest, TestLabel::Integration)

void AppLogViewerUITest::_openFromAnalyzeAndSettings()
{
    startUI();
    if (QTest::currentTestFailed()) {
        return;
    }

    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewAnalyze")));
    QVERIFY(clickButton(QStringLiteral("analyzeButton_App Log Viewer")));
    auto* viewer = findVisibleItem(_rootItem, QStringLiteral("settingsPage_AppLogViewer"));
    QVERIFY(viewer);
    QTRY_VERIFY_WITH_TIMEOUT(viewer->width() > 0 && viewer->height() > 0, TestTimeout::shortMs());

    QVERIFY(clickToolSelectDropdownButton(QStringLiteral("toolbar_viewSettings")));
    auto* button = findVisibleItem(_rootItem, QStringLiteral("settingsButton_App Log Viewer"));
    QVERIFY(button);
    QVERIFY(scrollIntoView(button, QStringLiteral("settings_buttonList")));
    QVERIFY(clickButton(QStringLiteral("settingsButton_App Log Viewer")));
    QVERIFY(findVisibleItem(_rootItem, QStringLiteral("settingsPage_AppLogViewer")));
}
