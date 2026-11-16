#include "GstVideoReceiverPipelineTest.h"

#ifdef QGC_GST_STREAMING

#include <memory>
#include <utility>
#include <vector>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtMultimedia/QVideoSink>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>

#include "Fixtures/RAIIFixtures.h"
#include "GStreamerDecoderRanks.h"
#include "GStreamerLogging.h"
#include "GstScoped.h"
#include "MockVideoStreamServer.h"

namespace {

using GstReceiverTest::kFrameHeight;
using GstReceiverTest::kFrameWidth;
using GstReceiverTest::kWatchdogTimeout;
using GstReceiverTest::loopbackUri;
using GstReceiverTest::Receiver;
using GstReceiverTest::State;
using GstReceiverTest::udpListenUri;

}  // namespace

QString GstVideoReceiverPipelineTest::failureContextSummary() const
{
    QString summary = GstVideoReceiverTestBase::failureContextSummary();
    if (_mockStreamFailureContext) {
        summary += '\n' + _mockStreamFailureContext();
    }
    return summary;
}

void GstVideoReceiverPipelineTest::_testStreamingStatesAndDisplay()
{
    Receiver stream;
    QCOMPARE(stream.receiver->state(), State::Stopped);
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());

    // Every state on the way, each once, and streaming exactly while Streaming.
    QCOMPARE(stream.stateSpy.count(), 2);
    QCOMPARE(stream.stateSpy.at(0).constFirst().value<State>(), State::Connecting);
    QCOMPARE(stream.stateSpy.at(1).constFirst().value<State>(), State::Streaming);
    QCOMPARE(stream.streamingSpy.count(), 1);
    QVERIFY(stream.receiver->streaming());

    // The QVideoSink's size, so it is the displayed one.
    QVERIFY_SIGNAL_COUNT_WAIT(stream.sizeSpy, 1, TestTimeout::mediumMs());
    QCOMPARE(stream.receiver->videoSize(), QSize(kFrameWidth, kFrameHeight));
    QVERIFY_TRUE_WAIT(!stream.receiver->decoderName().isEmpty(), TestTimeout::mediumMs());

    const quint16 port = stream.udpPort();
    stream.receiver->setActive(false);
    QVERIFY(stream.waitForState(State::Stopped));
    QVERIFY_TRUE_WAIT(!stream.receiver->decoding(), TestTimeout::shortMs());
    QVERIFY(!stream.receiver->streaming());
    QVERIFY(stream.errorSpy.isEmpty());

    QUdpSocket rebind;
    QVERIFY2(rebind.bind(QHostAddress::LocalHost, port, QAbstractSocket::DontShareAddress),
             "Stopped receiver still holds its UDP port");
}

void GstVideoReceiverPipelineTest::_testStaticSourcePadWaitsForData()
{
    // udp265 exposes its source pad before any data arrives, which alone isn't streaming.
    Receiver stream(loopbackUri(QLatin1StringView("udp265"), 0));
    stream.receiver->setActive(true);
    QVERIFY(stream.waitForState(State::Connecting));
    QVERIFY(stream.streamingSpy.isEmpty());
    QVERIFY(!stream.receiver->streaming());
}

void GstVideoReceiverPipelineTest::_testDecodesMockLinkStreams_data()
{
    QTest::addColumn<int>("streamType");

    using StreamType = MockVideoStreamServer::StreamType;
    QTest::newRow("rtp h264 udp") << int(StreamType::RtpUdpH264);
    QTest::newRow("rtp h265 udp") << int(StreamType::RtpUdpH265);
    QTest::newRow("mpeg-ts udp") << int(StreamType::MpegTsUdp);
    QTest::newRow("mpeg-ts tcp") << int(StreamType::MpegTsTcp);
#ifdef QGC_GST_RTSP_SERVER
    QTest::newRow("rtsp") << int(StreamType::RtspH264);
#endif
}

