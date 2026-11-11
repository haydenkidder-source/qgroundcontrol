#pragma once

#include "CommsTest.h"
#include "LinkConfiguration.h"

class UDPLinkReceiveTest : public CommsTest
{
    Q_OBJECT

private slots:
    void _testEmptyDatagramDoesNotBlockLaterDatagrams();
    void _testEmptyDatagramBetweenFrames();
    void _testEmptyDatagramDoesNotLearnTarget();

private:
    static quint16 _unusedUdpPort();
    SharedLinkConfigurationPtr _addUdpConfig(quint16 port);
    void _cleanupUdpConfig(const SharedLinkConfigurationPtr& config, quint16 port);
    void _testReceiveFrames(int framesBeforeEmpty, int framesAfterEmpty);
};
