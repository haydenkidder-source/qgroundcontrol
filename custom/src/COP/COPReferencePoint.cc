#include "COPReferencePoint.h"

#include "GPSCorrectionManager.h"
#include "GPSCorrectionSettings.h"
#include "GPSManager.h"
#include "RTCMFramer.h"
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

COPReferencePoint::COPReferencePoint(QObject* parent, GPSCorrectionManager* corrections, int expiryMs)
    : QObject(parent)
{
    _expiry.setSingleShot(true);
    _expiry.setInterval(expiryMs);
    connect(&_expiry, &QTimer::timeout, this, [this]() {
        _coordinate = {};
        emit changed();
    });
    if (!corrections) {
        corrections = GPSManager::instance()->corrections();
    }
    if (corrections) {
        connect(corrections, &GPSCorrectionManager::selectedFrameAdmitted, this, &COPReferencePoint::_accept);
        // A stream that ends, goes stale or loses the selection must not keep showing its reference.
        connect(corrections, &GPSCorrectionManager::stateChanged, this, [this, corrections]() {
            if (_hasStream && (corrections->state() != GPSCorrectionManager::State::Fresh ||
                               corrections->selectedStream() != _stream)) {
                _drop();
            }
        });
    }
}

void COPReferencePoint::_drop()
{
    _hasStream = false;
    _expiry.stop();
    if (_coordinate.isValid()) {
        _coordinate = {};
        emit changed();
    }
}

QGeoCoordinate COPReferencePoint::decode(const QByteArray& frame)
{
    if (frame.size() < 25 || !RTCMFramer::isValidFrame(QByteArrayView(frame))) {
        return {};
    }
    const auto message = bits(frame, 24, 12);
    const int payloadSize = static_cast<int>(bits(frame, 14, 10));
    if ((message != 1005 && message != 1006) || payloadSize < (message == 1005 ? 19 : 21)) {
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

void COPReferencePoint::_accept(const QByteArray& frame, const GPSCorrectionStream& stream, quint64 session)
{
    if (!_hasStream || _stream != stream || _session != session) {
        _drop();
        _hasStream = true;
        _stream = stream;
        _session = session;
    }
    const auto coordinate = decode(frame);
    if (coordinate.isValid()) {
        _coordinate = coordinate;
        _source = stream.source == GPSCorrectionSettings::Ntrip ? tr("NTRIP reference") : tr("RTK reference");
        emit changed();
    }
    if (_coordinate.isValid()) {
        _expiry.start();
    }
}
