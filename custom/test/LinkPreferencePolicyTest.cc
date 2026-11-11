#include "LinkPreferencePolicyTest.h"

#include "LinkPreferencePolicy.h"

namespace {
LinkPreferencePolicy::Timing timing()
{
    return {.initialSettleMs = 3, .baseHoldMs = 15, .flapWindowMs = 60, .stablePrimaryMs = 300, .maxHoldMs = 300};
}

LinkPreferencePolicy::Input input()
{
    return {.preferredName = QStringLiteral("UDP"),
            .linkNames = {QStringLiteral("RFD"), QStringLiteral("UDP")},
            .linkStatuses = {QString(), QString()},
            .primaryName = QStringLiteral("RFD"),
            .initialConnectDone = true};
}
}  // namespace

void LinkPreferencePolicyTest::_initialSettleOutlastsLossDetection()
{
    QCOMPARE(LinkPreferencePolicy::Timing{}.baseHoldMs, qint64(60000));
    auto state = input();
    LinkPreferencePolicy policy;
    policy.observe(state, 0);
    QVERIFY(policy.evaluate(state, 4500).isEmpty());
    QVERIFY(policy.evaluate(state, 5999).isEmpty());
    QCOMPARE(policy.evaluate(state, 6000), state.preferredName);

    // A single packet followed by silence must reach stock loss detection before selection.
    LinkPreferencePolicy silentLink;
    silentLink.observe(state, 0);
    state.linkStatuses[1] = QStringLiteral("lost");
    silentLink.observe(state, 4500);
    QVERIFY(silentLink.evaluate(state, 6000).isEmpty());
}

void LinkPreferencePolicyTest::_transferActive_data()
{
    QTest::addColumn<bool>("parametersReady");
    QTest::addColumn<bool>("pendingWrites");
    QTest::addColumn<bool>("loadProgressActive");
    QTest::addColumn<bool>("missionBusy");
    QTest::addColumn<bool>("fenceBusy");
    QTest::addColumn<bool>("rallyBusy");
    QTest::addColumn<bool>("signingPending");
    QTest::addColumn<bool>("active");
    QTest::newRow("armed-or-failed-download") << false << false << false << false << false << false << false << false;
    QTest::newRow("ready-idle") << true << false << false << false << false << false << false << false;
    QTest::newRow("parameter-write") << false << true << false << false << false << false << false << true;
    QTest::newRow("parameter-load") << false << false << true << false << false << false << false << true;
    QTest::newRow("ready-but-loading") << true << false << true << false << false << false << false << true;
    QTest::newRow("mission") << false << false << false << true << false << false << false << true;
    QTest::newRow("fence") << false << false << false << false << true << false << false << true;
    QTest::newRow("rally") << false << false << false << false << false << true << false << true;
    QTest::newRow("signing") << false << false << false << false << false << false << true << true;
}

void LinkPreferencePolicyTest::_transferActive()
{
    QFETCH(bool, parametersReady);
    QFETCH(bool, pendingWrites);
    QFETCH(bool, loadProgressActive);
    QFETCH(bool, missionBusy);
    QFETCH(bool, fenceBusy);
    QFETCH(bool, rallyBusy);
    QFETCH(bool, signingPending);
    QFETCH(bool, active);
    const LinkPreferencePolicy::TransferFlags flags{.parametersReady = parametersReady,
                                                    .pendingWrites = pendingWrites,
                                                    .loadProgressActive = loadProgressActive,
                                                    .missionBusy = missionBusy,
                                                    .fenceBusy = fenceBusy,
                                                    .rallyBusy = rallyBusy,
                                                    .signingPending = signingPending};
    QCOMPARE(LinkPreferencePolicy::transferActive(flags), active);

    auto state = input();
    state.transferActive = LinkPreferencePolicy::transferActive(flags);
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    QCOMPARE(policy.status(state, 3).gate,
             active ? LinkPreferencePolicy::Gate::PausedForTransfer : LinkPreferencePolicy::Gate::Ready);
    QCOMPARE(policy.evaluate(state, 3), active ? QString() : state.preferredName);
}

