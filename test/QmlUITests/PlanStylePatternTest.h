#pragma once

#include "CameraCalc.h"
#include "CorridorScanComplexItem.h"
#include "StructureScanComplexItem.h"
#include "SurveyComplexItem.h"

namespace PlanStylePatternTest {
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
