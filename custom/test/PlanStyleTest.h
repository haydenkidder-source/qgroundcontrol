#pragma once

#include <QtCore/QTemporaryDir>

#include "VehicleTestManualConnect.h"

class PlanStyleTest : public VehicleTestManualConnect
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _constants();
    void _colorsAndRestoredOrder();
    void _revisionsAndDetach();
    void _connectedWithoutRole();
    void _engineLifecycle();
    void _qmlContextTracksRegistry();

private:
    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _audioMuted;
};
