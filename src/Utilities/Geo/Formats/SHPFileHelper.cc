#include "SHPFileHelper.h"

#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QTextStream>

#include "QGCGeo.h"
#include "QGCLoggingCategory.h"
#include "StrictPolygon.h"
#include "shapefil.h"

QGC_LOGGING_CATEGORY(SHPFileHelperLog, "Utilities.SHPFileHelper")

namespace {
    constexpr const char *_errorPrefix = QT_TRANSLATE_NOOP("SHPFileHelper", "SHP file load failed. %1");

    // QFile-based hooks for shapelib to support Qt Resource System (qrc:/) paths.
    // This allows loading shapefiles embedded in the application binary.

    SAFile qfileOpen(const char *filename, const char *access, void *pvUserData)
    {
        Q_UNUSED(pvUserData);

        if (!filename || !access) {
            qCWarning(SHPFileHelperLog) << "QFile open called with null filename or access mode";
            return nullptr;
        }

        // Only support read mode - shapefiles are read-only in QGC
        if (access[0] != 'r') {
            qCWarning(SHPFileHelperLog) << "QFile hooks only support read mode, requested:" << access;
            return nullptr;
        }

        auto *file = new QFile(QString::fromUtf8(filename));
        if (!file->open(QIODevice::ReadOnly)) {
            qCWarning(SHPFileHelperLog) << "Failed to open file:" << filename << file->errorString();
            delete file;
            return nullptr;
        }

        return reinterpret_cast<SAFile>(file);
    }

    SAOffset qfileRead(void *p, SAOffset size, SAOffset nmemb, SAFile file)
    {
        if (!file || !p || size == 0) {
            return 0;
        }
        auto *qfile = reinterpret_cast<QFile *>(file);
        const qint64 bytesRequested = static_cast<qint64>(size) * static_cast<qint64>(nmemb);
        const qint64 bytesRead = qfile->read(static_cast<char *>(p), bytesRequested);
        if (bytesRead < 0) {
            return 0;
        }
        return static_cast<SAOffset>(bytesRead / static_cast<qint64>(size));
    }

    SAOffset qfileWrite(const void *p, SAOffset size, SAOffset nmemb, SAFile file)
    {
        Q_UNUSED(p);
        Q_UNUSED(size);
        Q_UNUSED(nmemb);
        Q_UNUSED(file);
        qCWarning(SHPFileHelperLog) << "QFile write not supported - shapefiles are read-only in QGC";
        return 0;
    }

    SAOffset qfileSeek(SAFile file, SAOffset offset, int whence)
    {
        if (!file) {
            return -1;
        }
        auto *qfile = reinterpret_cast<QFile *>(file);
        qint64 newPos = 0;

        switch (whence) {
        case SEEK_SET:
            newPos = static_cast<qint64>(offset);
            break;
        case SEEK_CUR:
            newPos = qfile->pos() + static_cast<qint64>(offset);
            break;
        case SEEK_END:
            newPos = qfile->size() + static_cast<qint64>(offset);
            break;
        default:
            return -1;
        }

        if (newPos < 0) {
            return -1;
        }

        return qfile->seek(newPos) ? 0 : -1;
    }

    SAOffset qfileTell(SAFile file)
    {
        if (!file) {
            return 0;
        }
        auto *qfile = reinterpret_cast<QFile *>(file);
        return static_cast<SAOffset>(qfile->pos());
    }

    int qfileFlush(SAFile file)
    {
        Q_UNUSED(file);
        // No-op for read-only files
        return 0;
    }

    int qfileClose(SAFile file)
    {
        if (!file) {
            return 0;
        }
        auto *qfile = reinterpret_cast<QFile *>(file);
        qfile->close();
        delete qfile;
        return 0;
    }

    void setupQFileHooks(SAHooks *hooks)
    {
        SASetupDefaultHooks(hooks);
        hooks->FOpen = qfileOpen;
        hooks->FRead = qfileRead;
        hooks->FWrite = qfileWrite;
        hooks->FSeek = qfileSeek;
        hooks->FTell = qfileTell;
        hooks->FFlush = qfileFlush;
        hooks->FClose = qfileClose;
        hooks->Error = [](const char *message) {
            qCWarning(SHPFileHelperLog) << "SHP Error:" << message;
        };
    }
}

