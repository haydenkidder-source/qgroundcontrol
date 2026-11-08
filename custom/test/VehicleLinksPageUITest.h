#pragma once

#include <QtCore/QTemporaryDir>

#include "QmlUITestBase.h"

class VehicleRoleController;

/// The Vehicle Links page is the one place to set up a vehicle (sysid, type, nickname) and give it links.
class VehicleLinksPageUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void init() override;
    void cleanup() override;
    void _emptyPageSaysWhatToDo();
    void _vehicleAddedByHandGetsACard_data();
    void _vehicleAddedByHandGetsACard();
    void _invalidSysidField_data();
    void _invalidSysidField();
    void _connectedVehicleIsOfferedForOneClickSetup_data();
    void _connectedVehicleIsOfferedForOneClickSetup();
    void _nicknameAlreadyInUseBlocksAdd();
    void _linksBelongToTheirVehicle();

private:
    bool _openPage();
    VehicleRoleController* _roles() const;
    QQuickItem* _requireItem(const QString& objectName) const;

    QTemporaryDir _directory;
    QVariant _savePath;
};
