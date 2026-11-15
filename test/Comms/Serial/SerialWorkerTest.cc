#include "SerialWorkerTest.h"

#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include <QtCore/QFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QThread>
#include <QtTest/QSignalSpy>

#include "SerialLink.h"

namespace {
QString openPseudoTerminal(QFile& master)
{
    const int descriptor = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        return {};
    }
    if (!master.open(descriptor, QIODevice::ReadWrite | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return {};
    }
    if (::grantpt(descriptor) != 0 || ::unlockpt(descriptor) != 0) {
        return {};
    }
    const char* name = ::ptsname(descriptor);
    return name ? QString::fromLocal8Bit(name) : QString();
}
}  // namespace

void SerialWorkerTest::_occupiedPort_data()
{
    QTest::addColumn<bool>("autoConnect");
    QTest::newRow("automatic-is-quiet") << true;
    QTest::newRow("manual-reports-error") << false;
}

void SerialWorkerTest::_occupiedPort()
{
    QFETCH(bool, autoConnect);
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    QVERIFY(master >= 0);
    const auto closeMaster = qScopeGuard([master] { ::close(master); });
    QCOMPARE(::grantpt(master), 0);
    QCOMPARE(::unlockpt(master), 0);
    const char* name = ::ptsname(master);
    QVERIFY(name);
    const QString portName = QString::fromLocal8Bit(name);
    QSerialPort occupied(portName);
    QVERIFY(occupied.open(QIODevice::ReadWrite));

    SerialConfiguration config(QStringLiteral("Occupied port"));
    config.setPortName(portName);
    config.setAutoConnect(autoConnect);
    SerialWorker worker(config.connectionSettings());
    worker.setupPort();
    QSignalSpy outcomes(&worker, &SerialWorker::openFailed);
    QSignalSpy errors(&worker, &SerialWorker::errorOccurred);
    QSignalSpy disconnected(&worker, &SerialWorker::disconnected);
    if (!autoConnect) {
        expectLogMessage("Comms.Serial.SerialLink", QtWarningMsg, QRegularExpression("Port error:"));
        expectLogMessage("Comms.Serial.SerialLink", QtWarningMsg, QRegularExpression("Opening port.*failed:"));
    }
    worker.connectToPort();
    QCOMPARE(worker.port()->error(), QSerialPort::PermissionError);
    QCOMPARE(outcomes.count(), 1);
    const auto outcome = qvariant_cast<SerialOpenOutcome>(outcomes.first().first());
    QCOMPARE(outcome.stage, SerialOpenOutcome::Stage::Open);
    QCOMPARE(outcome.automatic, autoConnect);
    QCOMPARE(outcome.error, QSerialPort::PermissionError);
    QVERIFY(!outcome.errorString.isEmpty());
    QVERIFY(!worker.isConnected());
    QCOMPARE(disconnected.count(), 1);
    QCOMPARE(errors.count(), autoConnect ? 0 : 1);
    if (!autoConnect) {
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }
}

UT_REGISTER_TEST(SerialWorkerTest, TestLabel::Unit, TestLabel::Comms)

void SerialWorkerTest::_settingsSnapshotAndAvailability()
{
    QFile master;
    const QString name = openPseudoTerminal(master);
    QVERIFY(!name.isEmpty());
    SerialConfiguration config(QStringLiteral("Snapshot"));
    config.setPortName(name);
    config.setBaud(115200);
    SerialWorker worker(config.connectionSettings());
    config.setPortName(QStringLiteral("/test/changed-after-snapshot"));
    config.setBaud(9600);
    QSignalSpy connected(&worker, &SerialWorker::connected);
    QSignalSpy outcomes(&worker, &SerialWorker::openFailed);
    QSignalSpy errors(&worker, &SerialWorker::errorOccurred);
    QSignalSpy disconnected(&worker, &SerialWorker::disconnected);
    worker.connectToPort();
    QCOMPARE(connected.size(), 1);
    QVERIFY(outcomes.isEmpty());
    QVERIFY(errors.isEmpty());
    QCOMPARE(worker.port()->baudRate(), 115200);
    worker.checkPortAvailability({name});
    QVERIFY(worker.isConnected());
    worker.checkPortAvailability({config.portName()});
    QVERIFY(!worker.isConnected());
    QVERIFY(!worker.port()->isOpen());
    QCOMPARE(disconnected.size(), 1);
    worker.checkPortAvailability({});
    worker.disconnectFromPort();
    QCOMPARE(disconnected.size(), 1);
}