namespace SHPFileHelper
{
    /// Validates the specified SHP file is truly a SHP file and is in the format we understand.
    ///     @param utmZone[out] Zone for UTM shape, 0 for lat/lon shape
    ///     @param utmSouthernHemisphere[out] true/false for UTM hemisphere
    /// @return true: Valid supported SHP file found, false: Invalid or unsupported file found
    bool _validateSHPFiles(const QString &shpFile, int *utmZone, bool *utmSouthernHemisphere, QString &errorString);

    /// @param utmZone[out] Zone for UTM shape, 0 for lat/lon shape
    /// @param utmSouthernHemisphere[out] true/false for UTM hemisphere
    SHPHandle _loadShape(const QString &shpFile, int *utmZone, bool *utmSouthernHemisphere, QString &errorString);
}

bool SHPFileHelper::_validateSHPFiles(const QString &shpFile, int *utmZone, bool *utmSouthernHemisphere, QString &errorString)
{
    *utmZone = 0;
    errorString.clear();

    if (!shpFile.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive)) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "File is not a .shp file: %1").arg(shpFile));
        return false;
    }

    const QString prjFilename = shpFile.left(shpFile.length() - 4) + QStringLiteral(".prj");
    QFile prjFile(prjFilename);
    if (!prjFile.exists()) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "File not found: %1").arg(prjFilename));
        return false;
    }

    if (!prjFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHPFileHelper", "PRJ file open failed: %1").arg(prjFile.errorString()));
        return false;
    }

    QTextStream strm(&prjFile);
    const QString line = strm.readLine();
    if (line.startsWith(QStringLiteral("GEOGCS[\"GCS_WGS_1984\","))) {
        *utmZone = 0;
        *utmSouthernHemisphere = false;
    } else if (line.startsWith(QStringLiteral("PROJCS[\"WGS_1984_UTM_Zone_"))) {
        static const QRegularExpression regEx(QStringLiteral("^PROJCS\\[\"WGS_1984_UTM_Zone_(\\d{1,2})([NS])"));
        const QRegularExpressionMatch regExMatch = regEx.match(line);
        const QStringList rgCapture = regExMatch.capturedTexts();
        if (rgCapture.count() == 3) {
            const int zone = rgCapture[1].toInt();
            if ((zone >= 1) && (zone <= 60)) {
                *utmZone = zone;
                *utmSouthernHemisphere = (rgCapture[2] == QStringLiteral("S"));
            }
        }

        if (*utmZone == 0) {
            errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "UTM projection is not in supported format. Must be PROJCS[\"WGS_1984_UTM_Zone_##N/S"));
        }
    } else {
        // Extract projection name from WKT for error reporting
        // Format is either GEOGCS["name",... or PROJCS["name",...
        QString projectionName;
        static const QRegularExpression nameRegEx(QStringLiteral("^(?:GEOGCS|PROJCS)\\[\"([^\"]+)\""));
        const QRegularExpressionMatch nameMatch = nameRegEx.match(line);
        if (nameMatch.hasMatch()) {
            projectionName = nameMatch.captured(1);
        }

        if (!projectionName.isEmpty()) {
            errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(
                QCoreApplication::translate("SHP", "Unsupported projection: %1. Supported projections are: WGS84 (GEOGCS[\"GCS_WGS_1984\"]) and UTM (PROJCS[\"WGS_1984_UTM_Zone_##N/S\"]). Convert your shapefile to WGS84 using QGIS or ogr2ogr.")
                .arg(projectionName));
        } else {
            errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(
                QCoreApplication::translate("SHP", "Unable to parse projection from PRJ file. Supported projections are: WGS84 (GEOGCS[\"GCS_WGS_1984\"]) and UTM (PROJCS[\"WGS_1984_UTM_Zone_##N/S\"])."));
        }
    }

    return errorString.isEmpty();
}

SHPHandle SHPFileHelper::_loadShape(const QString &shpFile, int *utmZone, bool *utmSouthernHemisphere, QString &errorString)
{
    errorString.clear();

    if (!_validateSHPFiles(shpFile, utmZone, utmSouthernHemisphere, errorString)) {
        return nullptr;
    }

    // Use QFile-based hooks for Qt Resource System compatibility (qrc:/ paths)
    SAHooks sHooks{};
    setupQFileHooks(&sHooks);

    SHPHandle shpHandle = SHPOpenLL(shpFile.toUtf8().constData(), "rb", &sHooks);
    if (!shpHandle) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "SHPOpen failed."));
    }

    return shpHandle;
}

