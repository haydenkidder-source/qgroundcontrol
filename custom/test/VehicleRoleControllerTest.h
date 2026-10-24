#pragma once

#include <QtCore/QTemporaryDir>

#include "UnitTest.h"

class VehicleRoleControllerTest : public UnitTest
{
    Q_OBJECT
private slots:
    void init() override;
    void cleanup() override;
    void _availableRolesAreArduPilotVehicleTypes();
    void _suggestedRoleFollowsVehicleType_data();
    void _suggestedRoleFollowsVehicleType();
    void _addEntryRejectsUnrecognizedRole();
    void _loadMigratesUnrecognizedSavedRoleToName();
    void _nicknamesMustBeDistinct();
    void _savedDuplicateNicknamesStillLoad();

private:
    QTemporaryDir _directory;
    QVariant _savePath;
};