void SerialWorkerTest::_configurationFailure_data()
{
    QTest::addColumn<bool>("forceDtrLow");
    QTest::newRow("invalid-baud") << false;
    QTest::newRow("required-dtr-unavailable") << true;
}

void SerialWorkerTest::_configurationFailure()
{
    QFETCH(bool, forceDtrLow);
    QFile master;
    const QString name = openPseudoTerminal(master);
    QVERIFY(!name.isEmpty());
    SerialConnectionSettings settings;
    settings.portName = name;
    settings.dtrForceLow = forceDtrLow;
    settings.baud = forceDtrLow ? 115200 : -1;
    SerialWorker worker(settings);
    QSignalSpy connected(&worker, &SerialWorker::connected);
    QSignalSpy outcomes(&worker, &SerialWorker::openFailed);
    QSignalSpy errors(&worker, &SerialWorker::errorOccurred);
    QSignalSpy disconnected(&worker, &SerialWorker::disconnected);
    worker.connectToPort();
    QVERIFY(connected.isEmpty());
    QCOMPARE(outcomes.count(), 1);
    const auto outcome = qvariant_cast<SerialOpenOutcome>(outcomes.first().first());
    QCOMPARE(outcome.stage, SerialOpenOutcome::Stage::Configuration);
    QVERIFY(outcome.error != QSerialPort::NoError);
    QVERIFY(!outcome.errorString.isEmpty());
    QCOMPARE(errors.size(), 1);
    QCOMPARE(disconnected.size(), 1);
    QVERIFY(!worker.isConnected());
    QVERIFY(!worker.port()->isOpen());
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("Could not configure port")));
    QSerialPort reopened(name);
    QVERIFY(reopened.open(QIODevice::ReadWrite));
}

void SerialWorkerTest::_reservationLifetime()
{
    SerialPortManager ports;
    const QString name = QStringLiteral("/test/worker-claim");
    {
        SerialConnectionSettings settings;
        settings.portName = name;
        auto claim = ports.reservePort(name);
        QVERIFY(claim);
        SerialWorker worker(settings, std::move(claim));
        QVERIFY(!claim);
        QVERIFY(ports.isPortReserved(name));
        QVERIFY(!ports.reservePort(name));
        // No setup or open: early destruction must also release the claim safely.
    }
    QVERIFY(ports.reservePort(name));
}

void SerialWorkerTest::_missingPort_data()
{
    _occupiedPort_data();
}

void SerialWorkerTest::_missingPort()
{
    QFETCH(bool, autoConnect);
    SerialConnectionSettings settings;
    settings.portName = QStringLiteral("/test/qgc-nonexistent-serial-port");
    settings.autoConnect = autoConnect;
    SerialWorker worker(settings);
    QSignalSpy outcomes(&worker, &SerialWorker::openFailed);
    QSignalSpy errors(&worker, &SerialWorker::errorOccurred);
    QSignalSpy disconnected(&worker, &SerialWorker::disconnected);
    if (!autoConnect) {
        expectLogMessage("Comms.Serial.SerialLink", QtWarningMsg, QRegularExpression("Port error:"));
        expectLogMessage("Comms.Serial.SerialLink", QtWarningMsg, QRegularExpression("Opening port.*failed:"));
    }
    worker.connectToPort();
    QVERIFY(!worker.isConnected());
    QCOMPARE(outcomes.count(), 1);
    const auto outcome = qvariant_cast<SerialOpenOutcome>(outcomes.first().first());
    QCOMPARE(outcome.stage, SerialOpenOutcome::Stage::Open);
    QCOMPARE(outcome.automatic, autoConnect);
    QCOMPARE(outcome.error, QSerialPort::DeviceNotFoundError);
    QVERIFY(!outcome.errorString.isEmpty());
    QCOMPARE(errors.size(), autoConnect ? 0 : 1);
    QCOMPARE(disconnected.size(), 1);
    if (!autoConnect) {
        verifyExpectedLogMessage();
        verifyExpectedLogMessage();
    }
}