ShapeFileHelper::ShapeType SHPFileHelper::determineShapeType(const QString &shpFile, QString &errorString)
{
    using ShapeType = ShapeFileHelper::ShapeType;

    ShapeType shapeType = ShapeType::Error;

    errorString.clear();

    int utmZone;
    bool utmSouthernHemisphere;
    SHPHandle shpHandle = SHPFileHelper::_loadShape(shpFile, &utmZone, &utmSouthernHemisphere, errorString);
    if (errorString.isEmpty()) {
        Q_CHECK_PTR(shpHandle);

        int cEntities, type;
        SHPGetInfo(shpHandle, &cEntities, &type, nullptr, nullptr);
        qCDebug(SHPFileHelperLog) << "SHPGetInfo" << shpHandle << cEntities << type;
        if (cEntities < 1) {
            errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "No entities found."));
        } else if (type == SHPT_POLYGON || type == SHPT_POLYGONZ) {
            shapeType = ShapeType::Polygon;
        } else if (type == SHPT_ARC || type == SHPT_ARCZ) {
            shapeType = ShapeType::Polyline;
        } else if (type == SHPT_POINT || type == SHPT_POINTZ) {
            shapeType = ShapeType::Point;
        } else {
            errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "No supported types found."));
        }
    }

    if (shpHandle) {
        SHPClose(shpHandle);
    }

    return shapeType;
}

int SHPFileHelper::getEntityCount(const QString &shpFile, QString &errorString)
{
    errorString.clear();

    int utmZone;
    bool utmSouthernHemisphere;
    SHPHandle shpHandle = SHPFileHelper::_loadShape(shpFile, &utmZone, &utmSouthernHemisphere, errorString);
    if (!shpHandle) {
        return 0;
    }

    int cEntities, type;
    SHPGetInfo(shpHandle, &cEntities, &type, nullptr, nullptr);
    SHPClose(shpHandle);

    return cEntities;
}

bool SHPFileHelper::loadStrictPolygonsFromFile(const QString& file, QList<QList<QGeoCoordinate>>& polygons,
                                               QString& errorString)
{
    polygons.clear();
    errorString.clear();
    int utmZone = 0;
    bool southern = false;
    if (!_validateSHPFiles(file, &utmZone, &southern, errorString)) {
        return false;
    }
    SAHooks hooks{};
    setupQFileHooks(&hooks);
    hooks.FOpen = [](const char* filename, const char* access, void*) -> SAFile {
        if (!filename || !access || access[0] != 'r') {
            return nullptr;
        }
        auto* input = new QFile(QString::fromUtf8(filename));
        if (!input->open(QIODevice::ReadOnly)) {
            delete input;
            return nullptr;
        }
        return reinterpret_cast<SAFile>(input);
    };
    hooks.Error = [](const char*) {};
    SHPHandle handle = SHPOpenLL(file.toUtf8().constData(), "rb", &hooks);
    if (!handle) {
        errorString = QStringLiteral("Unable to open SHP file");
        return false;
    }
    auto close = qScopeGuard([handle]() { SHPClose(handle); });
    int count = 0;
    int type = 0;
    SHPGetInfo(handle, &count, &type, nullptr, nullptr);
    if (count == 0) {
        errorString = QStringLiteral("No polygon geometry found");
        return false;
    }
    QList<QList<QGeoCoordinate>> staged;
    for (int entity = 0; entity < count; ++entity) {
        SHPObject* object = SHPReadObject(handle, entity);
        if (!object) {
            errorString = QStringLiteral("entity %1: failed to read object").arg(entity + 1);
            return false;
        }
        auto destroy = qScopeGuard([object]() { SHPDestroyObject(object); });
        if (object->nSHPType != SHPT_POLYGON && object->nSHPType != SHPT_POLYGONZ &&
            object->nSHPType != SHPT_POLYGONM) {
            errorString = QStringLiteral("entity %1: unsupported shape type %2")
                              .arg(entity + 1)
                              .arg(QString::fromLatin1(SHPTypeName(object->nSHPType)));
            return false;
        }
        if (object->nParts > 1) {
            errorString =
                QStringLiteral(
                    "multipart polygon entity %1 needs conversion (holes and multi-polygons are not imported)")
                    .arg(entity + 1);
            return false;
        }
        QList<QGeoCoordinate> ring;
        for (int vertex = 0; vertex < object->nVertices; ++vertex) {
            QGeoCoordinate coordinate;
            // Match the permissive reader's PRJ-driven WGS84/UTM conversion.
            if (utmZone) {
                if (!std::isfinite(object->padfX[vertex]) || !std::isfinite(object->padfY[vertex]) ||
                    !QGCGeo::convertUTMToGeo(object->padfX[vertex], object->padfY[vertex], utmZone, southern,
                                             coordinate)) {
                    errorString = QStringLiteral("entity %1 vertex %2: coordinate conversion failed")
                                      .arg(entity + 1)
                                      .arg(vertex + 1);
                    return false;
                }
            } else {
                const double lat = object->padfY[vertex];
                const double lon = object->padfX[vertex];
                if (!StrictPolygon::validPosition(lat, lon)) {
                    errorString =
                        QStringLiteral("entity %1 vertex %2: invalid coordinate").arg(entity + 1).arg(vertex + 1);
                    return false;
                }
                coordinate = QGeoCoordinate(lat, lon);
            }
            if (!StrictPolygon::validPosition(coordinate.latitude(), coordinate.longitude())) {
                errorString = QStringLiteral("entity %1 vertex %2: invalid coordinate").arg(entity + 1).arg(vertex + 1);
                return false;
            }
            ring.append(coordinate);
        }
        QString reason;
        if (!StrictPolygon::normalize(ring, reason)) {
            errorString =
                QStringLiteral("entity %1 ring (vertices 1-%2): %3").arg(entity + 1).arg(object->nVertices).arg(reason);
            return false;
        }
        staged.append(ring);
    }
    polygons = std::move(staged);
    return true;
}