void GstVideoReceiverPipelineTest::_testDecodesMockLinkStreams()
{
#ifndef QGC_HAS_MOCK_VIDEO_STREAM_SERVER
    QSKIP("MockVideoStreamServer is only built into single-config Debug builds");
#else
    QFETCH(int, streamType);

    using StreamType = MockVideoStreamServer::StreamType;
    const auto type = static_cast<StreamType>(streamType);
    if (!MockVideoStreamServer::isStreamTypeAvailable(type)) {
        QSKIP("This GStreamer install lacks the stream type's encoder or muxer");
    }

    const bool diagnoseH265 = (type == StreamType::RtpUdpH265);
    const GstDebugLevel previousDefault = gst_debug_get_default_threshold();
    const gboolean previousActive = gst_debug_is_active();
    const GstDebugColorMode previousColor = gst_debug_get_color_mode();
    const QByteArray previousDebug = qgetenv("GST_DEBUG");
    const bool useQtLogger = !GStreamer::nativeDebugLogRequested();
    GPrintFunc previousPrintErr = nullptr;
    std::vector<std::pair<GstDebugCategory*, GstDebugLevel>> previousThresholds;
    const auto restoreNativeLogging = qScopeGuard([&]() {
        if (!diagnoseH265) {
            return;
        }
        g_set_printerr_handler(previousPrintErr);
        // QGC installs only a default threshold; preserve ambient GST_DEBUG rules and category overrides too.
        gst_debug_set_threshold_from_string(previousDebug.constData(), TRUE);
        gst_debug_set_default_threshold(previousDefault);
        for (const auto& [category, threshold] : previousThresholds) {
            gst_debug_category_set_threshold(category, threshold);
        }
        gst_debug_set_active(previousActive);
        gst_debug_set_color_mode(previousColor);
        if (useQtLogger) {
            gst_debug_remove_log_function(gst_debug_log_default);
            gst_debug_add_log_function(GStreamer::qtGstLog, nullptr, nullptr);
        }
    });

    // QGC's filter overrides QT_LOGGING_RULES; strict capture includes debug and info too.
    std::vector<std::unique_ptr<TestFixtures::LoggingCategoryFixture>> diagnosticLogging;
    if (diagnoseH265) {
        GSList* const categories = gst_debug_get_all_categories();
        for (const GSList* entry = categories; entry; entry = entry->next) {
            auto* const category = static_cast<GstDebugCategory*>(entry->data);
            previousThresholds.emplace_back(category, gst_debug_category_get_threshold(category));
        }
        g_slist_free(categories);
        // GStreamer's default log function prints through g_printerr(), which QGC redirects to a Qt warning that
        // strict logging fails the row for; a null handler restores GLib's plain stderr output.
        previousPrintErr = g_set_printerr_handler(nullptr);
        if (useQtLogger) {
            gst_debug_remove_log_function(GStreamer::qtGstLog);
            gst_debug_add_log_function(gst_debug_log_default, nullptr, nullptr);
        }
        gst_debug_set_color_mode(GST_DEBUG_COLOR_MODE_OFF);
        gst_debug_set_threshold_from_string(
            "0,GST_STATES:4,decodebin3:4,h265parse:4,rtph265depay:4,udpsrc:4,x265enc:4,rtph265pay:4,"
            "rtpjitterbuffer:4,avdec*:4,libav:4,d3d11*:4,mf*:4,qgc:4",
            TRUE);
        for (const char* category :
             {"Video.GStreamer.GstVideoReceiver", "Video.GStreamer.GstSourceFactory", "Video.GStreamer.DecoderRanks",
              "Video.GStreamer.QGCQVideoSinkController", "VideoManager.MockVideoStreamServer"}) {
            const QRegularExpression anyMessage(QStringLiteral(".*"), QRegularExpression::DotMatchesEverythingOption);
            ignoreLogMessage(category, QtDebugMsg, anyMessage);
            ignoreLogMessage(category, QtInfoMsg, anyMessage);
            diagnosticLogging.emplace_back(
                std::make_unique<TestFixtures::LoggingCategoryFixture>(QString::fromLatin1(category)));
        }
    }

    QElapsedTimer elapsed;
    elapsed.start();
    qint64 senderStartedMs = -1;

    // The rows cover the stream paths, not decoder choice. CI's macOS VMs can't hardware-decode H.265, and
    // decodebin3 doesn't fall back from a hardware decoder that fails to start. Outlives the receiver.
    const GstTest::DecoderRankSnapshot decoderRanks;
    GStreamer::setCodecPriorities(GStreamer::ForceVideoDecoderSoftware);

    // Servers listen on a free port; for UDP the receiver does, and the server sends to it.
    MockVideoStreamServer server;
    const bool receiverListens =
        (type == StreamType::RtpUdpH264) || (type == StreamType::RtpUdpH265) || (type == StreamType::MpegTsUdp);
    QString uri;
    if (receiverListens) {
        const QLatin1StringView scheme = (type == StreamType::RtpUdpH264)   ? QLatin1StringView("udp")
                                         : (type == StreamType::RtpUdpH265) ? QLatin1StringView("udp265")
                                                                            : QLatin1StringView("mpegts");
        uri = loopbackUri(scheme, 0);
    } else {
        QVERIFY(server.start(type, QStringLiteral("127.0.0.1"), 0));
        senderStartedMs = elapsed.elapsed();
        uri = server.servedUri();
    }

    Receiver stream(uri);
    quint16 receiverUdpPort = 0;
    qint64 activatedMs = -1;
    qint64 streamingMs = -1;
    qint64 displayStartedMs = -1;
    _mockStreamFailureContext = [&]() {
        QString context;
        {
            QDebug out(&context);
            out.noquote().nospace();
            const auto& config = stream.receiver->config();
            const auto& display = stream.receiver->displayConfig();
            const auto stats = stream.receiver->stats();
            out << "Mock stream: type=" << streamType << " uri=" << config.uri << '\n'
                << "Elapsed ms: now=" << elapsed.elapsed() << " activated=" << activatedMs
                << " senderStarted=" << senderStartedMs << " streaming=" << streamingMs
                << " displayStarted=" << displayStartedMs << '\n'
                << "Receiver: state=" << stream.receiver->state() << " streaming=" << stream.receiver->streaming()
                << " decoding=" << stream.receiver->decoding() << " videoSize=" << stream.receiver->videoSize()
                << " active=" << stream.receiver->active() << '\n'
                << "Stats (GUI-cached): valid=" << stats.valid << " keyframeIntervalMs=" << stats.keyframeIntervalMs
                << '\n'
                << "Decoder: requested=ForceVideoDecoderSoftware reported=" << stream.receiver->decoderName()
                << " GST_PLUGIN_FEATURE_RANK=" << qgetenv("GST_PLUGIN_FEATURE_RANK") << '\n'
                << "Config: timeoutSec=" << config.timeout.count() << " lowLatency=" << config.lowLatency
                << " rtpJitterLatencyMs=" << config.rtpJitterLatencyMs << " autoReconnect=" << config.autoReconnect
                << '\n'
                << "Display: conversionElement=" << display.conversionElement
                << " disablePixelAspectRatio=" << display.disablePixelAspectRatio
                << " gpuZeroCopy=" << display.gpuZeroCopy
                << " outputBound=" << (stream.receiver->videoOutput() != nullptr)
                << " windowVisible=" << stream.window.isVisible() << " QT_QPA_PLATFORM=" << qgetenv("QT_QPA_PLATFORM")
                << " QT_QUICK_BACKEND=" << qgetenv("QT_QUICK_BACKEND") << '\n'
                << "MockVideoStreamServer: running=" << server.isRunning() << " servedUri=" << server.servedUri()
                << " receiverUdpPort=" << receiverUdpPort << " frames=" << server.framesSent()
                << " lastFrameAgoMs=" << server.lastFrameAgoMs() << " counter=rtp-h265-payloader-input" << '\n'
                << "FixtureRtpSender (unused in this row): frames=" << stream.sender.frames() << " UDP port=not started"
                << '\n';
            const auto appendSpy = [&out](const char* name, const QSignalSpy& spy) {
                out << name << ": count=" << spy.count() << " last=";
                if (spy.isEmpty()) {
                    out << "<none>";
                } else {
                    out << spy.constLast();
                }
                out << '\n';
            };
            appendSpy("stateSpy", stream.stateSpy);
            appendSpy("streamingSpy", stream.streamingSpy);
            appendSpy("decodingSpy", stream.decodingSpy);
            appendSpy("sizeSpy", stream.sizeSpy);
            appendSpy("errorSpy", stream.errorSpy);
            for (const auto& error : stream.errorSpy) {
                out << "errorOccurred: " << error.constFirst().toString() << '\n';
            }
        }
        return context;
    };
    // Dump while the locals exist, before teardown can wait on a stuck worker. Never query its pipeline here.
    const auto dumpBeforeTeardown = qScopeGuard([this]() {
        dumpFailureContextIfTestFailed(u"mock stream before teardown");
        _mockStreamFailureContext = {};
    });

    // Bind the output before activation so decoder setup does not depend on late display attachment.
    stream.window.show();
    stream.receiver->setVideoOutput(stream.output);
    activatedMs = elapsed.elapsed();
    stream.receiver->setActive(true);
    if (receiverListens) {
        // Connecting lasts until the server sends. A server that already does can take the receiver on to
        // Streaming before this thread sees Connecting.
        QVERIFY(stream.waitForState(State::Connecting));
        receiverUdpPort = stream.waitForUdpPort();
        QVERIFY(server.start(type, QStringLiteral("127.0.0.1"), receiverUdpPort));
        senderStartedMs = elapsed.elapsed();
    }
    QVERIFY(stream.waitForState(State::Streaming));
    streamingMs = elapsed.elapsed();
    displayStartedMs = elapsed.elapsed();
    QVERIFY(stream.display());
#endif
}

