#include "LinkPreferencePolicy.h"

#include <algorithm>

LinkPreferencePolicy::LinkPreferencePolicy()
    : LinkPreferencePolicy(Timing{})
{}

LinkPreferencePolicy::LinkPreferencePolicy(const Timing& timing)
    : _timing(timing)
    , _holdMs(timing.baseHoldMs)
{}

bool LinkPreferencePolicy::preferredHealthy(const Input& input)
{
    const auto index = input.linkNames.indexOf(input.preferredName);
    return !input.preferredName.isEmpty() && index >= 0 && index < input.linkStatuses.size() &&
           input.linkNames.count(input.preferredName) == 1 && input.linkStatuses[index].isEmpty();
}

bool LinkPreferencePolicy::transferActive(const TransferFlags& flags)
{
    // Readiness can remain false after an armed/skipped or failed initial download.
    return flags.pendingWrites || flags.loadProgressActive || flags.missionBusy || flags.fenceBusy || flags.rallyBusy ||
           flags.signingPending;
}

QString LinkPreferencePolicy::debugState() const
{
    return QStringLiteral("failoverSeen=%1 effectiveHoldMs=%2")
        .arg(_failoverSeen)
        .arg(_failoverSeen ? _holdMs : _timing.initialSettleMs);
}

void LinkPreferencePolicy::setBaseHoldMs(qint64 milliseconds)
{
    if (_timing.baseHoldMs != milliseconds) {
        _timing.baseHoldMs = milliseconds;
        _holdMs = milliseconds;
    }
}

void LinkPreferencePolicy::observe(const Input& input, qint64 now)
{
    // A detached link keeps its history; only a preference change resets it.
    if (input.preferredName.isEmpty()) {
        *this = LinkPreferencePolicy(_timing);
        return;
    }
    if (_preferredName != input.preferredName) {
        *this = LinkPreferencePolicy(_timing);
        _preferredName = input.preferredName;
    }

    const bool healthy = preferredHealthy(input);
    const bool primary = input.primaryName == _preferredName;
    // Core clears the primary before removing its name from the link list.
    _preferredUsed = _preferredUsed || primary;
    if (!healthy) {
        if (_wasHealthy && _lastFailback >= 0 && now - _lastFailback < _timing.flapWindowMs) {
            _holdMs = std::min(_holdMs * 2, _timing.maxHoldMs);
        }
        _lastFailback = -1;
        _healthySince = -1;
        _primaryHealthySince = -1;
        _switchPending = false;
        _failoverSeen = _failoverSeen || _preferredUsed ||
                        (!_lastPrimary.isEmpty() && !input.primaryName.isEmpty() && _lastPrimary != input.primaryName);
    } else {
        // Leaving a healthy preferred primary starts a full return wait, just like failover.
        const bool leftPreferred = _lastPrimary == _preferredName && !primary && !input.primaryName.isEmpty();
        if (leftPreferred) {
            _failoverSeen = true;
        }
        if (!_wasHealthy || leftPreferred) {
            _healthySince = now;
        }
        if (primary) {
            if (_switchPending && _failoverSeen) {
                _lastFailback = now;
            }
            _switchPending = false;
            if (_primaryHealthySince < 0) {
                _primaryHealthySince = now;
            }
            if (now - _primaryHealthySince >= _timing.stablePrimaryMs) {
                _holdMs = _timing.baseHoldMs;
                _lastFailback = -1;
            }
        } else {
            _primaryHealthySince = -1;
        }
    }
    _wasHealthy = healthy;
    _lastPrimary = input.primaryName;
}

LinkPreferencePolicy::Status LinkPreferencePolicy::status(const Input& input, qint64 now) const
{
    if (input.preferredName.isEmpty()) {
        return {Gate::Off};
    }
    if (!input.linkNames.contains(input.preferredName)) {
        return {Gate::NotAttached};
    }
    if (input.primaryName == input.preferredName) {
        return {Gate::OnPreferred};
    }
    if (!preferredHealthy(input)) {
        return {Gate::PreferredDown};
    }
    const qint64 hold = _failoverSeen ? _holdMs : _timing.initialSettleMs;
    const qint64 remaining = _healthySince < 0 ? hold : hold - (now - _healthySince);
    if (remaining > 0) {
        return {Gate::Waiting, remaining, _failoverSeen && _holdMs > _timing.baseHoldMs};
    }
    if (!input.initialConnectDone) {
        return {Gate::WaitingForConnect};
    }
    if (input.transferActive) {
        return {Gate::PausedForTransfer};
    }
    if (_switchPending) {
        return {Gate::Switching};
    }
    return {Gate::Ready};
}

QString LinkPreferencePolicy::evaluate(const Input& input, qint64 now)
{
    observe(input, now);
    if (status(input, now).gate != Gate::Ready) {
        return {};
    }
    _switchPending = true;
    return input.preferredName;
}
