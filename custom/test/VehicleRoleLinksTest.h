#pragma once

#include <QtCore/QTemporaryDir>
#include <QtCore/QVariant>

#include "CommsTest.h"

/// Tests for VehicleRoleController's multi-link association model (VehicleRoleLinkEntry) - see
/// custom/src/VehicleRoles/VehicleRoleController.h. Extends CommsTest (rather than plain UnitTest,
/// as COPControllerTest does) because _isLinkConfigConnectedTracksLiveLink() needs a real
/// LinkManager-connected MockLink, and CommsTest guarantees that link is torn down in cleanup()
/// regardless of how the test finishes.
class VehicleRoleLinksTest : public CommsTest
{
    Q_OBJECT
private slots:
    void init() override;
    void cleanup() override;

    void _addRemoveAndPersistLinks();
    void _oldFormatFileLoadsWithoutLinks();
    void _isLinkConfigConnectedTracksLiveLink();

private:
    QTemporaryDir _directory;
    QVariant _savePath;
};
