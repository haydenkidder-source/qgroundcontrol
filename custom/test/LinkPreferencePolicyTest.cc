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

void LinkPreferencePolicyTest::_primaryChangeClassification_data()
{
    QTest::addColumn<QString>("previousStatus");
    QTest::addColumn<bool>("preferredLost");
    QTest::addColumn<bool>("primarySignal");
    for (const char* status : {"healthy", "lost", "removed", "missing-status", "empty-primary"}) {
        for (bool preferredLost : {false, true}) {
            for (bool primarySignal : {false, true}) {
                const auto row = QStringLiteral("%1-%2-%3")
                                     .arg(QString::fromLatin1(status))
                                     .arg(preferredLost ? "outage" : "healthy")
                                     .arg(primarySignal ? "signal" : "snapshot");
                QTest::newRow(qPrintable(row)) << QString::fromLatin1(status) << preferredLost << primarySignal;
            }
        }
    }
}

void LinkPreferencePolicyTest::_primaryChangeClassification()
{
    QFETCH(QString, previousStatus);
    QFETCH(bool, preferredLost);
    QFETCH(bool, primarySignal);
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
    state.primaryName = QStringLiteral("Backup B");
    policy.observe(state, 2, false, primarySignal);
    state.linkStatuses[state.linkNames.indexOf(state.preferredName)].clear();
    policy.observe(state, 3);
    if (previousStatus == QStringLiteral("healthy")) {
        QVERIFY(policy.evaluate(state, 1000).isEmpty());
    } else {
        QCOMPARE(policy.evaluate(state, 1000), state.preferredName);
    }
}

void LinkPreferencePolicyTest::_outagePickRelease_data()
{
    QTest::addColumn<QString>("release");
    for (const char* name : {"next-loss", "preferred-primary", "preference-change", "reset"}) {
        QTest::newRow(name) << QString::fromLatin1(name);
    }
}

void LinkPreferencePolicyTest::_outagePickRelease()
{
    QFETCH(QString, release);
    auto state = input();
    state.primaryName = state.preferredName;
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 1);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 1, false, true);
    // Stock failover is followed by an explicit re-pick during the outage.
    policy.observe(state, 2, false, true);
    QVERIFY(policy.evaluate(state, 100).isEmpty());
    state.linkStatuses[1].clear();
    policy.observe(state, 101);
    QVERIFY(policy.evaluate(state, 1000).isEmpty());
    if (release == QStringLiteral("next-loss")) {
        state.linkStatuses[1] = QStringLiteral("lost");
        policy.observe(state, 1001);
        state.linkStatuses[1].clear();
    } else if (release == QStringLiteral("preferred-primary")) {
        state.primaryName = state.preferredName;
        policy.observe(state, 1001, false, true);
        state.primaryName = QStringLiteral("RFD");
        policy.observe(state, 1002, true, true);
    } else if (release == QStringLiteral("preference-change")) {
        state.preferredName = QStringLiteral("New UDP");
        state.linkNames[1] = state.preferredName;
    } else {
        state.preferredName.clear();
        policy.observe(state, 1001);
        state.preferredName = QStringLiteral("UDP");
    }
    policy.observe(state, 1002);
    QCOMPARE(policy.evaluate(state, 2000), state.preferredName);
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
    policy.observe(state, 10000, true);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 10001);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 10002);
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 11000).isEmpty());
    QVERIFY(policy.evaluate(state, 25999).isEmpty());
    state.linkStatuses[1] = QStringLiteral("lost");
    QVERIFY(policy.evaluate(state, 26000).isEmpty());
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 27000).isEmpty());
    QVERIFY(policy.evaluate(state, 41999).isEmpty());
    QCOMPARE(policy.evaluate(state, 42000), state.preferredName);
    QVERIFY(policy.evaluate(state, 50000).isEmpty());
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
    policy.observe(state, 17, true);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, 17 + dropAfter);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 17 + dropAfter);
    state.linkStatuses[1].clear();
    const auto recovery = 18 + dropAfter;
    policy.observe(state, recovery);
    QVERIFY(policy.evaluate(state, recovery + hold - 1).isEmpty());
    QCOMPARE(policy.evaluate(state, recovery + hold), state.preferredName);
}

void LinkPreferencePolicyTest::_manualChoiceAndStockFailover_data()
{
    QTest::addColumn<bool>("samePrimary");
    QTest::addColumn<bool>("lostAtSignal");
    QTest::addColumn<bool>("clearByPrimary");
    QTest::newRow("manual-new-primary") << false << false << false;
    QTest::newRow("manual-current-primary") << true << false << false;
    QTest::newRow("manual-cleared-by-primary") << true << false << true;
    QTest::newRow("stock-failover") << false << true << false;
    QTest::newRow("current-primary-while-preferred-lost") << true << true << false;
}