bool SHPFileHelper::loadPolygonsFromFile(const QString &shpFile, QList<QList<QGeoCoordinate>> &polygons, QString &errorString, double filterMeters)
{
    int utmZone = 0;
    bool utmSouthernHemisphere = false;
    SHPHandle shpHandle = nullptr;

    errorString.clear();
    polygons.clear();

    auto cleanup = qScopeGuard([&]() {
        if (shpHandle) SHPClose(shpHandle);
    });

    shpHandle = SHPFileHelper::_loadShape(shpFile, &utmZone, &utmSouthernHemisphere, errorString);
    if (!errorString.isEmpty()) {
        return false;
    }
    Q_CHECK_PTR(shpHandle);

    int cEntities, shapeType;
    SHPGetInfo(shpHandle, &cEntities, &shapeType, nullptr, nullptr);
    if (shapeType != SHPT_POLYGON && shapeType != SHPT_POLYGONZ) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHPFileHelper", "File contains %1, expected Polygon.").arg(SHPTypeName(shapeType)));
        return false;
    }

    const bool hasAltitude = (shapeType == SHPT_POLYGONZ);

    for (int entityIdx = 0; entityIdx < cEntities; entityIdx++) {
        SHPObject *shpObject = SHPReadObject(shpHandle, entityIdx);
        if (!shpObject) {
            qCWarning(SHPFileHelperLog) << "Failed to read polygon entity" << entityIdx;
            continue;
        }
        auto shpObjectCleanup = qScopeGuard([shpObject]() { SHPDestroyObject(shpObject); });

        // Ensure clockwise winding for outer rings (QGC requirement)
        SHPRewindObject(shpHandle, shpObject);

        // For multi-part polygons (e.g., polygons with holes), we extract only the outer ring.
        // In shapefiles, the first part is conventionally the outer boundary, and subsequent
        // parts are holes (inner rings). For QGC's use cases (survey areas, geofences), the
        // outer boundary is what matters for mission planning.
        const int firstPartEnd = (shpObject->nParts > 1) ? shpObject->panPartStart[1] : shpObject->nVertices;
        if (shpObject->nParts > 1) {
            qCDebug(SHPFileHelperLog) << "Polygon entity" << entityIdx << "has" << shpObject->nParts
                                      << "parts; using outer ring only (" << firstPartEnd << "vertices)";
        }

        QList<QGeoCoordinate> vertices;
        const bool entityHasAltitude = hasAltitude && shpObject->padfZ;

        for (int i = 0; i < firstPartEnd; i++) {
            QGeoCoordinate coord;
            if (utmZone) {
                if (!QGCGeo::convertUTMToGeo(shpObject->padfX[i], shpObject->padfY[i], utmZone, utmSouthernHemisphere, coord)) {
                    qCWarning(SHPFileHelperLog) << "UTM conversion failed for entity" << entityIdx << "vertex" << i;
                    continue;
                }
            } else {
                coord.setLatitude(shpObject->padfY[i]);
                coord.setLongitude(shpObject->padfX[i]);
            }
            if (entityHasAltitude) {
                coord.setAltitude(shpObject->padfZ[i]);
            }
            vertices.append(coord);
        }

        if (vertices.count() < 3) {
            qCWarning(SHPFileHelperLog) << "Skipping polygon entity" << entityIdx << "with less than 3 vertices";
            continue;
        }

        // Filter nearby vertices if enabled
        if (filterMeters > 0) {
            const QGeoCoordinate firstVertex = vertices.first();

            // Detect explicit closure vertex (shapefile rings often repeat first vertex as last).
            // Use a very small threshold (0.01m) so we only treat truly identical points as closure.
            constexpr double kClosureThreshold = 0.01;
            const bool hadExplicitClosure = vertices.last().distanceTo(firstVertex) < kClosureThreshold;

            // Filter consecutive vertices that are too close together
            int i = 0;
            while (i < (vertices.count() - 1)) {
                if ((vertices.count() > 3) && (vertices[i].distanceTo(vertices[i + 1]) < filterMeters)) {
                    vertices.removeAt(i + 1);
                } else {
                    i++;
                }
            }

            // If the original polygon had an explicit closure vertex, remove a single trailing
            // duplicate after filtering, but do not strip distinct vertices that merely happen
            // to be within filterMeters of the first.
            if (hadExplicitClosure && vertices.count() > 3 &&
                vertices.last().distanceTo(firstVertex) < kClosureThreshold) {
                vertices.removeLast();
            }
        }

        polygons.append(vertices);
    }

    if (polygons.isEmpty()) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "No valid polygons found."));
        return false;
    }

    return true;
}

