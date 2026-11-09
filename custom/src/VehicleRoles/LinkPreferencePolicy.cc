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

void LinkPreferencePolicy::setBaseHoldMs(qint64 milliseconds)
{
    if (_timing.baseHoldMs != milliseconds) {
        _timing.baseHoldMs = milliseconds;
        _holdMs = milliseconds;
    }
}

void LinkPreferencePolicy::observe(const Input& input, qint64 now, bool policySwitch, bool primarySignal)
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
        _manualOverride = false;
        _switchPending = false;
        _failoverSeen = _failoverSeen || _preferredUsed ||
                        (!_lastPrimary.isEmpty() && !input.primaryName.isEmpty() && _lastPrimary != input.primaryName);
    } else {
        if (!_wasHealthy) {
            _healthySince = now;
        }
        if (primary) {
            if (_switchPending && _failoverSeen) {
                _lastFailback = now;
            }
            _switchPending = false;
            _manualOverride = false;
            if (_primaryHealthySince < 0) {
                _primaryHealthySince = now;
            }
            if (now - _primaryHealthySince >= _timing.stablePrimaryMs) {
                _holdMs = _timing.baseHoldMs;
                _lastFailback = -1;
            }
        } else {
            _primaryHealthySince = -1;
            if (!policySwitch && !input.primaryName.isEmpty() &&
                (primarySignal || (!_lastPrimary.isEmpty() && _lastPrimary != input.primaryName))) {
                _manualOverride = true;
                _switchPending = false;
            }
        }
    }
    _wasHealthy = healthy;
    _lastPrimary = input.primaryName;
}

QString LinkPreferencePolicy::evaluate(const Input& input, qint64 now)
{
    observe(input, now);
    if (!preferredHealthy(input) || input.primaryName == input.preferredName || _manualOverride || _switchPending ||
        !input.initialConnectDone || input.transferActive) {
        return {};
    }
    const qint64 hold = _failoverSeen ? _holdMs : _timing.initialSettleMs;
    if (_healthySince < 0 || now - _healthySince < hold) {
        return {};
    }
    _switchPending = true;
    return input.preferredName;
}
