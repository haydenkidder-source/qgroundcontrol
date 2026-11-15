#pragma once

#include "UnitTest.h"

class PlanStyleComponentTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _landingVisuals_data();
    void _landingVisuals();
    void _patternVisuals_data();
    void _patternVisuals();
    void _sharedComponents();
    void _stockMissionVisuals_data();
    void _stockMissionVisuals();
    void _optionalVisualContract();
    void _stockBoundaryVisuals_data();
    void _stockBoundaryVisuals();
    void _stockEditorVisuals_data();
    void _stockEditorVisuals();
};
