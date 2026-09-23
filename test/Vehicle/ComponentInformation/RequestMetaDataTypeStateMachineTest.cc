#include "RequestMetaDataTypeStateMachineTest.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryFile>
#include <QtCore/QUuid>
#include <QtTest/QSignalSpy>

#include "CompInfoGeneral.h"
#include "CompInfoParam.h"
#include "ComponentInformationCache.h"
#include "ComponentInformationManager.h"
#include "FTPManager.h"
#include "FactMetaData.h"
#include "LinkManager.h"
#include "MockConfiguration.h"
#include "MockLink.h"
#include "MockLinkFTP.h"
#include "MultiVehicleManager.h"
#include "QGCCompression.h"
#include "QGCLoggingCategoryManager.h"
#include "RequestMetaDataTypeStateMachine.h"
#include "UnitTest.h"
#include "Vehicle.h"

void RequestMetaDataTypeStateMachineTest::_typeToStringReflectsRequestedType()
{
    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);

    RequestMetaDataTypeStateMachine requestMachine(manager, this);

    auto* general = manager->compInfoGeneral(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(general);
    requestMachine.request(general);
    QCOMPARE(requestMachine.typeToString(), QStringLiteral("COMP_METADATA_TYPE_GENERAL"));

    auto* param = manager->compInfoParam(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(param);
    requestMachine.request(param);
    QCOMPARE(requestMachine.typeToString(), QStringLiteral("COMP_METADATA_TYPE_PARAMETER"));
}

void RequestMetaDataTypeStateMachineTest::_requestCompleteEmittedForGeneral()
{
    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    auto* general = manager->compInfoGeneral(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(general);

    requestMachine.request(general);
    if (completeSpy.count() == 0) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    QCOMPARE(completeSpy.count(), 1);
    QVERIFY(!requestMachine.active());
    QVERIFY2(general->available() || !general->uriMetaDataFallback().isEmpty(),
             "General metadata URI is empty after request");
}

void RequestMetaDataTypeStateMachineTest::_requestCompleteEmittedForParameter()
{
    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    auto* param = manager->compInfoParam(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(param);

    requestMachine.request(param);
    if (completeSpy.count() == 0) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    QCOMPARE(completeSpy.count(), 1);
    QVERIFY(!requestMachine.active());
}

void RequestMetaDataTypeStateMachineTest::_sequentialRequestsReuseMachine()
{
    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    auto* general = manager->compInfoGeneral(MAV_COMP_ID_AUTOPILOT1);
    auto* param = manager->compInfoParam(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(general);
    QVERIFY(param);

    requestMachine.request(general);
    if (completeSpy.count() == 0) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    requestMachine.request(param);
    if (completeSpy.count() == 1) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    QCOMPARE(completeSpy.count(), 2);
    QVERIFY(!requestMachine.active());
}

void RequestMetaDataTypeStateMachineTest::_requestCompletesForArduPilot()
{
    _disconnectMockLink();
    _connectMockLink(MAV_AUTOPILOT_ARDUPILOTMEGA);

    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    auto* general = manager->compInfoGeneral(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(general);

    requestMachine.request(general);
    if (completeSpy.count() == 0) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    QCOMPARE(completeSpy.count(), 1);
    QVERIFY(vehicle()->isInitialConnectComplete());
}

void RequestMetaDataTypeStateMachineTest::_requestSkipsCompInfoOnHighLatencyLink()
{
    // High-latency link skips metadata requests, resulting in the expected failure warning.
    ignoreLogMessage("ComponentInformation.RequestMetaDataTypeStateMachine", QtWarningMsg,
                     QRegularExpression("failed to load metadata"));
    _disconnectMockLink();

    LinkManager::instance()->setConnectionsAllowed();
    auto* mvm = MultiVehicleManager::instance();
    QVERIFY(!mvm->activeVehicle());

    QSignalSpy activeVehicleSpy{mvm, &MultiVehicleManager::activeVehicleChanged};
    auto* mockConfig = new MockConfiguration(QStringLiteral("HighLatencyCompInfoMock"));
    mockConfig->setFirmwareType(MAV_AUTOPILOT_PX4);
    mockConfig->setVehicleType(MAV_TYPE_QUADROTOR);
    mockConfig->setHighLatency(true);
    mockConfig->setDynamic(true);

    SharedLinkConfigurationPtr linkConfig = LinkManager::instance()->addConfiguration(mockConfig);
    QVERIFY(LinkManager::instance()->createConnectedLink(linkConfig));

    _mockLink = qobject_cast<MockLink*>(linkConfig->link());
    QVERIFY(_mockLink);

    QVERIFY(activeVehicleSpy.wait(TestTimeout::longMs()));
    _vehicle = mvm->activeVehicle();
    QVERIFY(_vehicle);

    QSignalSpy initialConnectCompleteSpy{_vehicle, &Vehicle::initialConnectComplete};
    QVERIFY(initialConnectCompleteSpy.wait(TestTimeout::longMs()) || _vehicle->isInitialConnectComplete());

    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);

    _mockLink->clearReceivedMavCommandCounts();

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    auto* general = manager->compInfoGeneral(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(general);

    requestMachine.request(general);
    if (completeSpy.count() == 0) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(_mockLink->receivedMavCommandCount(MAV_CMD_REQUEST_MESSAGE), 0);
    QVERIFY(!requestMachine.active());

    _disconnectMockLink();
}

void RequestMetaDataTypeStateMachineTest::_requestUsesCachedMetadataForParameter()
{
    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);
    QVERIFY_TRUE_WAIT(!manager->isRunning(), TestTimeout::mediumMs());

    auto* param = manager->compInfoParam(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(param);

    static constexpr uint32_t crc = 0x1234ABCD;
    param->setUriMetaData(QStringLiteral("http://example.invalid/cache-hit-param.json"), crc);

    const QString fileTag = QString::asprintf("%08x_%02i_%i", crc, static_cast<int>(param->type), 0);
    const QString tempJsonFile =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("qgc-compinfo-cache-hit-%1.json")
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QFile file(tempJsonFile);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray jsonMetadata =
        R"({"version":1,"parameters":[{"name":"CACHE_HIT_PARAM","type":"Float","shortDesc":"Loaded from cache"}]})";
    QCOMPARE(file.write(jsonMetadata), jsonMetadata.size());
    file.close();

    const QString cachedPath = manager->fileCache().insert(fileTag, tempJsonFile);
    QVERIFY(!cachedPath.isEmpty());
    QVERIFY(QFile::exists(cachedPath));

    _mockLink->clearReceivedMavCommandCounts();
    const int initialCompMetadataRequests =
        _mockLink->receivedRequestMessageCount(MAV_COMP_ID_AUTOPILOT1, MAVLINK_MSG_ID_COMPONENT_METADATA);
    const int initialCompInformationRequests =
        _mockLink->receivedRequestMessageCount(MAV_COMP_ID_AUTOPILOT1, MAVLINK_MSG_ID_COMPONENT_INFORMATION);

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    requestMachine.request(param);
    if (completeSpy.count() == 0) {
        QVERIFY(completeSpy.wait(TestTimeout::longMs()));
    }

    QCOMPARE(completeSpy.count(), 1);
    QCOMPARE(_mockLink->receivedRequestMessageCount(MAV_COMP_ID_AUTOPILOT1, MAVLINK_MSG_ID_COMPONENT_METADATA),
             initialCompMetadataRequests);
    QCOMPARE(_mockLink->receivedRequestMessageCount(MAV_COMP_ID_AUTOPILOT1, MAVLINK_MSG_ID_COMPONENT_INFORMATION),
             initialCompInformationRequests);
    QVERIFY(!requestMachine.active());

    FactMetaData* metadata = param->factMetaDataForName(QStringLiteral("CACHE_HIT_PARAM"), FactMetaData::valueTypeFloat);
    QVERIFY(metadata);
    QCOMPARE(metadata->shortDescription(), QStringLiteral("Loaded from cache"));
}

// A metadata download that is clearly not going to finish in a sane time must be abandoned early: it sits in
// front of parameter load, and on a slow telemetry link the projection is obvious within a few seconds.
void RequestMetaDataTypeStateMachineTest::_slowFtpDownloadAbortsEarly()
{
    auto* manager = vehicle()->compInfoManager();
    QVERIFY(manager);
    QVERIFY_TRUE_WAIT(!manager->isRunning(), TestTimeout::mediumMs());

    auto* param = manager->compInfoParam(MAV_COMP_ID_AUTOPILOT1);
    QVERIFY(param);
    // Random CRC so the file cache cannot satisfy the request
    param->setUriMetaData(QStringLiteral("mftp://[;comp=1]parameter.json.xz"), QRandomGenerator::global()->generate());

    // ~2 KB/s against a 92 KB file: projected total is far past the abort limit after the first few bursts
    _mockLink->mockLinkFTP()->setBurstReadDelayMs(1000);

    // Debug output for the category is off by default; enable it so the abort message is captured
    const char* category = "ComponentInformation.RequestMetaDataTypeStateMachine";
    QGCLoggingCategoryManager::instance()->setCategoryEnabled(category, true);
    const auto restoreLogging = qScopeGuard([category]() {
        QGCLoggingCategoryManager::instance()->setCategoryEnabled(category, false);
    });
    ignoreLogMessage(category, QtDebugMsg, QRegularExpression(".*"));
    ignoreLogMessage(category, QtWarningMsg, QRegularExpression("failed to load metadata"));
    // The mock's blocking burst delay also starves unrelated commands sent to it during the download
    ignoreLogMessage("Vehicle.StandardModes", QtWarningMsg, QRegularExpression("Failed to retrieve available modes"));
    expectLogMessage(category, QtDebugMsg, QRegularExpression("Slow download, aborting"));

    RequestMetaDataTypeStateMachine requestMachine(manager, this);
    QSignalSpy completeSpy(&requestMachine, &RequestMetaDataTypeStateMachine::requestComplete);
    QVERIFY(completeSpy.isValid());

    QElapsedTimer timer;
    timer.start();
    requestMachine.request(param);
    QVERIFY(UnitTest::waitForSignal(completeSpy, TestTimeout::longMs(), QStringLiteral("requestComplete")));
    const qint64 elapsedMs = timer.elapsed();
    _mockLink->mockLinkFTP()->setBurstReadDelayMs(0);

    verifyExpectedLogMessage();
    // Abort fires on the first progress report 5s after the first data packet; well under the old fixed 10s,
    // with slack for CI load
    const qint64 maxAbortMs = TestTimeout::isCI() ? 15000 : 10000;
    QVERIFY2(elapsedMs < maxAbortMs, qPrintable(QStringLiteral("Slow download ran %1 ms before aborting").arg(elapsedMs)));
    QVERIFY(!requestMachine.active());
}

UT_REGISTER_TEST(RequestMetaDataTypeStateMachineTest, TestLabel::Integration, TestLabel::Vehicle)

void RequestMetaDataTypeStateMachineTest::_concurrentFtpMetadata_data()
{
    QTest::addColumn<bool>("compressed");
    QTest::addColumn<bool>("translation");
    QTest::newRow("json") << false << false;
    QTest::newRow("xz") << true << false;
    QTest::newRow("translation-summary") << false << true;
}

void RequestMetaDataTypeStateMachineTest::_concurrentFtpMetadata()
{
    QFETCH(bool, compressed);
    QFETCH(bool, translation);
    auto* firstVehicle = vehicle();
    QVERIFY(firstVehicle);
    QVERIFY_TRUE_WAIT(!firstVehicle->compInfoManager()->isRunning(), TestTimeout::longMs());

    expectAppMessage(QRegularExpression(QStringLiteral("Connected to Vehicle [0-9]+")));
    auto* secondLink = MockLink::startPX4MockLink();
    QVERIFY(secondLink);
    QPointer<Vehicle> secondVehicle;
    const auto disconnectSecond = qScopeGuard([&] {
        secondLink->disconnect();
        QVERIFY(UnitTest::waitForCondition([&] { return secondVehicle.isNull(); }, TestTimeout::longMs()));
    });
    QVERIFY_TRUE_WAIT(MultiVehicleManager::instance()->getVehicleById(secondLink->vehicleId()), TestTimeout::longMs());
    secondVehicle = MultiVehicleManager::instance()->getVehicleById(secondLink->vehicleId());
    QVERIFY(secondVehicle);
    verifyExpectedLogMessage();
    QVERIFY_TRUE_WAIT(secondVehicle->isInitialConnectComplete(), TestTimeout::longMs());
    QVERIFY_TRUE_WAIT(!secondVehicle->compInfoManager()->isRunning(), TestTimeout::longMs());
    QVERIFY(firstVehicle->id() != secondVehicle->id());

    // Capture exactly what the metadata consumer receives, after decompression and cache insertion.
    class CapturedMetadata : public CompInfo
    {
    public:
        explicit CapturedMetadata(Vehicle* owningVehicle)
            : CompInfo(COMP_METADATA_TYPE_PARAMETER, MAV_COMP_ID_AUTOPILOT1, owningVehicle)
        {}

        void setJson(const QString& path) override
        {
            fileName = path;
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) {
                bytes = file.readAll();
            }
        }

        QString fileName;
        QByteArray bytes;
    };

    CapturedMetadata firstInfo(firstVehicle);
    CapturedMetadata secondInfo(secondVehicle);
    const QString suffix = compressed ? QStringLiteral(".json.xz") : QStringLiteral(".json");
    const QString remotePath = QStringLiteral("/metadata") + suffix;
    const QString firstSource = QStringLiteral(":MockLink/General.MetaData") + suffix;
    const QString secondSource = QStringLiteral(":MockLink/Parameter.MetaData") + suffix;
    QFile firstFile(firstSource);
    QFile secondFile(secondSource);
    QVERIFY(firstFile.open(QIODevice::ReadOnly));
    QVERIFY(secondFile.open(QIODevice::ReadOnly));
    const QByteArray firstBytes =
        compressed ? QGCCompression::decompressData(firstFile.readAll()) : firstFile.readAll();
    const QByteArray secondBytes =
        compressed ? QGCCompression::decompressData(secondFile.readAll()) : secondFile.readAll();
    QVERIFY(!firstBytes.isEmpty());
    QVERIFY(!secondBytes.isEmpty());
    QVERIFY(firstBytes != secondBytes);

    auto* firstServer = mockLink()->mockLinkFTP();
    auto* secondServer = secondLink->mockLinkFTP();
    firstServer->setDownloadFile(remotePath, firstSource);
    secondServer->setDownloadFile(remotePath, secondSource);
    firstServer->setBurstReadDelayMs(100);
    secondServer->setBurstReadDelayMs(5);
    const auto restoreDelay = qScopeGuard([&] { firstServer->setBurstReadDelayMs(0); });
    const QString uri = QStringLiteral("mftp://[;comp=1]") + remotePath;
    const uint32_t firstCrc = QRandomGenerator::global()->generate();
    const uint32_t secondCrc = firstCrc ^ 0xffffffffU;
    firstInfo.setUriMetaData(uri, firstCrc);
    secondInfo.setUriMetaData(uri, secondCrc);
    const bool expectMissingLocale = translation && !QLocale::system().name().startsWith(QLatin1String("en"));
    if (translation) {
        const QString summaryPath = QStringLiteral("/translation.json");
        firstServer->setDownloadFile(summaryPath, firstSource);
        secondServer->setDownloadFile(summaryPath, firstSource);
        for (CapturedMetadata* info : {&firstInfo, &secondInfo}) {
            QJsonObject advertisement = QJsonDocument::fromJson(firstBytes).object();
            advertisement.insert(
                QStringLiteral("metadataTypes"),
                QJsonArray{
                    QJsonObject{{QStringLiteral("type"), static_cast<int>(info->type)},
                                {QStringLiteral("uri"), uri},
                                {QStringLiteral("fileCrc"), static_cast<double>(info->crcMetaData())},
                                {QStringLiteral("translationUri"), QStringLiteral("mftp://[;comp=1]") + summaryPath}}});
            QTemporaryFile advertisementFile;
            QVERIFY(advertisementFile.open());
            const QByteArray json = QJsonDocument(advertisement).toJson();
            QCOMPARE(advertisementFile.write(json), json.size());
            QVERIFY(advertisementFile.flush());
            CompInfoGeneral general(MAV_COMP_ID_AUTOPILOT1, info->vehicle);
            general.setJson(advertisementFile.fileName());
            general.setUris(*info);
            QVERIFY(!info->uriTranslation().isEmpty());
            if (expectMissingLocale) {
                expectLogMessage("ComponentInformation.ComponentInformationTranslation", QtWarningMsg,
                                 QRegularExpression("not found in translation json"));
            }
        }
    }
    const auto cacheTag = [](uint32_t crc) {
        return QString::asprintf("%08x_%02i_%i", crc, static_cast<int>(COMP_METADATA_TYPE_PARAMETER), 0);
    };
    QVERIFY(firstVehicle->compInfoManager()->fileCache().access(cacheTag(firstCrc)).isEmpty());
    QVERIFY(secondVehicle->compInfoManager()->fileCache().access(cacheTag(secondCrc)).isEmpty());

    const QDir tempDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    const QStringList tempFilesBefore = tempDir.entryList({QStringLiteral("metadata-*")}, QDir::Files);

    RequestMetaDataTypeStateMachine firstRequest(firstVehicle->compInfoManager());
    RequestMetaDataTypeStateMachine secondRequest(secondVehicle->compInfoManager());
    QSignalSpy firstComplete(&firstRequest, &RequestMetaDataTypeStateMachine::requestComplete);
    QSignalSpy secondComplete(&secondRequest, &RequestMetaDataTypeStateMachine::requestComplete);
    QSignalSpy firstDownload(firstVehicle->ftpManager(), &FTPManager::downloadComplete);
    QSignalSpy secondDownload(secondVehicle->ftpManager(), &FTPManager::downloadComplete);
    QSignalSpy firstProgress(firstVehicle->ftpManager(), &FTPManager::commandProgress);
    QSignalSpy secondProgress(secondVehicle->ftpManager(), &FTPManager::commandProgress);
    bool overlapped = false;
    const auto recordOverlap = [&] {
        overlapped |= !firstProgress.isEmpty() && !secondProgress.isEmpty() && firstDownload.isEmpty() &&
                      secondDownload.isEmpty();
    };
    connect(firstVehicle->ftpManager(), &FTPManager::commandProgress, &firstRequest, recordOverlap);
    connect(secondVehicle->ftpManager(), &FTPManager::commandProgress, &secondRequest, recordOverlap);
    firstRequest.request(&firstInfo);
    secondRequest.request(&secondInfo);
    QVERIFY_TRUE_WAIT(firstComplete.count() == 1 && secondComplete.count() == 1, TestTimeout::longMs());
    QVERIFY(overlapped);
    if (expectMissingLocale) {
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }
    QCOMPARE(firstDownload.count(), translation ? 2 : 1);
    QCOMPARE(secondDownload.count(), translation ? 2 : 1);
    for (const QSignalSpy* downloads : {&firstDownload, &secondDownload}) {
        for (const auto& completion : *downloads) {
            QVERIFY(completion.at(1).toString().isEmpty());
            QVERIFY(!QFile::exists(completion.at(0).toString()));
        }
    }
    QVERIFY(firstDownload.first().at(1).toString().isEmpty());
    QVERIFY(secondDownload.first().at(1).toString().isEmpty());
    QCOMPARE(firstInfo.bytes, firstBytes);
    QCOMPARE(secondInfo.bytes, secondBytes);
    QVERIFY(firstInfo.fileName != secondInfo.fileName);
    QCOMPARE(firstVehicle->compInfoManager()->fileCache().access(cacheTag(firstCrc)), firstInfo.fileName);
    QCOMPARE(secondVehicle->compInfoManager()->fileCache().access(cacheTag(secondCrc)), secondInfo.fileName);
    const QString firstTemp = firstDownload.first().at(0).toString();
    const QString secondTemp = secondDownload.first().at(0).toString();
    QVERIFY(firstTemp != secondTemp);
    QVERIFY(!QFile::exists(firstTemp));
    QVERIFY(!QFile::exists(secondTemp));
    QFile firstCached(firstInfo.fileName);
    QFile secondCached(secondInfo.fileName);
    QVERIFY(firstCached.open(QIODevice::ReadOnly));
    QVERIFY(secondCached.open(QIODevice::ReadOnly));
    QCOMPARE(firstCached.readAll(), firstBytes);
    QCOMPARE(secondCached.readAll(), secondBytes);
    QCOMPARE(tempDir.entryList({QStringLiteral("metadata-*")}, QDir::Files), tempFilesBefore);
}