void LinkPreferencePolicyTest::_debugState()
{
    auto state = input();
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    QCOMPARE(policy.debugState(), QStringLiteral("failoverSeen=0 effectiveHoldMs=3"));
    state.primaryName = state.preferredName;
    policy.observe(state, 2);
    QCOMPARE(policy.debugState(), QStringLiteral("failoverSeen=0 effectiveHoldMs=3"));
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 3);
    QCOMPARE(policy.debugState(), QStringLiteral("failoverSeen=1 effectiveHoldMs=15"));
    policy.setBaseHoldMs(42);
    QCOMPARE(policy.debugState(), QStringLiteral("failoverSeen=1 effectiveHoldMs=42"));
}

void LinkPreferencePolicyTest::_statusAndDecision_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<int>("expectedGate");
    QTest::addColumn<qint64>("now");
    QTest::addColumn<qint64>("remainingMs");
    const auto row = [](const char* name, LinkPreferencePolicy::Gate gate, qint64 now = 6000, qint64 remainingMs = 0) {
        QTest::newRow(name) << QString::fromLatin1(name) << int(gate) << now << remainingMs;
    };
    row("off", LinkPreferencePolicy::Gate::Off);
    row("not-attached", LinkPreferencePolicy::Gate::NotAttached);
    row("on-preferred", LinkPreferencePolicy::Gate::OnPreferred);
    row("preferred-down", LinkPreferencePolicy::Gate::PreferredDown);
    row("initial-hold-minus-one", LinkPreferencePolicy::Gate::Waiting, 5999, 1);
    row("initial-hold", LinkPreferencePolicy::Gate::Ready);
    row("return-hold-minus-one", LinkPreferencePolicy::Gate::Waiting, 60001, 1);
    row("return-hold", LinkPreferencePolicy::Gate::Ready, 60002);
    row("waiting-before-connect", LinkPreferencePolicy::Gate::Waiting, 5999, 1);
    row("waiting-during-transfer", LinkPreferencePolicy::Gate::Waiting, 5999, 1);
    row("waiting-for-connect", LinkPreferencePolicy::Gate::WaitingForConnect);
    row("paused-for-transfer", LinkPreferencePolicy::Gate::PausedForTransfer);
    row("switching", LinkPreferencePolicy::Gate::Switching, 6001);
    row("transfer-before-switch", LinkPreferencePolicy::Gate::PausedForTransfer, 6001);
    row("connect-before-switch", LinkPreferencePolicy::Gate::WaitingForConnect, 6001);
}

