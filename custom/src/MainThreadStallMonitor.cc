#include "MainThreadStallMonitor.h"

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(MainThreadStallLog, "Custom.MainThreadStall")

MainThreadStallMonitor::MainThreadStallMonitor(QObject* parent)
    : QObject(parent)
    , _timer(this)
{
    _timer.setInterval(100);
    connect(&_timer, &QTimer::timeout, this, [this]() {
        const qint64 lateMs = _tickTimer.restart() - _timer.interval();
        if (lateMs > 250 && (!_logRateTimer.isValid() || _logRateTimer.elapsed() >= 1000)) {
            qCInfo(MainThreadStallLog) << "Main thread stalled for" << lateMs << "ms";
            _logRateTimer.start();
        }
    });
    _tickTimer.start();
    _timer.start();
}