bool SHPFileHelper::loadPolylinesFromFile(const QString &shpFile, QList<QList<QGeoCoordinate>> &polylines, QString &errorString, double filterMeters)
{
    int utmZone = 0;
    bool utmSouthernHemisphere = false;
    SHPHandle shpHandle = nullptr;

    errorString.clear();
    polylines.clear();

    auto cleanup = qScopeGuard([&]() {
        if (shpHandle) SHPClose(shpHandle);
    });

    shpHandle = SHPFileHelper::_loadShape(shpFile, &utmZone, &utmSouthernHemisphere, errorString);
    if (!errorString.isEmpty()) {
        return false;
    }
    Q_CHECK_PTR(shpHandle);

    int cEntities, shapeType;
    SHPGetInfo(shpHandle, &cEntities, &shapeType, nullptr, nullptr);
    if (shapeType != SHPT_ARC && shapeType != SHPT_ARCZ) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHPFileHelper", "File contains %1, expected Arc.").arg(SHPTypeName(shapeType)));
        return false;
    }

    const bool hasAltitude = (shapeType == SHPT_ARCZ);

    for (int entityIdx = 0; entityIdx < cEntities; entityIdx++) {
        SHPObject *shpObject = SHPReadObject(shpHandle, entityIdx);
        if (!shpObject) {
            qCWarning(SHPFileHelperLog) << "Failed to read polyline entity" << entityIdx;
            continue;
        }
        auto shpObjectCleanup = qScopeGuard([shpObject]() { SHPDestroyObject(shpObject); });

        // For multi-part polylines (disconnected segments), we extract only the first part.
        // This maintains consistency with polygon handling and provides the primary path.
        // Each part in a multi-part polyline is typically a separate disconnected segment.
        const int firstPartEnd = (shpObject->nParts > 1) ? shpObject->panPartStart[1] : shpObject->nVertices;
        if (shpObject->nParts > 1) {
            qCDebug(SHPFileHelperLog) << "Polyline entity" << entityIdx << "has" << shpObject->nParts
                                      << "parts; using first part only (" << firstPartEnd << "vertices)";
        }

        QList<QGeoCoordinate> vertices;
        const bool entityHasAltitude = hasAltitude && shpObject->padfZ;

        for (int i = 0; i < firstPartEnd; i++) {
            QGeoCoordinate coord;
            if (utmZone) {
                if (!QGCGeo::convertUTMToGeo(shpObject->padfX[i], shpObject->padfY[i], utmZone, utmSouthernHemisphere, coord)) {
                    qCWarning(SHPFileHelperLog) << "UTM conversion failed for entity" << entityIdx << "vertex" << i;
                    continue;
                }
            } else {
                coord.setLatitude(shpObject->padfY[i]);
                coord.setLongitude(shpObject->padfX[i]);
            }
            if (entityHasAltitude) {
                coord.setAltitude(shpObject->padfZ[i]);
            }
            vertices.append(coord);
        }

        if (vertices.count() < 2) {
            qCWarning(SHPFileHelperLog) << "Skipping polyline entity" << entityIdx << "with less than 2 vertices";
            continue;
        }

        // Filter nearby vertices if enabled
        if (filterMeters > 0) {
            int i = 0;
            while (i < (vertices.count() - 1)) {
                if ((vertices.count() > 2) && (vertices[i].distanceTo(vertices[i+1]) < filterMeters)) {
                    vertices.removeAt(i+1);
                } else {
                    i++;
                }
            }
        }

        polylines.append(vertices);
    }

    if (polylines.isEmpty()) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "No valid polylines found."));
        return false;
    }

    return true;
}

