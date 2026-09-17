#include "COPVideoSession.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QGlobalStatic>
#include <QtCore/QHash>
#include <QtCore/QUrl>

#include "Fact.h"
#include "QGCCameraManager.h"
#include "QGCCorePlugin.h"
#include "QGCVideoStreamInfo.h"
#include "SettingsManager.h"
#include "VideoBackend.h"
#include "VideoManager.h"
#include "VideoSettings.h"

namespace {
using VideoOwners = QHash<QString, QPointer<COPVideoSession>>;
Q_GLOBAL_STATIC(VideoOwners, videoOwners)
}  // namespace

COPVideoSession::COPVideoSession(QObject* parent)
    : QObject(parent)
{
    _refresh.setInterval(1000);
    connect(&_refresh, &QTimer::timeout, this, &COPVideoSession::_update);
    _refresh.start();
    _setStatus(tr("Waiting for advertised video stream"));
}

COPVideoSession::~COPVideoSession()
{
    _stop();
}

void COPVideoSession::setVehicle(Vehicle* vehicle)
{
    if (_vehicle == vehicle) {
        return;
    }
    _stop();
    if (_vehicle) {
        disconnect(_vehicle, nullptr, this, nullptr);
    }
    _vehicle = vehicle;
    if (vehicle) {
        connect(vehicle, &QObject::destroyed, this, &COPVideoSession::_stop);
    }
    emit configurationChanged();
}

void COPVideoSession::setOutput(QQuickItem* output)
{
    if (_output == output) {
        return;
    }
    _stop();
    if (_output) {
        disconnect(_output, nullptr, this, nullptr);
    }
    _output = output;
    if (output) {
        connect(output, &QObject::destroyed, this, &COPVideoSession::_stop);
    }
    emit configurationChanged();
}

void COPVideoSession::setUri(const QString& uri)
{
    if (_uri == uri) {
        return;
    }
    _uri = uri;
    _stop();
    emit configurationChanged();
}

void COPVideoSession::setEnabled(bool enabled)
{
    if (_enabled == enabled) {
        return;
    }
    _enabled = enabled;
    if (!enabled) {
        _stop();
    }
    emit configurationChanged();
}

void COPVideoSession::_setStatus(const QString& status)
{
    _status = status;
    emit statusChanged();
}

void COPVideoSession::_stop()
{
    if (_receiver) {
        void* sink = _receiver->sink();
        // Receiver destruction joins its worker before the sink can be released.
        delete _receiver;
        _receiver = nullptr;
        QGCCorePlugin::instance()->releaseVideoSink(sink);
    }
    if (!videoOwners.isDestroyed() && videoOwners->value(_endpoint) == this) {
        videoOwners->remove(_endpoint);
    }
    _endpoint.clear();
    _decoding = false;
    emit statusChanged();
}

void COPVideoSession::_update()
{
    if (!_enabled || !_vehicle || !_output || !_output->window() || VideoBackend::disabledForUnitTests()) {
        _stop();
        return;
    }
    auto* settings = SettingsManager::instance()->videoSettings();
    if (!settings->streamEnabled()->rawValue().toBool() ||
        (settings->disableWhenDisarmed()->rawValue().toBool() && !_vehicle->armed())) {
        _stop();
        _setStatus(tr("Video paused by application settings"));
        return;
    }
    const bool lowLatency = settings->lowLatencyMode()->rawValue().toBool();
    const int jitterLatency = settings->rtpJitterLatencyMs()->rawValue().toInt();
    auto* cameras = _vehicle->cameraManager();
    auto* stream = cameras ? cameras->currentStreamInstance() : nullptr;
    if (_uri.isEmpty() && (!stream || stream->uri().isEmpty())) {
        _stop();
        _setStatus(tr("No stream advertised by this vehicle"));
        return;
    }
    QString uri = _uri.isEmpty() && stream ? stream->uri() : _uri;
    if (_uri.isEmpty() && stream && stream->type() == VIDEO_STREAM_TYPE_RTPUDP &&
        !uri.contains(QStringLiteral("://"))) {
        const QString scheme =
            stream->encoding() == VIDEO_STREAM_ENCODING_H265 ? QStringLiteral("udp265") : QStringLiteral("udp");
        uri = QStringLiteral("%1://0.0.0.0:%2").arg(scheme, uri);
    } else if (_uri.isEmpty() && stream && stream->type() == VIDEO_STREAM_TYPE_MPEG_TS &&
               !uri.contains(QStringLiteral("://"))) {
        uri = QStringLiteral("mpegts://0.0.0.0:%1").arg(uri);
    }
    if (_receiver && _receiver->uri() == uri && _receiver->lowLatency() == lowLatency &&
        _receiver->rtpJitterLatencyMs() == jitterLatency) {
        return;
    }
    _stop();
    if (!_backendReady) {
        if (!_backendPending) {
            _backendPending = true;
            auto* manager = VideoManager::instance();
            QtConcurrent::run([manager]() {
                return manager->waitForVideoBackendReady();
            }).then(this, [this](bool ready) {
                _backendPending = false;
                _backendReady = ready;
                _setStatus(ready ? tr("Connecting video…") : tr("Video backend unavailable"));
            });
        }
        return;
    }
    const QUrl address(uri);
    const bool datagram = address.scheme() == QStringLiteral("udp") || address.scheme() == QStringLiteral("udp265") ||
                          address.scheme() == QStringLiteral("mpegts");
    const QString endpoint = datagram ? QStringLiteral("udp:%1").arg(address.port()) : uri;
    if (videoOwners->value(endpoint) && videoOwners->value(endpoint) != this) {
        _setStatus(tr("Video address already assigned to another vehicle"));
        return;
    }
    _receiver = QGCCorePlugin::instance()->createVideoReceiver(this);
    if (!_receiver) {
        _setStatus(tr("Video receiver unavailable"));
        return;
    }
    _endpoint = endpoint;
    videoOwners->insert(endpoint, this);
    _receiver->setUri(uri);
    _receiver->setLowLatency(lowLatency);
    _receiver->setRtpJitterLatencyMs(jitterLatency);
    _receiver->setWidget(_output);
    void* sink = QGCCorePlugin::instance()->createVideoSink(_output, _receiver);
    if (!sink) {
        _stop();
        _setStatus(tr("Video output unavailable"));
        return;
    }
    _receiver->setSink(sink);
    VideoBackend::attachSink(_receiver, sink, _output);
    connect(_receiver, &VideoReceiver::onStartComplete, _receiver, [this](VideoReceiver::STATUS status) {
        if (!_receiver) {
            return;
        }
        if (status == VideoReceiver::STATUS_OK) {
            _receiver->startDecoding(_receiver->sink());
        } else {
            _setStatus(tr("Stream unavailable; retrying"));
            _stop();
        }
    });
    connect(_receiver, &VideoReceiver::decodingChanged, _receiver, [this](bool decoding) {
        _decoding = decoding;
        _setStatus(decoding ? QString() : tr("Waiting for video"));
    });
    _setStatus(tr("Connecting video…"));
    _receiver->start(address.scheme() == QStringLiteral("rtsp") ? settings->rtspTimeout()->rawValue().toUInt() : 3);
}
