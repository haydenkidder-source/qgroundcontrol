#include "COPReferencePoint.h"

#include "GPSCorrectionManager.h"
#include "GPSManager.h"
#include "RTCMParser.h"
#include <GeographicLib/Geocentric.hpp>

namespace {
quint64 bits(const QByteArray& bytes, int offset, int count)
{
    quint64 result = 0;
    for (int i = offset; i < offset + count; ++i) {
        result = (result << 1) | ((static_cast<quint8>(bytes[i / 8]) >> (7 - i % 8)) & 1);
    }
    return result;
}

double ecef(const QByteArray& bytes, int offset)
{
    const quint64 raw = bits(bytes, offset, 38);
    const qint64 signedValue =
        (raw & (quint64{1} << 37)) ? static_cast<qint64>(raw) - (qint64{1} << 38) : static_cast<qint64>(raw);
    return static_cast<double>(signedValue) * 0.0001;
}
}  // namespace

COPReferencePoint::COPReferencePoint(QObject* parent)
    : QObject(parent)
{
    _expiry.setSingleShot(true);
    _expiry.setInterval(30000);
    connect(&_expiry, &QTimer::timeout, this, [this]() {
        _coordinate = {};
        emit changed();
    });
    if (auto* corrections = GPSManager::instance()->corrections()) {
        connect(corrections, &GPSCorrectionManager::correctionRouted, this, &COPReferencePoint::_accept);
    }
}

QGeoCoordinate COPReferencePoint::decode(const QByteArray& frame)
{
    if (frame.size() < 25 || static_cast<quint8>(frame[0]) != RTCMParser::kPreamble) {
        return {};
    }
    const auto message = bits(frame, 24, 12);
    const int payloadSize = static_cast<int>(bits(frame, 14, 10));
    if ((message != 1005 && message != 1006) || payloadSize < (message == 1005 ? 19 : 21) ||
        frame.size() != payloadSize + 6) {
        return {};
    }
    const auto crc = RTCMParser::crc24q(reinterpret_cast<const uint8_t*>(frame.constData()), frame.size() - 3);
    if (crc != bits(frame, (frame.size() - 3) * 8, 24)) {
        return {};
    }
    // RTCM 1005/1006 carry signed 38-bit antenna-reference ECEF coordinates, in 0.1 mm units.
    const double x = ecef(frame, 58);
    const double y = ecef(frame, 98);
    const double z = ecef(frame, 138);
    if (x == 0 && y == 0 && z == 0) {
        return {};
    }
    double latitude = 0;
    double longitude = 0;
    double altitude = 0;
    GeographicLib::Geocentric::WGS84().Reverse(x, y, z, latitude, longitude, altitude);
    return {latitude, longitude, altitude};
}

void COPReferencePoint::_accept(const GPSCorrectionFrame& frame)
{
    if (!frame.validated || frame.filtered) {
        return;
    }
    const QString instance = QString::number(static_cast<int>(frame.source)) + QLatin1Char(':') + frame.sourceInstance;
    if (_instance != instance || _session != frame.session) {
        _coordinate = {};
        _instance = instance;
        _session = frame.session;
        emit changed();
    }
    const auto coordinate = decode(frame.data);
    if (coordinate.isValid()) {
        _coordinate = coordinate;
        _source = frame.source == GPSCorrectionSource::Ntrip ? tr("NTRIP reference") : tr("RTK reference");
        emit changed();
    }
    if (_coordinate.isValid()) {
        _expiry.start();
    }
}
