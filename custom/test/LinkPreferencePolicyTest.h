#pragma once

#include "UnitTest.h"

class LinkPreferencePolicyTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _selectionGuards_data();
    void _selectionGuards();
    void _continuousHealthAndPendingSwitch();
    void _secondaryLossBeforeFirstSelection();
    void _flapWindow_data();
    void _flapWindow();
    void _manualChoiceAndStockFailover_data();
    void _manualChoiceAndStockFailover();
    void _deadlineGuards_data();
    void _deadlineGuards();
    void _flappingBackoffAndStableReset();
    void _preferenceChangeResetsHistory();
    void _physicalDisconnectRetainsBackoff();
    void _emptyPrimarySignalDoesNotOverride();
};