bool SHPFileHelper::loadPointsFromFile(const QString &shpFile, QList<QGeoCoordinate> &points, QString &errorString)
{
    int utmZone = 0;
    bool utmSouthernHemisphere = false;
    SHPHandle shpHandle = nullptr;

    errorString.clear();
    points.clear();

    auto cleanup = qScopeGuard([&]() {
        if (shpHandle) SHPClose(shpHandle);
    });

    shpHandle = SHPFileHelper::_loadShape(shpFile, &utmZone, &utmSouthernHemisphere, errorString);
    if (!errorString.isEmpty()) {
        return false;
    }
    Q_CHECK_PTR(shpHandle);

    int cEntities, shapeType;
    SHPGetInfo(shpHandle, &cEntities, &shapeType, nullptr, nullptr);
    if (shapeType != SHPT_POINT && shapeType != SHPT_POINTZ) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHPFileHelper", "File contains %1, expected Point.").arg(SHPTypeName(shapeType)));
        return false;
    }

    const bool hasAltitude = (shapeType == SHPT_POINTZ);

    for (int entityIdx = 0; entityIdx < cEntities; entityIdx++) {
        SHPObject *shpObject = SHPReadObject(shpHandle, entityIdx);
        if (!shpObject) {
            qCWarning(SHPFileHelperLog) << "Failed to read point entity" << entityIdx;
            continue;
        }
        auto shpObjectCleanup = qScopeGuard([shpObject]() { SHPDestroyObject(shpObject); });

        // Point shapes have exactly one vertex per entity
        if (shpObject->nVertices != 1) {
            qCWarning(SHPFileHelperLog) << "Skipping point entity" << entityIdx << "with unexpected vertex count:" << shpObject->nVertices;
            continue;
        }

        QGeoCoordinate coord;
        if (utmZone) {
            if (!QGCGeo::convertUTMToGeo(shpObject->padfX[0], shpObject->padfY[0], utmZone, utmSouthernHemisphere, coord)) {
                qCWarning(SHPFileHelperLog) << "UTM conversion failed for point entity" << entityIdx;
                continue;
            }
        } else {
            coord.setLatitude(shpObject->padfY[0]);
            coord.setLongitude(shpObject->padfX[0]);
        }

        if (hasAltitude && shpObject->padfZ) {
            coord.setAltitude(shpObject->padfZ[0]);
        }

        points.append(coord);
    }

    if (points.isEmpty()) {
        errorString = QCoreApplication::translate("SHPFileHelper", _errorPrefix).arg(QCoreApplication::translate("SHP", "No valid points found."));
        return false;
    }

    return true;
}