void SerialWorkerTest::_queuedOpenOutcome_data()
{
    QTest::addColumn<bool>("occupied");
    QTest::newRow("access-denied") << true;
    QTest::newRow("device-not-found") << false;
}

void SerialWorkerTest::_queuedOpenOutcome()
{
    QFETCH(bool, occupied);
    QFile master;
    const QString portName = openPseudoTerminal(master);
    QVERIFY(!portName.isEmpty());
    QSerialPort blocker(portName);
    if (occupied) {
        QVERIFY(blocker.open(QIODevice::ReadWrite));
    }
    SerialConnectionSettings settings;
    settings.portName = occupied ? portName : QStringLiteral("/test/queued-absent-port");
    settings.autoConnect = true;
    QThread workerThread;
    auto* worker = new SerialWorker(settings);
    worker->moveToThread(&workerThread);
    (void) connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);
    QObject receiver;
    SerialOpenOutcome received;
    int receivedCount = 0;
    bool receivedOnApplicationThread = false;
    std::atomic<bool> emittedOnWorkerThread{false};
    (void) connect(
        worker, &SerialWorker::openFailed, worker,
        [&emittedOnWorkerThread, &workerThread](const SerialOpenOutcome&) {
            emittedOnWorkerThread.store(QThread::currentThread() == &workerThread);
        },
        Qt::DirectConnection);
    (void) connect(
        worker, &SerialWorker::openFailed, &receiver,
        [&](const SerialOpenOutcome& outcome) {
            received = outcome;
            ++receivedCount;
            receivedOnApplicationThread = QThread::currentThread() == receiver.thread();
        },
        Qt::QueuedConnection);
    (void) connect(&workerThread, &QThread::started, worker, &SerialWorker::connectToPort);
    workerThread.start();
    const auto stopWorker = qScopeGuard([&] {
        workerThread.quit();
        workerThread.wait();
    });
    QTRY_COMPARE_WITH_TIMEOUT(receivedCount, 1, TestTimeout::shortMs());
    QVERIFY(emittedOnWorkerThread.load());
    QVERIFY(receivedOnApplicationThread);
    QCOMPARE(received.stage, SerialOpenOutcome::Stage::Open);
    QVERIFY(received.automatic);
    QCOMPARE(received.error, occupied ? QSerialPort::PermissionError : QSerialPort::DeviceNotFoundError);
    QVERIFY(!received.errorString.isEmpty());
    // Queue a marker behind the complete open attempt, then inspect only application-thread state.
    bool drained = false;
    QVERIFY(QMetaObject::invokeMethod(
        worker,
        [&receiver, &drained] {
            (void) QMetaObject::invokeMethod(&receiver, [&drained] { drained = true; }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(drained, TestTimeout::shortMs());
    QCOMPARE(receivedCount, 1);
}

void SerialWorkerTest::_bootloaderOutcome()
{
    SerialConnectionSettings settings;
    settings.portName = QStringLiteral("/test/bootloader");
    settings.autoConnect = true;
    settings.bootloader = true;
    SerialWorker worker(settings);
    QSignalSpy outcomes(&worker, &SerialWorker::openFailed);
    QSignalSpy errors(&worker, &SerialWorker::errorOccurred);
    QSignalSpy disconnected(&worker, &SerialWorker::disconnected);
    expectLogMessage("Comms.Serial.SerialLink", QtWarningMsg, QRegularExpression("Not connecting to bootloader"));
    worker.connectToPort();
    verifyExpectedLogMessage();
    QCOMPARE(outcomes.count(), 1);
    const auto outcome = qvariant_cast<SerialOpenOutcome>(outcomes.first().first());
    QCOMPARE(outcome.stage, SerialOpenOutcome::Stage::Bootloader);
    QCOMPARE(outcome.error, QSerialPort::NoError);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(disconnected.count(), 1);
    QVERIFY(!worker.isConnected());
}
