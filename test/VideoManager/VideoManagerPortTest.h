#pragma once

#include "UnitTest.h"

class VideoManagerPortTest : public UnitTest
{
    Q_OBJECT

private slots:
    void init() override;
    void _gateAtInitAndActivation();
    void _whenReadyDeliversQueuedOnceOnTheGuiThread();
    void _whenReadyReportsFailureAndHonoursItsContext();
};
