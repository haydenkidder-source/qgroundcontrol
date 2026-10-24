#pragma once

#include <QtCore/QList>
#include <QtCore/QPointer>

#include "LinkConfiguration.h"
#include "MockLink.h"
#include "QmlUITestBase.h"

/// A confirmation belongs to the vehicle that was active when it opened. With several vehicles connected, a confirmed
/// Emergency Stop, plan removal or rover mode switch must never land on whichever vehicle became active while the
/// operator was confirming.
class GuidedActionTargetUITest : public QmlUITestBase
{
    Q_OBJECT

private slots:
    void cleanup() override;
    void _emergencyStopConfirmationStaysWithItsVehicle();
    void _planRemovalConfirmationStaysWithItsVehicle();
    void _roverTakeoverAndReturnToAuto();
    void _roverTakeoverConfirmationStaysWithRover();

private:
    MockLink* _startVehicle(int index, MAV_AUTOPILOT firmware, MAV_TYPE type, bool takeoff);
    bool _openEmergencyStopConfirmation();
    QQuickItem* _openToolStripConfirmation(const QString& toolStripButton, const QString& title);
    bool _confirmationHidden();

    QList<SharedLinkConfigurationPtr> _configs;
    QList<QPointer<MockLink>> _links;
};