void LinkPreferencePolicyTest::_statusAndDecision()
{
    QFETCH(QString, scenario);
    QFETCH(int, expectedGate);
    QFETCH(qint64, now);
    QFETCH(qint64, remainingMs);
    auto state = input();
    LinkPreferencePolicy policy;
    if (scenario.startsWith(QStringLiteral("return-hold"))) {
        state.primaryName = state.preferredName;
        policy.observe(state, 0);
        state.linkStatuses[1] = QStringLiteral("lost");
        policy.observe(state, 1);
        state.primaryName = QStringLiteral("RFD");
        policy.observe(state, 1);
        state.linkStatuses[1].clear();
        policy.observe(state, 2);
    } else {
        policy.observe(state, 0);
    }
    if (scenario == QStringLiteral("off")) {
        state.preferredName.clear();
    } else if (scenario == QStringLiteral("not-attached")) {
        state.linkNames.removeLast();
        state.linkStatuses.removeLast();
    } else if (scenario == QStringLiteral("on-preferred")) {
        state.primaryName = state.preferredName;
    } else if (scenario == QStringLiteral("preferred-down")) {
        state.linkStatuses[1] = QStringLiteral("lost");
    } else if (scenario == QStringLiteral("waiting-for-connect") ||
               scenario == QStringLiteral("waiting-before-connect")) {
        state.initialConnectDone = false;
    } else if (scenario == QStringLiteral("paused-for-transfer") ||
               scenario == QStringLiteral("waiting-during-transfer")) {
        state.transferActive = true;
    } else if (scenario == QStringLiteral("switching") || scenario.endsWith(QStringLiteral("before-switch"))) {
        QCOMPARE(policy.evaluate(state, 6000), state.preferredName);
        state.transferActive = scenario == QStringLiteral("transfer-before-switch");
        state.initialConnectDone = scenario != QStringLiteral("connect-before-switch");
    }
    policy.observe(state, now);
    const auto& readOnlyPolicy = policy;
    const auto status = readOnlyPolicy.status(state, now);
    QCOMPARE(int(status.gate), expectedGate);
    QCOMPARE(status.remainingMs, remainingMs);
    QVERIFY(!status.extendedHold);
    QCOMPARE(policy.evaluate(state, now),
             status.gate == LinkPreferencePolicy::Gate::Ready ? state.preferredName : QString());
    if (status.gate == LinkPreferencePolicy::Gate::Ready) {
        QCOMPARE(policy.status(state, now).gate, LinkPreferencePolicy::Gate::Switching);
        QVERIFY(policy.evaluate(state, now).isEmpty());
    }
}

void LinkPreferencePolicyTest::_healthyPreferredDeparture_data()
{
    QTest::addColumn<bool>("extended");
    QTest::newRow("default-wait") << false;
    QTest::newRow("escalated-wait") << true;
}

void LinkPreferencePolicyTest::_healthyPreferredDeparture()
{
    QFETCH(bool, extended);
    auto state = input();
    state.primaryName = state.preferredName;
    LinkPreferencePolicy policy;
    policy.observe(state, 0);
    qint64 lastReturn = 0;
    if (extended) {
        qint64 now = 0;
        for (qint64 hold : {60000, 120000}) {
            state.linkStatuses[1] = QStringLiteral("lost");
            policy.observe(state, ++now);
            state.primaryName = QStringLiteral("RFD");
            policy.observe(state, now);
            state.linkStatuses[1].clear();
            policy.observe(state, ++now);
            now += hold;
            QCOMPARE(policy.evaluate(state, now), state.preferredName);
            state.primaryName = state.preferredName;
            policy.observe(state, now);
        }
        lastReturn = now;
    }
    const qint64 departure = lastReturn + 10000;
    policy.observe(state, departure - 1);
    state.primaryName = QStringLiteral("RFD");
    QVERIFY(policy.evaluate(state, departure).isEmpty());
    const qint64 hold = extended ? 120000 : 60000;
    const auto waiting = policy.status(state, departure);
    QCOMPARE(waiting.gate, LinkPreferencePolicy::Gate::Waiting);
    QCOMPARE(waiting.remainingMs, hold);
    QCOMPARE(waiting.extendedHold, extended);
    // Health predates the departure; it cannot count towards this return.
    QVERIFY(policy.evaluate(state, departure + hold - 1).isEmpty());
    QCOMPARE(policy.status(state, departure + hold - 1).remainingMs, qint64(1));
    QCOMPARE(policy.evaluate(state, departure + hold), state.preferredName);
}

void LinkPreferencePolicyTest::_secondaryChoicesKeepRecoveryDeadline_data()
{
    QTest::addColumn<QString>("choice");
    for (const char* name : {"stock-failover", "same-during-outage", "third-during-outage", "same-after-recovery",
                             "third-after-recovery"}) {
        QTest::newRow(name) << QString::fromLatin1(name);
    }
}

