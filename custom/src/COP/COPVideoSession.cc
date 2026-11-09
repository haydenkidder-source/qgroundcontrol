#include "COPVideoSession.h"

#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QGlobalStatic>
#include <QtCore/QHash>
#include <QtCore/QUrl>

#include "Fact.h"
#include "MultiVehicleManager.h"
#include "QGCCameraManager.h"
#include "QGCCorePlugin.h"
#include "QGCVideoStreamInfo.h"
#include "QtMultimediaVideoReceiver.h"
#include "SettingsManager.h"
#include "VideoBackend.h"
#include "VideoManager.h"
#include "VideoSettings.h"
#include "VideoStreamResolver.h"

namespace {
using COPVideoOwners = QHash<QString, QPointer<COPVideoSession>>;
Q_GLOBAL_STATIC(COPVideoOwners, copVideoOwners)
}  // namespace

COPVideoSession::COPVideoSession(QObject* parent)
    : QObject(parent)
    , _receiverFactory([](QObject* owner) -> VideoReceiver* {
        if (VideoBackend::disabledForUnitTests()) {
            return nullptr;
        }
        if (SettingsManager::instance()->videoSettings()->videoBackend()->rawValue().toInt() ==
            VideoSettings::VideoBackendQtMultimedia) {
            return new QtMultimediaVideoReceiver(owner);
        }
        return QGCCorePlugin::instance()->createVideoReceiver(owner);
    })
    , _requestBackendReady([](QObject* context, std::function<void(bool)> continuation) {
        VideoManager::instance()->whenVideoBackendReady(context, std::move(continuation));
    })
{
    _refresh.setInterval(1000);
    connect(&_refresh, &QTimer::timeout, this, &COPVideoSession::_update);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &COPVideoSession::shutdown);
    connect(MultiVehicleManager::instance(), &MultiVehicleManager::vehicleRemoved, this, [this](Vehicle* vehicle) {
        if (vehicle == _vehicle) {
            setVehicle(nullptr);
        }
    });
    _refresh.start();
    _setStatus(tr("Waiting for advertised video stream"));
}

COPVideoSession::~COPVideoSession()
{
    shutdown();
}

void COPVideoSession::shutdown()
{
    _shutdown = true;
    _refresh.stop();
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
        connect(vehicle, &Vehicle::armedChanged, this, &COPVideoSession::_queueUpdate);
    }
    emit configurationChanged();
    _queueUpdate();
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
        connect(output, &QQuickItem::windowChanged, this, [this] {
            if (_receiver) {
                _receiver->setVideoOutput(_output);
            }
            _queueUpdate();
        });
    }
    emit configurationChanged();
    _queueUpdate();
}

void COPVideoSession::setUri(const QString& uri)
{
    if (_uri == uri) {
        return;
    }
    _invalidate();
    _uri = uri;
    emit configurationChanged();
    _queueUpdate();
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
    _queueUpdate();
}

void COPVideoSession::_queueUpdate()
{
    const quint64 generation = _generation;
    const QPointer<Vehicle> vehicle = _vehicle;
    const QPointer<QQuickItem> output = _output;
    QMetaObject::invokeMethod(
        this,
        [this, generation, vehicle, output] {
            if (generation == _generation && vehicle && vehicle == _vehicle && output && output == _output) {
                _update();
            }
        },
        Qt::QueuedConnection);
}

void COPVideoSession::_setStatus(const QString& status)
{
    _status = status;
    emit statusChanged();
}

void COPVideoSession::_invalidate()
{
    ++_generation;
    _backendPending = false;
    _pendingUri.clear();
}

void COPVideoSession::_releaseEndpoint()
{
    if (!copVideoOwners.isDestroyed() && copVideoOwners->value(_endpoint) == this) {
        copVideoOwners->remove(_endpoint);
    }
    _endpoint.clear();
}

void COPVideoSession::_stop()
{
    _invalidate();
    if (_receiver) {
        disconnect(_receiver, nullptr, this, nullptr);
        // Deleted directly, as VideoStreamController retires a receiver: its destructor unloads the backend and joins
        // its worker before another stream can claim the endpoint. Deactivating or detaching the output first would
        // make a Qt Multimedia receiver wait out its network read on this thread.
        delete _receiver;
    }
    _releaseEndpoint();
    _decoding = false;
    emit statusChanged();
}

