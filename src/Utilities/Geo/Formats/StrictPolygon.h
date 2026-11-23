#pragma once

#include <cmath>

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>

namespace StrictPolygon {
inline bool validPosition(double lat, double lon)
{
    return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

inline bool normalize(QList<QGeoCoordinate>& ring, QString& reason)
{
    if (ring.size() < 2 || ring.first().latitude() != ring.last().latitude() ||
        ring.first().longitude() != ring.last().longitude()) {
        reason = QStringLiteral("ring is not closed");
        return false;
    }
    ring.removeLast();
    QList<QGeoCoordinate> distinct;
    for (const auto& vertex : ring) {
        if (!distinct.contains(vertex)) {
            distinct.append(vertex);
            if (distinct.size() == 3) {
                return true;
            }
        }
    }
    if (distinct.size() < 3) {
        reason = QStringLiteral("ring has fewer than three distinct vertices");
        return false;
    }
    return true;
}
}  // namespace StrictPolygon