void LinkPreferencePolicyTest::_secondaryChoicesKeepRecoveryDeadline()
{
    QFETCH(QString, choice);
    auto state = input();
    state.linkNames.append(QStringLiteral("Backup B"));
    state.linkStatuses.append(QString());
    state.primaryName = state.preferredName;
    LinkPreferencePolicy policy;
    policy.observe(state, 0);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 1);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 2);
    if (choice.endsWith(QStringLiteral("during-outage"))) {
        if (choice.startsWith(QStringLiteral("third"))) {
            state.primaryName = QStringLiteral("Backup B");
        }
        policy.observe(state, 10000);
    }
    QVERIFY(policy.evaluate(state, 20000).isEmpty());
    state.linkStatuses[1].clear();
    constexpr qint64 recovery = 30000;
    policy.observe(state, recovery);
    QCOMPARE(policy.status(state, recovery).gate, LinkPreferencePolicy::Gate::Waiting);
    QCOMPARE(policy.status(state, recovery).remainingMs, qint64(60000));
    if (choice.endsWith(QStringLiteral("after-recovery"))) {
        if (choice.startsWith(QStringLiteral("third"))) {
            state.primaryName = QStringLiteral("Backup B");
        }
        QVERIFY(policy.evaluate(state, recovery + 10000).isEmpty());
        QCOMPARE(policy.status(state, recovery + 10000).remainingMs, qint64(50000));
    }
    QVERIFY(policy.evaluate(state, recovery + 59999).isEmpty());
    QCOMPARE(policy.evaluate(state, recovery + 60000), state.preferredName);
}

void LinkPreferencePolicyTest::_selectionGuards_data()
{
    QTest::addColumn<QString>("guard");
    for (const char* name : {"none", "unattached", "renamed", "lost", "missing-status", "duplicate-name", "primary",
                             "initial-connect", "transfer"}) {
        QTest::newRow(name) << QString::fromLatin1(name);
    }
}

void LinkPreferencePolicyTest::_selectionGuards()
{
    QFETCH(QString, guard);
    auto state = input();
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    if (guard == QStringLiteral("none")) {
        state.preferredName.clear();
    } else if (guard == QStringLiteral("unattached")) {
        state.preferredName = QStringLiteral("Other vehicle UDP");
    } else if (guard == QStringLiteral("renamed")) {
        state.linkNames[1] = QStringLiteral("Renamed UDP");
    } else if (guard == QStringLiteral("lost")) {
        state.linkStatuses[1] = QStringLiteral("nonempty status");
    } else if (guard == QStringLiteral("missing-status")) {
        state.linkStatuses.removeLast();
    } else if (guard == QStringLiteral("duplicate-name")) {
        state.linkNames.append(state.preferredName);
        state.linkStatuses.append(QString());
    } else if (guard == QStringLiteral("primary")) {
        state.primaryName = state.preferredName;
    } else if (guard == QStringLiteral("initial-connect")) {
        state.initialConnectDone = false;
    } else if (guard == QStringLiteral("transfer")) {
        state.transferActive = true;
    }
    QVERIFY(policy.evaluate(state, 0).isEmpty());
    QVERIFY(policy.evaluate(state, 1).isEmpty());
    if (guard == QStringLiteral("initial-connect") || guard == QStringLiteral("transfer")) {
        state.initialConnectDone = true;
        state.transferActive = false;
        QVERIFY(policy.evaluate(state, 2).isEmpty());
        QCOMPARE(policy.evaluate(state, 3), state.preferredName);
    } else {
        QVERIFY(policy.evaluate(state, 1000).isEmpty());
        if (guard == QStringLiteral("unattached") || guard == QStringLiteral("renamed")) {
            state.linkNames[1] = state.preferredName;
            QVERIFY(policy.evaluate(state, 1001).isEmpty());
            QVERIFY(policy.evaluate(state, 1003).isEmpty());
            QCOMPARE(policy.evaluate(state, 1004), state.preferredName);
        }
    }
}

