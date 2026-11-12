#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTimer>

class MainThreadStallMonitor : public QObject
{
public:
    explicit MainThreadStallMonitor(QObject* parent);

private:
    QTimer _timer;
    QElapsedTimer _tickTimer;
    QElapsedTimer _logRateTimer;
};