void GstVideoReceiverPipelineTest::_testConfigChangeRestarts()
{
    Receiver stream;
    QVERIFY(stream.startStreaming());
    const GStreamer::GstElementPtr first = stream.pipeline();

    // Live: no restart.
    VideoReceiver::Config config = stream.receiver->config();
    config.autoReconnect = !config.autoReconnect;
    stream.receiver->setConfig(config);
    QCOMPARE(stream.pipeline().get(), first.get());
    QCOMPARE(stream.receiver->state(), State::Streaming);

    // Anything else applies to a new pipeline at once.
    stream.stateSpy.clear();
    config.lowLatency = !config.lowLatency;
    stream.receiver->setConfig(config);
    QVERIFY(stream.resumeSending());
    QVERIFY(stream.pipeline().get() != first.get());
    QCOMPARE(stream.stateSpy.constFirst().constFirst().value<State>(), State::Connecting);
    QVERIFY(stream.errorSpy.isEmpty());

    // An empty URI stops the stream until one is set again.
    config.uri.clear();
    stream.receiver->setConfig(config);
    QVERIFY(stream.waitForState(State::Stopped));
    config.uri = udpListenUri();
    stream.receiver->setConfig(config);
    QVERIFY(stream.resumeSending());
}

void GstVideoReceiverPipelineTest::_testOutputDetachKeepsPipeline()
{
    Receiver stream;
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());
    const GStreamer::GstElementPtr pipeline = stream.pipeline();
    stream.stateSpy.clear();

    stream.receiver->setVideoOutput(nullptr);
    QVERIFY_TRUE_WAIT(!stream.receiver->decoding(), TestTimeout::mediumMs());

    // A destroyed output detaches too; a new one shows the stream again.
    auto* const replacement = new QQuickVideoOutput(stream.window.contentItem());
    replacement->setSize(QSizeF(kFrameWidth, kFrameHeight));
    stream.receiver->setVideoOutput(replacement);
    QVERIFY_TRUE_WAIT(stream.receiver->decoding(), TestTimeout::mediumMs());
    QVERIFY_TRUE_WAIT(replacement->videoSink()->videoFrame().isValid(), TestTimeout::mediumMs());
    delete replacement;
    QCOMPARE(stream.receiver->videoOutput(), nullptr);
    QVERIFY_TRUE_WAIT(!stream.receiver->decoding(), TestTimeout::mediumMs());

    QVERIFY(stream.display());
    QCOMPARE(stream.pipeline().get(), pipeline.get());
    QVERIFY2(stream.stateSpy.isEmpty(), "Changing the output restarted the stream");
}