void LinkPreferencePolicyTest::_continuousHealthAndPendingSwitch()
{
    auto state = input();
    LinkPreferencePolicy policy;
    QVERIFY(policy.evaluate(state, 0).isEmpty());
    QVERIFY(policy.evaluate(state, LinkPreferencePolicy::kInitialSettleMs - 1).isEmpty());
    QCOMPARE(policy.evaluate(state, LinkPreferencePolicy::kInitialSettleMs), state.preferredName);
    QVERIFY(policy.evaluate(state, 10000).isEmpty());

    state.primaryName = state.preferredName;
    policy.observe(state, 10000);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 10001);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 10002);
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 11000).isEmpty());
    QVERIFY(policy.evaluate(state, 70999).isEmpty());
    state.linkStatuses[1] = QStringLiteral("lost");
    QVERIFY(policy.evaluate(state, 71000).isEmpty());
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 72000).isEmpty());
    QVERIFY(policy.evaluate(state, 131999).isEmpty());
    QCOMPARE(policy.evaluate(state, 132000), state.preferredName);
    QVERIFY(policy.evaluate(state, 140000).isEmpty());
}

void LinkPreferencePolicyTest::_secondaryLossBeforeFirstSelection()
{
    auto state = input();
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 2);
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 100).isEmpty());
    QVERIFY(policy.evaluate(state, 102).isEmpty());
    QCOMPARE(policy.evaluate(state, 103), state.preferredName);
    state.linkStatuses[1] = QStringLiteral("lost");
    QVERIFY(policy.evaluate(state, 104).isEmpty());
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 105).isEmpty());
    QCOMPARE(policy.evaluate(state, 108), state.preferredName);
}

void LinkPreferencePolicyTest::_flapWindow_data()
{
    QTest::addColumn<qint64>("dropAfter");
    QTest::addColumn<qint64>("hold");
    QTest::newRow("within-window") << qint64(59) << qint64(30);
    QTest::newRow("at-window-boundary") << qint64(60) << qint64(15);
}

void LinkPreferencePolicyTest::_flapWindow()
{
    QFETCH(qint64, dropAfter);
    QFETCH(qint64, hold);
    auto state = input();
    state.primaryName = state.preferredName;
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 1);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 1);
    state.linkStatuses[1].clear();
    policy.observe(state, 2);
    QCOMPARE(policy.evaluate(state, 17), state.preferredName);
    state.primaryName = state.preferredName;
    policy.observe(state, 17);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 17 + dropAfter);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 17 + dropAfter);
    state.linkStatuses[1].clear();
    const auto recovery = 18 + dropAfter;
    policy.observe(state, recovery);
    const auto waiting = policy.status(state, recovery);
    QCOMPARE(waiting.gate, LinkPreferencePolicy::Gate::Waiting);
    QCOMPARE(waiting.extendedHold, hold > 15);
    QVERIFY(policy.evaluate(state, recovery + hold - 1).isEmpty());
    QCOMPARE(policy.evaluate(state, recovery + hold), state.preferredName);
    QVERIFY(!policy.status(state, recovery + hold).extendedHold);
}

void LinkPreferencePolicyTest::_secondaryChoicesKeepWait_data()
{
    QTest::addColumn<QString>("previousStatus");
    QTest::addColumn<bool>("preferredLost");
    for (const char* name : {"same-secondary", "healthy", "lost", "removed", "missing-status", "empty-primary"}) {
        for (bool preferredLost : {false, true}) {
            const auto row =
                QStringLiteral("%1-%2").arg(QString::fromLatin1(name)).arg(preferredLost ? "outage" : "healthy");
            QTest::newRow(qPrintable(row)) << QString::fromLatin1(name) << preferredLost;
        }
    }
}

