#pragma once

#include <QtCore/QStringList>

/// One vehicle's preference history. All times are monotonic milliseconds supplied by the caller.
class LinkPreferencePolicy
{
public:
    // Outlast stock's 3500 ms heartbeat timeout plus its 1000 ms comm-loss polling interval.
    static constexpr qint64 kInitialSettleMs = 6000;

    struct Timing
    {
        qint64 initialSettleMs = kInitialSettleMs;
        qint64 baseHoldMs = 15000;
        qint64 flapWindowMs = 60000;
        qint64 stablePrimaryMs = 300000;
        qint64 maxHoldMs = 300000;
    };

    struct Input
    {
        QString preferredName;
        QStringList linkNames;
        QStringList linkStatuses;
        QString primaryName;
        bool initialConnectDone = false;
        bool transferActive = false;
    };

    LinkPreferencePolicy();
    explicit LinkPreferencePolicy(const Timing& timing);

    /// Empty means no switch. A request stays pending until observed as primary, lost, or cancelled.
    QString evaluate(const Input& input, qint64 now);
    /// Observe immediately; primarySignal also detects a manual re-pick of the current primary.
    void observe(const Input& input, qint64 now, bool policySwitch = false, bool primarySignal = false);

    void cancelPendingSwitch() { _switchPending = false; }

    void setBaseHoldMs(qint64 milliseconds);

    static bool preferredHealthy(const Input& input);

private:
    Timing _timing;
    QString _preferredName;
    QString _lastPrimary;
    qint64 _healthySince = -1;
    qint64 _primaryHealthySince = -1;
    qint64 _lastFailback = -1;
    qint64 _holdMs = 15000;
    bool _wasHealthy = false;
    bool _failoverSeen = false;
    bool _preferredUsed = false;
    bool _manualOverride = false;
    bool _switchPending = false;
};
