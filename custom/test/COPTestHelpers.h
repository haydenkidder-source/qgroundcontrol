#pragma once

#include "LinkManager.h"
#include "MockLink.h"

namespace COPTestHelpers {

inline MockLink* connectMockLink(quint32 systemId)
{
    auto* mockConfig = new MockConfiguration(QStringLiteral("COP reconnect"));
    mockConfig->setFirmwareType(MAV_AUTOPILOT_PX4);
    mockConfig->setVehicleType(MAV_TYPE_QUADROTOR);
    mockConfig->setIncrementVehicleId(false);
    mockConfig->setSystemId(systemId);
    mockConfig->setDynamic(true);
    auto config = LinkManager::instance()->addConfiguration(mockConfig);
    if (!LinkManager::instance()->createConnectedLink(config)) {
        return nullptr;
    }
    return qobject_cast<MockLink*>(config->link());
}

}  // namespace COPTestHelpers
