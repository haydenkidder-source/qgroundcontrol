#pragma once

#include "BaseClasses/VehicleTest.h"

class APMStreamRateTest : public VehicleTestAPM
{
    Q_OBJECT

private slots:
    void _testBackoffAndRecovery_data();
    void _testBackoffAndRecovery();
};
