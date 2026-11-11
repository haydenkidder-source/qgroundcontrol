#pragma once

#include "UnitTest.h"

class LinkPreferencePolicyTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _initialSettleOutlastsLossDetection();
    void _transferActive_data();
    void _transferActive();
    void _debugState();
    void _statusAndDecision_data();
    void _statusAndDecision();
    void _healthyPreferredDeparture_data();
    void _healthyPreferredDeparture();
    void _secondaryChoicesKeepRecoveryDeadline_data();
    void _secondaryChoicesKeepRecoveryDeadline();
    void _selectionGuards_data();
    void _selectionGuards();
    void _continuousHealthAndPendingSwitch();
    void _secondaryLossBeforeFirstSelection();
    void _flapWindow_data();
    void _flapWindow();
    void _secondaryChoicesKeepWait_data();
    void _secondaryChoicesKeepWait();
    void _deadlineGuards_data();
    void _deadlineGuards();
    void _flappingBackoffAndStableReset();
    void _preferenceChangeResetsHistory();
    void _physicalDisconnectRetainsBackoff();
    void _emptyPrimaryKeepsSteadyClock();
};