void GstVideoReceiverPipelineTest::_testRestartRebuildsVideoSink()
{
    Receiver stream;
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());
    const GStreamer::GstElementPtr firstSink =
        GStreamer::adoptElement(GST_ELEMENT(gst_object_ref(displaySink(stream.receiver.get()))));
    const GStreamer::GstElementPtr firstPipeline = stream.pipeline();

    stream.receiver->restart();
    QVERIFY(displaySink(stream.receiver.get()) != firstSink.get());
    QVERIFY(stream.resumeSending());
    QVERIFY(stream.pipeline().get() != firstPipeline.get());
    QVERIFY_TRUE_WAIT(stream.receiver->decoding(), TestTimeout::mediumMs());
    // Only the receiver's ref and this one were ever taken on the replaced sink; the pipeline let go of it.
    QCOMPARE(GST_OBJECT_REFCOUNT_VALUE(firstSink.get()), 1);
}

void GstVideoReceiverPipelineTest::_testDecoderQosDisabled()
{
    Receiver stream;
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());

    // The decoders decodebin3 plugged, inspected on the worker that owns the pipeline.
    int decoders = 0;
    QStringList qosEnabled;
    QVERIFY(runOnPipeline(stream.receiver.get(), [&decoders, &qosEnabled](GstElement* pipeline) {
        for (const GStreamer::GstElementPtr& element : GstTest::childElements(GST_BIN(pipeline), /*recurse=*/true)) {
            GstElementFactory* const factory = gst_element_get_factory(element.get());
            if (!factory || !gst_element_factory_list_is_type(factory, GST_ELEMENT_FACTORY_TYPE_DECODER) ||
                !g_object_class_find_property(G_OBJECT_GET_CLASS(element.get()), "qos")) {
                continue;
            }
            ++decoders;
            gboolean qos = TRUE;
            g_object_get(element.get(), "qos", &qos, nullptr);
            if (qos) {
                qosEnabled.append(QString::fromUtf8(GST_OBJECT_NAME(element.get())));
            }
        }
    }));
    QVERIFY(decoders > 0);
    QVERIFY2(qosEnabled.isEmpty(), qPrintable(qosEnabled.join(u',')));
}

