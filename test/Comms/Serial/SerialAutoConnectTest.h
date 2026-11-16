#pragma once

#include "UnitTest.h"

class SerialAutoConnectTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _classification_data();
    void _classification();
    void _skipTransitions();
    void _retryDoesNotClearSkip();
    void _verifiedConnection_data();
    void _verifiedConnection();
    void _gatePrecedence_data();
    void _gatePrecedence();
    void _compositeSelection_data();
    void _compositeSelection();
    void _discoveryDeadlineAndRetryState();
    void _replacementDeviceResetsConfiguration();
};