void COPVideoSession::_update()
{
    if (_shutdown || !_enabled || !_vehicle || !_output || !_output->window()) {
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
    auto* cameras = _vehicle->cameraManager();
    auto* stream = cameras ? cameras->currentStreamInstance() : nullptr;
    const auto snapshot = VideoStreamResolver::snapshot(settings);
    std::optional<VideoReceiver::Config> config;
    if (_uri.isEmpty()) {
        // COP never falls back to the standard receiver's manually configured endpoint.
        if (VideoStreamResolver::isAutoStream(stream)) {
            config = VideoStreamResolver::resolve(snapshot, stream, VideoStreamResolver::Role::Primary);
        }
    } else {
        config = VideoReceiver::Config{
            .uri = _uri,
            .timeout = std::chrono::seconds(
                _uri.startsWith(QLatin1StringView("rtsp"), Qt::CaseInsensitive) ? snapshot.rtspTimeoutSec : 3),
            .lowLatency = snapshot.lowLatency,
            .rtpJitterLatencyMs = snapshot.rtpJitterLatencyMs,
            .autoReconnect = snapshot.autoReconnect,
        };
    }
    if (!config) {
        _stop();
        _setStatus(tr("No stream advertised by this vehicle"));
        return;
    }
    if (!_backendReady) {
        if (_backendPending && _pendingUri != config->uri) {
            _invalidate();
        }
        if (!_backendPending && _requestBackendReady) {
            _backendPending = true;
            _pendingUri = config->uri;
            const quint64 generation = _generation;
            const QPointer<COPVideoSession> session(this);
            const QPointer<Vehicle> vehicle = _vehicle;
            const QPointer<QQuickItem> output = _output;
            _requestBackendReady(this, [session, generation, vehicle, output](bool ready) {
                if (!session || generation != session->_generation || !vehicle || vehicle != session->_vehicle ||
                    !output || output != session->_output || session->_shutdown || !session->_enabled) {
                    return;
                }
                session->_backendPending = false;
                session->_backendReady = ready;
                session->_setStatus(ready ? tr("Connecting video…") : tr("Video backend unavailable"));
                if (ready) {
                    // Resolve again: advertised URIs and settings may have changed while init was pending.
                    session->_update();
                }
            });
        }
        return;
    }
    const QUrl address(config->uri);
    const QString scheme = address.scheme().toLower();
    const bool datagram =
        scheme == QStringLiteral("udp") || scheme == QStringLiteral("udp265") || scheme == QStringLiteral("mpegts");
    // All UDP encodings/host spellings on a port compete with a wildcard listener on that port.
    const QString endpoint = datagram ? QStringLiteral("udp:%1").arg(address.port()) : config->uri;
    if (copVideoOwners->value(endpoint) && copVideoOwners->value(endpoint) != this) {
        _stop();
        _setStatus(tr("Video address already assigned to another vehicle"));
        return;
    }
    if (!_receiver) {
        _receiver = _receiverFactory(this);
        if (!_receiver) {
            _setStatus(tr("Video receiver unavailable"));
            return;
        }
        _receiver->setName(QStringLiteral("copVideo%1").arg(_vehicle->id()));
        connect(_receiver, &VideoReceiver::decodingChanged, this, [this](bool decoding) {
            _decoding = decoding;
            _setStatus(decoding ? QString() : tr("Waiting for video"));
        });
        connect(_receiver, &VideoReceiver::stateChanged, this, [this](VideoReceiver::State state) {
            if (state == VideoReceiver::State::Reconnecting) {
                _setStatus(tr("Stream unavailable; retrying"));
            }
        });
        connect(_receiver, &VideoReceiver::errorOccurred, this, &COPVideoSession::_setStatus);
        _setStatus(tr("Connecting video…"));
    }
    if (_endpoint != endpoint) {
        _releaseEndpoint();
        _endpoint = endpoint;
        copVideoOwners->insert(endpoint, this);
    }
    const auto display = VideoStreamResolver::displayConfig(snapshot);
    const bool displayChanged = display != _receiver->displayConfig();
    _receiver->setDisplayConfig(display);
    _receiver->setConfig(*config);
    if (_receiver->videoOutput() != _output) {
        _receiver->setVideoOutput(_output);
    }
    if (displayChanged && _receiver->active()) {
        _receiver->restart();
    }
    _receiver->setActive(true);
}
