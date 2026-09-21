# MAVLink loss on the Microhard mesh

**Assessment: shared-link loss/queueing is the leading explanation for the reported
cross-vehicle failures. QGC has several mechanisms that amplify the impact, including
one concrete command retry timing defect fixed here. This is source-level analysis,
not a hardware-confirmed diagnosis or a claim that the mesh is now reliable.**

## Scope and evidence

Reviewed on `claude/link-loss-mesh-tuning`, initially identical to fork `master`
`4150ac06e6651c13188dd3176035c6818f3c95d5`. The eight-minute log evidence is the
operator's supplied summary; the raw log, packet capture, radio counters, firmware
versions and actual saved stream settings were not available.

The confirmed topology is one Microhard 2450 per vehicle, plus GCS, sharing one
8 MHz mesh with automatic rate scaling. There is no assumed single-vehicle relay.
UDP provides neither radio collision avoidance nor channel arbitration; those are
MAC/PHY responsibilities. It also supplies no congestion control or retransmission
([RFC 8085](https://www.rfc-editor.org/rfc/rfc8085.html)). If this radio firmware uses
contention-based access rather than strict TDMA, simultaneous transmissions and a
derated radio's increased airtime are a very plausible source of correlated bursts
of loss. The exact radio MAC mode is not established here. An 8 MHz channel width
alone does not establish usable throughput or prove saturation.

Multiple protocols failing on two independent vehicle streams makes a shared RF or
queueing bottleneck more likely than separate protocol defects. Nevertheless,
`MAV_RESULT_FAILED` alone is not proof of loss: distinguish a negative ACK actually
received from the vehicle from QGC's synthetic failure after no ACK/message. Capture
both directions and inspect the logged failure code before classifying each event.

## Finding 1 — Retry budgets are fixed, and some are shorter than they appear

Source references below are relative to the repository root; function names are
included so references survive subsequent line-number changes.

| Path | Actual production behavior before this change | Assessment for this mesh |
| --- | --- | --- |
| `src/Vehicle/MavCommandQueue.{cc,h}` | ACK window 1200 ms; timeout polling 500 ms; retry allowlist gets 3 total sends, other commands 1. High-latency flag selects 120000 ms. No exponential backoff/jitter or measured RTT adaptation. | A 1.2 s response budget can be exceeded by RF queueing. More seriously, retransmissions did not restart the timer. |
| `src/FactSystem/ParameterManager.{cc,h}` | Initial list response window 5000 ms, up to 4 retries. Missing-index recovery follows 3000 ms inactivity, with at most 10 outstanding index requests per vehicle and 5 re-read attempts per index. | Already more tolerant than mission item reads; still fixed and not a mesh-wide budget. Multiple vehicles can have 20-30 recovery requests outstanding. |
| `src/MissionManager/PlanManager.{cc,h}` | `AckMissionItem`: 250 ms. Count/request/clear/guided states: 1500 ms. `_maxRetryCount = 5`. | These are ordinary MAVLink defaults, not mesh tuning. Download retries can repeatedly fall inside one RF loss burst. Upload recovery is particularly impatient. |

### Command retry defect and implemented correction

`sendWorker()` starts `entry.elapsedTimer`. Previously `_sendFromList()` never
restarted it. Once expired, `_responseTimeoutCheck()` retransmitted on every
500 ms poll. With three sends and no ACK, idealized give-up was around 2.2-2.7 s
from initial send, with only about 500 ms after the last send. Timer scheduling can
extend those figures. The 120 s high-latency selection had the same defect after
its first window.

The fix restarts the timer immediately before each actual transmission. Every send,
including the last retry, now gets the configured ACK window: at least 3.6 s total
for three ordinary sends, plus polling/scheduling delay. It adds no retries, changes
no default constants and preserves the safety allowlist that avoids delayed repeated
ARM/control commands. It benefits retryable status requests such as AVAILABLE_MODES;
it cannot repair mission/parameter loss or guarantee delivery through a long outage.

`SET_MESSAGE_INTERVAL` is **not** on the retry allowlist. Its "max retries" warning
can mean one unsuccessful send, not three. Repeated warnings throughout the session
are separate invocations, not one command retrying for eight minutes. Duplicate
interval commands are allowed; ACKs are matched by component and command, without
the requested message ID, so a warning's message label is not definitive attribution
when these requests overlap.

### Mission state distinctions matter

In `PlanManager::_ackTimeout()`, count/item/clear and the initial upload-count paths
use `if (_retryCount > _maxRetryCount)` before incrementing. Starting at zero, that
permits six retransmissions, seven waits, rather than five retransmissions. A missing
downloaded item therefore exhausts its budget in roughly 1.75 s; count/clear paths
can last roughly 10.5 s without progress. These are per-step budgets, not limits on
an entire mission transfer.

**A stalled upload after some items have been requested, or a lost final ACK, fails
after one 1500 ms window.** Those `AckMissionRequest` branches do not use the retry
counter. Raising `_maxRetryCount` would not fix the two reported upload errors.
The vehicle drives upload item requests and its own retry timing also matters.
`AckMissionItem` is the download wait for an item, not the upload final-ACK state.

The [MAVLink mission protocol](https://mavlink.io/en/services/mission.html#timeouts-and-retries)
recommends 1500 ms, 250 ms and five retries. Thus these are stock protocol defaults,
not evidence of deliberate USB-only tuning. They are nevertheless too short when
the mesh's loss bursts or round-trip tails exceed those windows. No finite retry
count compensates for sustained offered load above usable capacity.

### Parameter recovery is bounded, but not adaptive

`_fillIndexBatchQueue()` refills on replies and clears/reissues its outstanding
batch on inactivity timeout. Received parameters restart the inactivity timer; there
is no single fixed "15 s overall download deadline." A repeatedly absent index gets
five requests separated by inactivity windows, potentially much longer while other
parameters keep arriving. Non-default components can also be abandoned after two
silent cycles with at least five outstanding requests; the default autopilot is
exempt from this shortcut. Named reads/writes use two retries and a 1000 ms ACK wait,
distinct from initial index recovery. More retries without lower traffic can worsen
contention rather than resolve it.

## Finding 2 — QGC can reinforce congestion and synchronized recovery

`src/FirmwarePlugin/APM/APMFirmwarePlugin.cc`, `initializeStreamRates()` and
`adjustIncomingMavlinkMessage()`, explain the recurring HOME/EXTENDED warnings:

- Each ArduPilot vehicle independently requests stream groups. Defaults in
  `src/Settings/APMMavlinkStreamRate.SettingsGroup.json` are raw sensors 2 Hz,
  extended status 2, RC 2, position 3, EXTRA1 10, EXTRA2 10 and EXTRA3 3.
  These are **group rates**, not a total of 32 MAVLink packets/s: groups contain
  multiple messages and vary by vehicle firmware. No per-fleet airtime division occurs.
- HOME_POSITION and EXTENDED_SYS_STATE are separately requested at 1 Hz, even when
  `apmStartMavlinkStreams` is disabled. If either battery or home telemetry has been
  absent for more than ten seconds, an incoming message triggers full stream-rate
  reinitialization, resets the watchdog timestamps and sends both interval commands
  again. This fits the reported 10-20 s cadence; it does not prove the caller without
  the full log. Missing/unsupported battery data can also drive this watchdog.
- `Vehicle::requestDataStream()` defaults to five copies through
  `sendMessageMultiple()`. This is paced at one queued message per 500 ms per vehicle,
  not all 35 group requests in one burst. Seven groups take about 17.5 s to drain
  with no other queued traffic. Reinitializing every roughly 11 s can append work
  faster than that queue drains. There is no coalescing of obsolete stream requests.
- `InitialConnectStateMachine` sequences modes, metadata, parameters, mission,
  fence and rally within each vehicle. `RequestMessageCoordinator` serializes
  requests per component; StandardModes requests one mode at a time. These are
  useful existing limits. They do not stagger separate vehicles sharing the mesh;
  their connect/download sequences can overlap with telemetry and other subsystems.
- `MAVLinkStreamConfig` has explicit 100 Hz tuning streams. These are opt-in
  high-rate views, not the normal connection default. Avoid tuning/inspector rate
  overrides during bandwidth testing; verify actual message rates on the wire.

`src/Comms/UDPLink.cc`, `UDPWorker::writeData()`, sends to all configured targets
and all learned session endpoints (deduplicating a configured endpoint already in
the session list). It does not route by MAVLink target system. **If** the vehicles
share one QGC UDP link, each targeted request can be sent to every vehicle endpoint.
That multiplies outgoing datagrams without causing the non-target vehicle to execute
the command. This is conditional on the actual socket configuration, not a change
to the confirmed radio topology. Check captures for this amplification; a byte-send
signal alone does not account for the multiple socket writes. There is no radio
airtime feedback/backpressure in this UDP send path.

## Finding 3 — Existing lossy-link features are not a mesh congestion controller

Comparison baseline: local `upstream/master`
[`cd7132bf07586516bbf39f6e61934e9d062e4c6a`](https://github.com/mavlink/qgroundcontrol/commit/cd7132bf07586516bbf39f6e61934e9d062e4c6a),
dated 2026-09-19. Live upstream raw source was also consulted on 2026-09-20;
mutable web snapshots can differ, so the precise diff claims use the pinned local
ref. At the starting fork commit, command queue, PlanManager, StandardModes,
RequestMessageCoordinator, parameter header constants, APM stream initialization,
stream default JSON, BulkRefreshJob and FTPManager.cc matched that upstream ref.
**No fork-specific mesh retry tuning was found in the reviewed production paths.**

The ParameterManager implementation differs by an already-shipped per-process,
per-vehicle FTP download filename and a defensive offline-file check, not retry
timing. Those changes are preserved. The held `claude/ftp-cube-hardening` branch was
not merged, and FTPManager was not modified.

Existing features in this checkout and the pinned upstream include:

- High-latency link mode increases command ACK timeouts to 120 s but skips normal
  parameter/initial plan loading. It is not a suitable "lossy mesh" switch and does
  not tune PlanManager's ordinary transfer constants.
- PX4 parameter hash/cache reuse and parameter FTP support reduce download work;
  `_tryftp` starts enabled for APM/PX4 and falls back to the stream path on failure.
  Preserve the existing fallback and cache behavior.
- `BulkRefreshJob` uses 1, 2 and 4 s delays between failed named-parameter refresh
  rounds. This is not initial missing-index recovery or global traffic adaptation.
- FTP already uses a 3000 ms ACK timer and missing-block recovery. Radio detection
  via `RADIO_STATUS` selects smaller 110-byte read chunks for SiK/RFD framing.
  An Ethernet/UDP Microhard bridge is not automatically such a radio; do not force
  that classification or transfer its framing assumptions to this mesh.
- Radio status/packet-loss Facts provide visibility, but the reviewed command,
  parameter and mission retry loops do not consume them to adjust timing/rates.
  There is no enabled/disabled mesh-wide adaptive scheduler to turn on here.

Autopilot-side adaptation is separate: PX4's
[`MAV_0_RADIO_CTL`](https://docs.px4.io/main/en/advanced_config/parameter_reference#MAV_0_RADIO_CTL)
can throttle using `RADIO_STATUS.txbuf`, but requires that feedback from the radio.
Do not assume an IP bridge supplies it, or apply PX4 parameters to ArduPilot.
ArduPilot documents reduced telemetry rates during parameter/waypoint transfers and
warns about conflicting stream-rate requesters in its
[data-rate guide](https://ardupilot.org/dev/docs/mavlink-requesting-data.html).
Neither provides a QGC-wide shared-mesh airtime budget.

## Recommended changes and hardware validation

1. **Reduce offered load before increasing retries.** On the ground, start with one
   vehicle, then two, then Rover + Stallion + Hex, preserving the same RF conditions.
   Use the existing Fact-backed stream settings to trial EXTRA1 5 Hz and EXTRA2
   2 Hz instead of 10/10, then measure control/display freshness and loss before
   accepting those rates. These are experiment values, not flight-approved defaults.
   Keep battery/home/status available so intentional omissions do not trigger the
   watchdog. A live stream-setting edit targets only the active vehicle; preconfigure
   before connecting or explicitly verify each vehicle's resulting rates.
2. For firmware-owned rates, disable QGC's group stream requests or use "Controlled
   By Vehicle" (-1), and configure the correct firmware/channel through Facts.
   This does not stop an already-running stream or suppress QGC's two 1 Hz interval
   requests. Avoid competing GCS/companion rate requests. Measure actual aggregate
   packet/byte rates rather than adding the group-rate numbers.
3. Stagger vehicle connections and bulk operations operationally: finish one
   parameter/mission transfer before starting the next, and defer optional FTP,
   logs or tuning traffic. If captures show UDP fanout, trial separate local UDP
   ports with one vehicle sender per link, or an existing correctly configured
   MAVLink router. Merely adding a target to the same shared socket will not isolate
   it. Do not rewrite routing without confirming the amplification first.
4. Verify radio MAC/scheduling mode, rate history, retries/drops, noise/SNR and queue
   occupancy using the deployed firmware's diagnostics. Budget for the worst usable
   rate, all transmitters, mesh retransmissions/hops and any video or other traffic
   actually present. Preserve airtime headroom at range. If loss remains strongly
   correlated as rates derate, RF/channel/load configuration is the primary fix;
   longer QGC timers only tolerate some of the resulting interruptions.
5. After lowering load, measure request-to-reply tails and longest correlated loss
   bursts. If they still exceed the mission windows, a small explicit mesh profile
   could trial 1000 ms item waits and 3000-5000 ms ACK waits, keeping retry counts
   bounded. Those values are hypotheses pending measurements. Raising the upload
   ACK wait helps delayed replies but cannot recover a permanently lost final ACK;
   any retransmission change needs firmware/protocol tests for duplicate final
   items/ACKs. Do not globally extend ARM/control-command retries.
6. If watchdog reinitializations persist after load reduction, the next small code
   candidate is coalescing stale pending stream-group requests or backing off that
   specific recovery loop. Test eventual reinitialization after reboot as well as
   queue bounds; suppressing all recovery would break legitimate stream restoration.
   Link-quality-driven retry tuning and cross-vehicle scheduling are larger follow-ups,
   not justified as speculative changes in this patch.

Record QGC/firmware versions, saved settings, each IP/port and sysid/compid, per-message
rates, sent requests/received ACKs, transfer completion/missing indices, latency tails
and burst lengths. Correlate timestamped captures at GCS and vehicle sides with radio
counters; sequence gaps alone can also reflect reordering/routing. Repeat mission,
fence and rally uploads including first request, middle item and final ACK under
controlled delay/loss on a bench. Compare the timer fix with its parent commit under
identical conditions. Independent random drops alone are insufficient to model this
report's correlated outages. Define acceptable telemetry age and command latency
before the three-vehicle trial.

## Regression coverage and execution evidence

`SendMavCommandWithSignallingTest::_retryAckWindows` uses the existing connected
MockLink and no-response command, with the existing 500 ms ACK-timeout fixture.
It checks that failure cannot occur before three complete ACK windows, that exactly
three commands reached MockLink, and that the terminal result is no-response.
Before the fix, the 50 ms test polling cadence normally gives up around 600-650 ms
instead of at least 1500 ms. The test has no fixed sleep. Existing cases cover
second-attempt ACKs, no-retry commands and IN_PROGRESS handling; the reentrancy suite
covers vehicle shutdown from a give-up callback.

Actual validation performed for this change: `just build` succeeded (Debug,
incremental), and `ctest -R SendMavCommandWithSignallingTest` passed, including the
new `_retryAckWindows` case, on this checkout.

No hardware reliability improvement is claimed as measured. The patch corrects a
source-confirmed retry-window defect; it does not establish RF capacity or validate
the proposed stream/mission timeout experiments. Separately, PlanManager's
`AckMissionRequest` handling of a lost final MISSION_ACK or a lost mid-upload
MISSION_REQUEST fails after a single 1500 ms window with no retry, unlike the other
Ack states in the same state machine - this is a distinct, not-yet-fixed gap that
plausibly accounts for the reported "vehicle failed to send final ack" and "vehicle
did not request all items from ground station" upload failures on a lossy link, and
is being investigated as a follow-up.