void LinkPreferencePolicyTest::_manualChoiceAndStockFailover()
{
    QFETCH(bool, samePrimary);
    QFETCH(bool, lostAtSignal);
    QFETCH(bool, clearByPrimary);
    auto state = input();
    if (!samePrimary) {
        state.primaryName = state.preferredName;
    }
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    if (lostAtSignal) {
        state.linkStatuses[1] = QStringLiteral("lost");
        policy.observe(state, 1);
    }
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 1, false, true);
    state.linkStatuses[1].clear();
    QVERIFY(policy.evaluate(state, 2).isEmpty());
    if (lostAtSignal && !samePrimary) {
        const qint64 deadline = 17;
        QVERIFY(policy.evaluate(state, deadline - 1).isEmpty());
        QCOMPARE(policy.evaluate(state, deadline), state.preferredName);
    } else {
        QVERIFY(policy.evaluate(state, 1000).isEmpty());
        if (clearByPrimary) {
            state.primaryName = state.preferredName;
            policy.observe(state, 1001, false, true);
            state.primaryName = QStringLiteral("RFD");
            policy.observe(state, 1002, true, true);
            QCOMPARE(policy.evaluate(state, 1002), state.preferredName);
        }
        state.linkStatuses[1] = QStringLiteral("lost");
        policy.observe(state, 1002);
        state.linkStatuses[1].clear();
        QVERIFY(policy.evaluate(state, 1003).isEmpty());
        const qint64 deadline = (!samePrimary || clearByPrimary) ? 1018 : 1006;
        QVERIFY(policy.evaluate(state, deadline - 1).isEmpty());
        QCOMPARE(policy.evaluate(state, deadline), state.preferredName);
    }
    state.primaryName = state.preferredName;
    policy.observe(state, 2000, true, true);
    // A policy-origin signal must not latch a manual override.
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, 2001, true, true);
    QCOMPARE(policy.evaluate(state, 2002), state.preferredName);
    state.primaryName = state.preferredName;
    policy.observe(state, 2002, true, true);
    QVERIFY(policy.evaluate(state, 2003).isEmpty());
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
    LinkPreferencePolicy policy(timing());
    qint64 now = 0;
    policy.observe(state, now);
    qint64 hold = 15;
    for (int failure = 0; failure < 7; ++failure) {
        state.linkStatuses[1] = QStringLiteral("lost");
        policy.observe(state, ++now);
        state.primaryName = QStringLiteral("RFD");
        policy.observe(state, now);
        state.linkStatuses[1].clear();
        policy.observe(state, ++now);
        QVERIFY(policy.evaluate(state, now + hold - 1).isEmpty());
        now += hold;
        QCOMPARE(policy.evaluate(state, now), state.preferredName);
        state.primaryName = state.preferredName;
        policy.observe(state, now, true);
        hold = qMin(hold * 2, qint64(300));
    }
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, ++now);
    now += 300;
    policy.observe(state, now);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, ++now);
    state.linkStatuses[1].clear();
    policy.observe(state, ++now);
    QVERIFY(policy.evaluate(state, now + 299).isEmpty());
    now += 300;
    QCOMPARE(policy.evaluate(state, now), state.preferredName);
    state.primaryName = state.preferredName;
    policy.observe(state, now, true);
    now += 300;
    policy.observe(state, now);
    state.linkStatuses[1] = QStringLiteral("lost");
    policy.observe(state, ++now);
    state.primaryName = QStringLiteral("RFD");
    policy.observe(state, now);
    state.linkStatuses[1].clear();
    policy.observe(state, ++now);
    QVERIFY(policy.evaluate(state, now + 14).isEmpty());
    QCOMPARE(policy.evaluate(state, now + 15), state.preferredName);
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
        policy.observe(state, now, false, true);
        policy.observe(state, now);
        state.linkNames.removeLast();
        state.linkStatuses.removeLast();
        state.primaryName = QStringLiteral("RFD");
        policy.observe(state, now, false, true);
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
    policy.observe(state, 17, true, true);
    disconnectPreferred(18);
    reconnectPreferred(19);
    QVERIFY(policy.evaluate(state, 48).isEmpty());
    QCOMPARE(policy.evaluate(state, 49), state.preferredName);
}

void LinkPreferencePolicyTest::_emptyPrimarySignalDoesNotOverride()
{
    auto state = input();
    LinkPreferencePolicy policy(timing());
    policy.observe(state, 0);
    state.primaryName.clear();
    policy.observe(state, 1, false, true);
    QVERIFY(policy.evaluate(state, 2).isEmpty());
    QCOMPARE(policy.evaluate(state, 3), state.preferredName);
}

UT_REGISTER_TEST(LinkPreferencePolicyTest, TestLabel::Unit)
