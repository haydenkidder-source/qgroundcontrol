#pragma once

#include "UnitTest.h"

class PlanStyleComponentTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _sharedComponents();
    void _stockMissionVisuals_data();
    void _stockMissionVisuals();
    void _optionalVisualContract();
};