void GstVideoReceiverPipelineTest::_testScreenshotSavesDisplayedFrame()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Receiver stream;
    const QString path = tempDir.filePath(QStringLiteral("screenshot.png"));

    expectLogMessage("Video.VideoReceiver", QtWarningMsg,
                     QRegularExpression(QStringLiteral("^No video output to take a screenshot from")));
    QVERIFY(!stream.receiver->takeScreenshot(path));
    verifyExpectedLogMessage();

    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());
    QVERIFY_TRUE_WAIT(stream.videoSink()->videoFrame().isValid(), TestTimeout::mediumMs());

    // Qt Multimedia converts on the CPU when the platform (offscreen here) has no RHI, and says so.
    ignoreLogMessage("default", QtWarningMsg, QRegularExpression(QStringLiteral("No RHI backend")));
    QVERIFY(stream.receiver->takeScreenshot(path));
    QCOMPARE(QImage(path).size(), QSize(kFrameWidth, kFrameHeight));
}

void GstVideoReceiverPipelineTest::_testDeactivateReleasesDisplayedFrame()
{
    Receiver stream;
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());
    QVERIFY_TRUE_WAIT(stream.videoSink()->videoFrame().isValid(), TestTimeout::mediumMs());

    stream.receiver->setActive(false);
    QVERIFY_TRUE_WAIT(!stream.videoSink()->videoFrame().isValid(), TestTimeout::mediumMs());

    // Releasing the frame keeps the output bound: the next stream shows again.
    QVERIFY(stream.startStreaming());
    QVERIFY_TRUE_WAIT(stream.videoSink()->videoFrame().isValid(), TestTimeout::mediumMs());
}

void GstVideoReceiverPipelineTest::_testDecodesH265Stream()
{
    GSTTEST_SKIP_UNLESS_FACTORIES("avdec_h265");
    // The stream path, not decoder choice, is under test; a hardware decoder that fails to start isn't replaced.
    const GstTest::DecoderRankSnapshot decoderRanks;
    GStreamer::setCodecPriorities(GStreamer::ForceVideoDecoderSoftware);

    Receiver stream(loopbackUri(QLatin1StringView("udp265"), 0), /*autoReconnect=*/false, kWatchdogTimeout,
                    QString::fromLatin1(GstTest::kH265Stream));
    QVERIFY2(stream.sender.error().isEmpty(), qPrintable(stream.sender.error()));
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());
    QVERIFY_TRUE_WAIT(stream.receiver->videoSize() == QSize(kFrameWidth, kFrameHeight), TestTimeout::mediumMs());
    QVERIFY(stream.errorSpy.isEmpty());
}

void GstVideoReceiverPipelineTest::_testResolutionChangeUpdatesVideoSize()
{
    Receiver stream;
    QVERIFY(stream.startStreaming());
    QVERIFY(stream.display());
    QVERIFY_TRUE_WAIT(stream.receiver->videoSize() == QSize(kFrameWidth, kFrameHeight), TestTimeout::mediumMs());
    const qsizetype stateChanges = stream.stateSpy.count();
    const qsizetype sizeChanges = stream.sizeSpy.count();

    // A camera switching resolution mid-stream: its next keyframe carries the new parameter sets.
    QVERIFY2(stream.sender.switchTo(QString::fromLatin1(GstTest::kH264SmallStream)), qPrintable(stream.sender.error()));
    QVERIFY_TRUE_WAIT(stream.receiver->videoSize() == QSize(160, 120), TestTimeout::mediumMs());
    QVERIFY(stream.sizeSpy.count() > sizeChanges);

    // Taken in stride: no reconnect, no error.
    QCOMPARE(stream.stateSpy.count(), stateChanges);
    QCOMPARE(stream.receiver->state(), State::Streaming);
    QVERIFY(stream.errorSpy.isEmpty());
}

#endif

UT_REGISTER_TEST(GstVideoReceiverPipelineTest, TestLabel::Integration)
