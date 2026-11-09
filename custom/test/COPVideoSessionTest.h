#pragma once

#include "UnitTest.h"

class COPVideoSessionTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _staleReadiness_data();
    void _staleReadiness();
    void _receiverOwnsReconnectAndConfiguration();
    void _endpointOwnershipAndShutdown();
};
