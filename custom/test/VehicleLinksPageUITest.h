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
    void _settingsPlacementAndAnalyzeRemoval();
    void _emptyPageSaysWhatToDo();
    void _vehicleAddedByHandGetsACard_data();
    void _vehicleAddedByHandGetsACard();
    void _invalidSysidField_data();
    void _invalidSysidField();
    void _connectedVehicleIsOfferedForOneClickSetup_data();
    void _connectedVehicleIsOfferedForOneClickSetup();
    void _nicknameAlreadyInUseBlocksAdd();
    void _existingSystemIdBlocksAdd();
    void _failedAddKeepsFields();
    void _duplicateConfigurationDisablesLinkAction();
    void _stockLinkEditor();
    void _customLinkEditor_data();
    void _customLinkEditor();
    void _connectedTransportStatus();
    void _connectingTransportStatus();
    void _retryingTransportStatus();
    void _transportStatusChangesLive();
    void _vehicleCommunicationLossChangesLive();
    void _absentVehicleStatus();
    void _singleVehicleDisconnectsImmediately();
    void _sharedLinkDisconnectAsksFirst();
    void _linksBelongToTheirVehicle();
    void _preferredAndStatusTagsFollowVehicle_data();
    void _preferredAndStatusTagsFollowVehicle();

private:
    bool _openPage();
    VehicleRoleController* _roles() const;
    QQuickItem* _requireItem(const QString& objectName) const;

    QTemporaryDir _directory;
    QVariant _savePath;
    QVariant _failbackHold;
};