void LinkPreferencePolicyTest::_secondaryChoicesKeepWait()
{
    QFETCH(QString, previousStatus);
    QFETCH(bool, preferredLost);
    auto state = input();
    state.linkNames.append(QStringLiteral("Backup B"));
    state.linkStatuses.append(QString());
    if (preferredLost) {
        state.linkStatuses[1] = QStringLiteral("lost");
    }
    if (previousStatus == QStringLiteral("empty-primary")) {
        state.primaryName.clear();
    }
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    if (previousStatus == QStringLiteral("lost")) {
        state.linkStatuses[0] = QStringLiteral("lost");
        policy.observe(state, 1);
    } else if (previousStatus == QStringLiteral("removed")) {
        state.linkNames.removeFirst();
        state.linkStatuses.removeFirst();
    } else if (previousStatus == QStringLiteral("missing-status")) {
        state.linkNames.move(0, 2);
        state.linkStatuses.removeFirst();
    }
    if (previousStatus != QStringLiteral("same-secondary")) {
        state.primaryName = QStringLiteral("Backup B");
    }
    QCOMPARE(LinkPreferencePolicy::preferredHealthy(state), !preferredLost);
    policy.observe(state, 2);
    state.linkStatuses[state.linkNames.indexOf(state.preferredName)].clear();
    if (preferredLost) {
        policy.observe(state, 3);
    }
    const bool failover = preferredLost && previousStatus != QStringLiteral("empty-primary") &&
                          previousStatus != QStringLiteral("same-secondary");
    const qint64 deadline = preferredLost ? 3 + (failover ? 15 : 3) : 3;
    QVERIFY(policy.evaluate(state, deadline - 1).isEmpty());
    QCOMPARE(policy.evaluate(state, deadline), state.preferredName);
    QVERIFY(policy.evaluate(state, deadline + 1).isEmpty());
}

void LinkPreferencePolicyTest::_deadlineGuards_data()
{
    QTest::addColumn<bool>("lost");
    QTest::newRow("loss-at-deadline") << true;
    QTest::newRow("transfer-at-deadline") << false;
}

void LinkPreferencePolicyTest::_deadlineGuards()
{
    QFETCH(bool, lost);
    auto state = input();
    LinkPreferencePolicy policy(timing());
    QVERIFY(policy.evaluate(state, 0).isEmpty());
    QVERIFY(policy.evaluate(state, 2).isEmpty());
    if (lost) {
        state.linkStatuses[1] = QStringLiteral("lost");
    } else {
        state.transferActive = true;
    }
    QVERIFY(policy.evaluate(state, 3).isEmpty());
    state.linkStatuses[1].clear();
    state.transferActive = false;
    if (lost) {
        QVERIFY(policy.evaluate(state, 4).isEmpty());
        QVERIFY(policy.evaluate(state, 6).isEmpty());
        QCOMPARE(policy.evaluate(state, 7), state.preferredName);
    } else {
        QCOMPARE(policy.evaluate(state, 4), state.preferredName);
    }
}

void LinkPreferencePolicyTest::_flappingBackoffAndStableReset()
{
    auto state = input();
    state.primaryName = state.preferredName;
    LinkPreferencePolicy policy;
    qint64 now = 0;
    policy.observe(state, now);
    QVERIFY(!policy.status(state, now).extendedHold);
    for (qint64 hold : {60000, 120000, 240000, 300000, 300000}) {
        state.linkStatuses[1] = QStringLiteral("lost");
        policy.observe(state, ++now);
        QVERIFY(!policy.status(state, now).extendedHold);
        state.primaryName = QStringLiteral("RFD");
        policy.observe(state, now);
        state.linkStatuses[1].clear();
        policy.observe(state, ++now);
        QCOMPARE(policy.status(state, now).remainingMs, hold);
        QCOMPARE(policy.status(state, now).extendedHold, hold > 60000);
        QVERIFY(policy.debugState().contains(QStringLiteral("effectiveHoldMs=%1").arg(hold)));
        QVERIFY(policy.evaluate(state, now + hold - 1).isEmpty());
        now += hold;
        QCOMPARE(policy.evaluate(state, now), state.preferredName);
        QVERIFY(!policy.status(state, now).extendedHold);
        state.primaryName = state.preferredName;
        policy.observe(state, now);
    }
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, ++now);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, now);
    state.linkStatuses[1].clear();
    policy.observe(state, ++now);
    now += 300000;
    policy.observe(state, now);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, ++now);
    state.linkStatuses[1].clear();
    policy.observe(state, ++now);
    QVERIFY(policy.evaluate(state, now + 299999).isEmpty());
    now += 300000;
    QCOMPARE(policy.evaluate(state, now), state.preferredName);
    state.primaryName = state.preferredName;
    policy.observe(state, now);
    now += 300000;
    policy.observe(state, now);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, ++now);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, now);
    state.linkStatuses[1].clear();
    policy.observe(state, ++now);
    QCOMPARE(policy.status(state, now).remainingMs, qint64(60000));
    QVERIFY(!policy.status(state, now).extendedHold);
    QVERIFY(policy.evaluate(state, now + 59999).isEmpty());
    QCOMPARE(policy.evaluate(state, now + 60000), state.preferredName);
}

