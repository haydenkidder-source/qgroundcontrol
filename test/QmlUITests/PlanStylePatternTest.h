#pragma once

#include <QtCore/QScopeGuard>

#include "AppSettings.h"
#include "CameraCalc.h"
#include "CorridorScanComplexItem.h"
#include "QGCMAVLink.h"
#include "SettingsManager.h"
#include "StructureScanComplexItem.h"
#include "SurveyComplexItem.h"

namespace PlanStylePatternTest {
inline auto planStyleOfflineVehicleSettings(MAV_AUTOPILOT firmwareType, MAV_TYPE vehicleType)
{
    auto* settings = SettingsManager::instance()->appSettings();
    auto* firmware = settings->offlineEditingFirmwareClass();
    auto* vehicleClass = settings->offlineEditingVehicleClass();
    const auto savedFirmware = firmware->rawValue();
    const auto savedVehicleClass = vehicleClass->rawValue();
    // Starting an empty mission makes even an explicitly typed offline vehicle follow these settings.
    firmware->setRawValue(static_cast<int>(QGCMAVLink::firmwareClass(firmwareType)));
    vehicleClass->setRawValue(static_cast<int>(QGCMAVLink::vehicleClass(vehicleType)));
    return qScopeGuard([firmware, vehicleClass, savedFirmware, savedVehicleClass] {
        firmware->setRawValue(savedFirmware);
        vehicleClass->setRawValue(savedVehicleClass);
    });
}

inline QList<QGeoCoordinate> square(const QGeoCoordinate& base)
{
    const auto north = base.atDistanceAndAzimuth(200, 0);
    return {base, north, north.atDistanceAndAzimuth(200, 90), base.atDistanceAndAzimuth(200, 90)};
}

inline void seed(VisualMissionItem* item, const QGeoCoordinate& base)
{
    if (auto* transect = qobject_cast<TransectStyleComplexItem*>(item)) {
        transect->cameraCalc()->setCameraBrand(CameraCalc::canonicalManualCameraName());
        transect->cameraCalc()->adjustedFootprintSide()->setRawValue(40);
        transect->cameraCalc()->adjustedFootprintFrontal()->setRawValue(25);
        transect->turnAroundDistance()->setRawValue(30);
        if (auto* survey = qobject_cast<SurveyComplexItem*>(item)) {
            survey->surveyAreaPolygon()->appendVertices(square(base));
            survey->gridAngle()->setRawValue(0);
        } else if (auto* corridor = qobject_cast<CorridorScanComplexItem*>(item)) {
            corridor->corridorWidth()->setRawValue(80);
            const auto north = base.atDistanceAndAzimuth(200, 0);
            corridor->corridorPolyline()->appendVertices({base, north, north.atDistanceAndAzimuth(200, 90)});
        }
    } else if (auto* structure = qobject_cast<StructureScanComplexItem*>(item)) {
        structure->cameraCalc()->setCameraBrand(CameraCalc::canonicalManualCameraName());
        structure->cameraCalc()->distanceToSurface()->setRawValue(30);
        structure->structurePolygon()->appendVertices(square(base));
        structure->layers()->setRawValue(2);
    }
}
}  // namespace PlanStylePatternTest
