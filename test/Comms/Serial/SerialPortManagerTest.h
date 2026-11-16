#pragma once

#include "UnitTest.h"

class SerialPortManagerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _ownerMetadata_data();
    void _ownerMetadata();
    void _routingExclusionsDoNotOccupyPorts();
    void _exclusiveReservations();
    void _displayMetadataUsesCachedInventory();
    void _singlePortInventory();
    void _inventoryNotificationsAndBaudRates();
};