void LinkPreferencePolicyTest::_preferenceChangeResetsHistory()
{
    auto state = input();
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    QCOMPARE(policy.evaluate(state, 3), state.preferredName);
    state.linkNames.removeLast();
    QVERIFY(policy.evaluate(state, 4).isEmpty());
    state.linkNames.append(state.preferredName);
    QVERIFY(policy.evaluate(state, 5).isEmpty());
    QVERIFY(policy.evaluate(state, 7).isEmpty());
    QCOMPARE(policy.evaluate(state, 8), state.preferredName);
    state.preferredName.clear();
    QVERIFY(policy.evaluate(state, 9).isEmpty());
    state.preferredName = QStringLiteral("UDP");
    QVERIFY(policy.evaluate(state, 10).isEmpty());
    QCOMPARE(policy.evaluate(state, 13), state.preferredName);
    state.preferredName = QStringLiteral("RFD");
    state.primaryName = QStringLiteral("UDP");
    QVERIFY(policy.evaluate(state, 14).isEmpty());
    QCOMPARE(policy.evaluate(state, 17), state.preferredName);
}

void LinkPreferencePolicyTest::_physicalDisconnectRetainsBackoff()
{
    auto state = input();
    state.primaryName = state.preferredName;
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    const auto disconnectPreferred = [&](qint64 now) {
        // _removeLink signals the cleared primary and the old lists before erasing the link.
        state.primaryName.clear();
        policy.observe(state, now);
        policy.observe(state, now);
        state.linkNames.removeLast();
        state.linkStatuses.removeLast();
        state.primaryName = QStringLiteral("RFD");
        policy.observe(state, now);
    };
    const auto reconnectPreferred = [&](qint64 now) {
        state.linkNames.append(state.preferredName);
        state.linkStatuses.append(QString());
        policy.observe(state, now);
    };
    disconnectPreferred(1);
    reconnectPreferred(2);
    QVERIFY(policy.evaluate(state, 16).isEmpty());
    QCOMPARE(policy.evaluate(state, 17), state.preferredName);
    state.primaryName = state.preferredName;
    policy.observe(state, 17);
    disconnectPreferred(18);
    reconnectPreferred(19);
    QVERIFY(policy.evaluate(state, 48).isEmpty());
    QCOMPARE(policy.evaluate(state, 49), state.preferredName);
}

void LinkPreferencePolicyTest::_emptyPrimaryKeepsSteadyClock()
{
    auto state = input();
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    state.primaryName.clear();
    policy.observe(state, 1);
    QVERIFY(policy.evaluate(state, 2).isEmpty());
    QCOMPARE(policy.evaluate(state, 3), state.preferredName);

    // Clearing a preferred primary during removal is not a selection of another link.
    LinkPreferencePolicy removal;
    state.primaryName = state.preferredName;
    removal.observe(state, 0);
    state.primaryName.clear();
    removal.observe(state, 10000);
    QCOMPARE(removal.evaluate(state, 10000), state.preferredName);
}

UT_REGISTER_TEST(LinkPreferencePolicyTest, TestLabel::Unit)
