# Multi-vehicle UDP port topology audit

**Assessment: retain one dedicated GCS UDP port per vehicle. This isolates the
MAVLink byte parsers and removes cross-vehicle outbound fanout when each link has
exactly one destination endpoint. It does not enforce that endpoint count. Learned
senders persist until disconnect, and configuration and receive-filter exceptions
can weaken the intended isolation. Normal command, parameter and mission state is
vehicle-scoped, not tied to the active UI vehicle.**

## Scope and evidence

Source reviewed on `claude/multi-vehicle-port-topology-audit`, baseline
`a4fb07fe0adfae7c209a6285d7a4381fc1a84ffd`, on 2026-09-23. References are relative
to the repository root; function names identify the relevant code even if line
numbers subsequently change. This is a bounded source audit, not a reliability
sign-off or an exhaustive review of every multi-vehicle subsystem.

The operator reports that on 2026-09-22 each Microhard pMDDL2450 bridge was given a
distinct destination UDP port at the GCS. Rover, Stallion and Hex still share the
same 8 MHz radio mesh. The approximately 19-minute test, timestamps 21:02–21:21,
had zero reported malformed-packet/corruption/CRC symptoms after the change. The
operator also reports timer/buffer-based serial-to-IP chunking and previously clean
radio drop counters despite corrupted MAVLink reception. These are supplied field
observations, not independently inspected logs, captures or radio documentation.

This closes the operational trial proposed by Finding 2 and Recommendation 3 of
[the link-loss investigation](LINK_LOSS_MESH_FINDINGS.md): the new topology worked
in the reported test. The code below explains why it can prevent byte interleaving.
The observation alone does not measure the previous outbound amplification, prove
which prior datagrams interleaved, or establish performance under future RF loads.
The 446+ interval-command warnings and the already-merged APM reinitialization
backoff work are explicitly outside this audit; no retry-timing investigation is
repeated here.

## Ranking

- **A — confirmed by tracing the code, fix now:** deterministic defect under the
  stated trigger; field occurrence need not be established. This is a triage
  recommendation, not a claim that a patch was implemented here.
- **B — plausible mechanism, worth a targeted regression test even without a live
  bug:** code behavior established, but trigger frequency, platform behavior or the
  appropriate policy needs validation.
- **C — theoretical/low-confidence, worth watching for, not actionable yet:** no
  demonstrated deployment trigger. Do not change production policy solely for it.

| Finding | Rank | Likelihood in this deployment | Impact if triggered |
| --- | --- | --- | --- |
| 1. Learned endpoint accumulation | B | Certain after a new sender tuple; actual radio/NAT tuple churn unverified | Extra outbound copies; possible duplicates or renewed mixed inbound streams |
| 2a. Startup checkbox changes a manual UDP port | A | Deterministic when the checkbox changes; operator use unverified | Dedicated listener can move away from its assigned port |
| 2b. Duplicate local listener ports are not rejected | B | Conditional on configuration; not inherent to default 14550 plus manual 14551 | Bind failure or platform-dependent delivery to competing sockets |
| 3. Metadata FTP downloads share temporary basenames | A | Conditional on overlapping cache misses with equal basenames; deployed metadata URIs unknown | Local metadata overwrite/mixing or failed processing |
| 4. Source sysid zero bypasses link isolation | B | Low/unknown: no evidence these bridges emit it | Radio Facts can be copied to unrelated vehicles; other messages can add unrelated links |
| 5. Unrelated vehicle removal resets active selection | A, existing finding | Deterministic for the described three-vehicle selection/removal order | Operator's active vehicle changes unexpectedly |
| 6. UDP topology regression coverage missing | B | Confirmed gap in reviewed tests | Endpoint/configuration/parser regressions lack a direct transport test |
| Boundary: duplicate vehicle sysids after a replacement | C for field occurrence | No evidence of duplicate sysids here | Two physical vehicles merge into one logical Vehicle |

## Finding 1 — A dedicated port is not an exclusive peer binding

**Rank B. Endpoint retention and fanout are source-confirmed; endpoint churn in the
Microhard deployment is unconfirmed.**

`src/Comms/UDPLink.cc:440`, `UDPWorker::_onSocketReadyRead()`, drains nonempty UDP
datagrams, appends their payloads to one buffer and emits bytes without sender
metadata. It then adds each previously unseen sender address/port to
`_sessionTargets`. Loopback and addresses belonging to the GCS are normalized to
localhost; remote senders retain their address. `containsTarget()` compares address
**and** port. There is no MAVLink validation, sysid check, expected-peer allowlist,
expiry, last-seen replacement or endpoint-count limit in this learning path.
`src/Comms/UDPLink.h`, `UDPWorker`, owns a separate list per worker.

`UDPWorker::writeData()` (`UDPLink.cc:391`) sends the same bytes to every resolved
configured target that is not already learned, then every learned target. Thus a
configured endpoint equal to a learned endpoint gets one copy, not two. Different
configured/learned tuples still get separate copies, even if both reach the same
radio. The function does not inspect a MAVLink target system. `dataSent` is emitted
once after the loops, not once per datagram.

Concrete source-level sequence:

1. Link A receives bytes from `radio-A-IP:port-P` and learns that tuple.
2. It later receives from `radio-A-IP:port-Q`, or a new remote address. Both tuples
   remain in the list.
3. A command for A is serialized with A's target sysid by
   `src/Vehicle/MavCommandQueue.cc`, `_sendFromList()`, and sent on A's primary link.
4. That link's worker transmits it to both tuples. This does not send through the
   workers for B and C. If the old tuple is unreachable, it is wasted traffic; if
   both paths still reach A, duplicate delivery is possible. If the old endpoint
   has been reassigned, it receives A-targeted bytes. Whether another autopilot
   executes anything requires its target filtering and the message type; execution
   by the wrong aircraft is **not** established here.

`disconnectLink()` (`UDPLink.cc:376`) clears the learned list after closing a
connected socket. `_onSocketDisconnected()` only updates state/signals. No timer
ages individual peers out. Recreating the link gives a new worker/list; a radio
reboot without closing the GCS UDP socket does not itself clear this state.
Removing a configured host operates on `_targetHosts`, not `_sessionTargets`
(`UDPConfiguration::removeHost()`). Therefore it is not an eviction mechanism for
an already-learned peer.

For stable remote endpoints, one configured/learned tuple per link means one copy
per request on that link. This closes the original cross-vehicle fanout concern
under that explicit condition. Merely changing a mesh route does not necessarily
change the sender tuple; source cannot establish NAT or reboot behavior. Also,
specifying one configured target does not prevent other senders from being learned.

The inbound benefit is separate: `LinkManager::createConnectedLink()` allocates a
MAVLink channel for each link, and `src/Comms/MAVLinkProtocol.cc:102`,
`receiveBytes()`, calls `mavlink_parse_char(link->mavlinkChannel(), ...)`. Splitting
one vehicle's MAVLink frame across its own consecutive datagrams is compatible
with that persistent parser. Alternating partial frames from different senders on
one UDPLink feeds one parser with the combined bytes; separating their local ports
and links separates parser state. If a second byte stream starts reaching the same
port, this hazard can return. Learned stale destinations alone do not corrupt
incoming bytes; additional incoming traffic is required.

**Follow-up:** loopback socket tests should learn P then Q, count actual outbound
datagrams, verify configured/learned deduplication, and verify clearing after
connected disconnect/recreation. Separately test split frames across three links.
Do not globally replace learning with “last sender wins”: UDPLink also supports
intentional multi-peer use. An exclusive-peer mode or expiry policy needs an
explicit compatibility decision. Capture sender tuples across an actual radio
reboot before attributing this mechanism to the field.

## Finding 2 — Autoconnect has two different configuration hazards

### 2a. The startup checkbox rewrites manual UDP addressing

**Rank A. High confidence in configuration mutation; no GUI/runtime repro run.**

`src/AppSettings/LinkConfigurationManager.qml:146` labels a checkbox
“Automatically Connect on Start” and assigns `editingConfig.autoConnect` when it
changes. That property uses the virtual setter in `src/Comms/LinkConfiguration.h`.
For UDP, `UDPConfiguration::setAutoConnect()` (`UDPLink.cc:66`) does more than set a
startup preference:

- Enabling it sets `localPort` to the global `udpListenPort` and adds the global
  target host/port when configured.
- Disabling it sets `localPort` to **zero** and removes that global target when
  configured.
- The mutation occurs only when the boolean changes.

Thus a manual port 14551 with startup disabled becomes the global listener port
(default 14550) when startup is enabled. It can also acquire an unrelated outbound
target. `LinkManager::endConfigurationEditing()` copies and saves the edited
configuration; `UDPConfiguration::copyFrom()` copies the edited port and targets.
`src/AppSettings/UdpSettings.qml` binds the port field to `localPort`; its
`saveSettings()` is a no-op, so it does not restore an earlier value on Save.
The effect on a subsequently opened socket follows `UDPWorker::connectLink()`;
this is not a claim that an already-bound socket immediately rebinds.

Loading an already-correct saved configuration is different:
`LinkManager::loadLinkConfigurationList()` sets autoconnect before calling
`UDPConfiguration::loadSettings()`, which restores the saved port/hosts. Do not
claim every restart destroys manual ports. The problematic trigger is changing
the startup flag without restoring the desired addressing afterward.

**Fix direction:** keep saved-link startup policy independent of addressing; put
global listener defaults in the dynamic autoconnect creation path. Test both toggle
directions, copy/save/reload, global-target preservation, and dynamic listener
creation before changing this behavior. Meanwhile verify the port and target list
after editing startup settings and before reconnecting.

### 2b. Default autoconnect does not reserve 14551, but duplicate binds are possible

**Rank B. Missing conflict check is confirmed; exact socket delivery is untested.**

`src/Settings/AutoConnect.SettingsGroup.json` defaults `udpListenPort` to **14550**.
`LinkManager::_addUDPAutoConnectLink()` (`src/Comms/LinkManager.cc:459`) creates one
dynamic `UDP Link (AutoConnect)` using that setting. It does not scan 14551 or
allocate adjacent listening ports. Manual 14551 and default autoconnect 14550
therefore have no inherent same-port conflict in this code.

However, the existence check compares UDP type and **configuration name**, not
local port. `createConnectedLink()` has no UDP port reservation check.
`UDPWorker::connectLink()` binds AnyIPv4 with `ReuseAddressHint | ShareAddress`.
A manual link using the selected autoconnect port, two manual links using one port,
or the mutation in 2a can therefore request overlapping binds. Source establishes
those requests, not whether a particular OS fails the bind, distributes unicast
traffic, or duplicates some traffic. Do not infer a reliable exclusive owner from
a successful bind. Multiple parsers receiving different fragments would undermine
the intended isolation, but that delivery pattern needs a platform test.

**Follow-up:** reserve non-overlapping local ports, including the configured
explicit autoconnect listener. Test manual/autoconnect creation in both orders
with real sockets on supported platforms. Record which listener receives each
fragment. Source does not justify disabling a distinct, unused 14550 listener
solely because manual 14551 exists.

## Finding 3 — FTP wire isolation does not guarantee local file isolation

**Rank A. Confirmed shared-path defect under concurrent equal-basename downloads;
field reachability depends on advertised metadata and cache misses.**

`Vehicle` creates its own FTPManager and ComponentInformationManager
(`src/Vehicle/Vehicle.cc`, `_commonInit()`). FTP replies are strongly scoped:
`FTPManager::_mavlinkMessageReceived()` (`src/Vehicle/FTPManager.cc:609`) checks
message type, vehicle sysid, requested component, active operation and GCS target
system before handling response state. Session checks also occur in the data
handlers. Equal FTP session numbers on different vehicles do not by themselves
mix replies.

A different collision exists on disk:

1. `InitialConnectStateMachine::_requestCompInfo()` starts that vehicle's component
   information request. `ComponentInformationManager` owns its request state
   machine; it does not serialize all vehicles through one global operation.
2. On an FTP metadata cache miss,
   `src/Vehicle/ComponentInformation/RequestMetaDataTypeStateMachine.cc:461`,
   `_requestFile()`, calls that vehicle's FTPManager with the common
   `QStandardPaths::TempLocation` directory and **no output filename override**.
3. `FTPManager::download()` derives the output basename from the remote path when
   the override is empty. Its busy guard is per FTPManager, so it does not prevent
   another vehicle downloading the same basename concurrently.
4. `_openFileROAckOrNak()` (`FTPManager.cc:722`) opens that local path with
   `WriteOnly | Truncate`. Both downloads can consequently target the same local
   file. Subsequent offset writes can overlap; completion passes the filename to
   metadata processing. `_ftpDownloadComplete()` and
   `_downloadCompleteJsonWorker()` then consume/decompress/cache the file.

A bench trigger is two vehicles advertising FTP metadata with identical basenames
but different bytes, empty caches, and overlapping downloads. The local pathname
collision is certain under those inputs. The exact outcome—truncation, mixed
content, processing failure or wrong content consumed—depends on interleaving and
file handling; none was observed in a running application here. The fleet's
advertised URIs and whether this path runs on its firmware were not supplied.

This is a **new angle**, not the prior parameter-transfer retry analysis.
`ParameterManager::_startParameterDownload()` already supplies
`param-<process-id>-<vehicle-id>.pck`, protecting that particular download. That
fix does not cover this metadata caller.

**Fix direction:** use transfer-owned temporary paths and retain the expected
compression suffix; review decompression/cache insertion and cleanup ownership as
part of the fix. A regression should overlap two downloads with the same remote
basename and different known payloads, then verify each consumer's bytes and
cleanup. A filename-only edit without exercising that lifecycle is not presented
as a verified fix in this audit.

## Finding 4 — Radio Facts are per vehicle, with a source-zero exception

**Rank B. Dispatcher exception confirmed; no source-zero field traffic established.**

`Vehicle::_commonInit()` creates a separate `RadioStatusFactGroup(this)` for each
vehicle. `src/Vehicle/FactGroups/RadioStatusFactGroup.cc`, `handleMessage()` and
`_handleRadioStatus()`, update that instance's Facts. There is no global RSSI
average. For a nonzero radio sysid different from the vehicle sysid,
`Vehicle::_mavlinkMessageReceived()` (`Vehicle.cc:528`) admits RADIO_STATUS only
when that vehicle already contains the incoming link. This supports radios using
their own identity. On one-link-per-vehicle connections, that exception normally
routes radio data to the appropriate vehicle.

But the outer filter accepts **all messages whose source `message.sysid == 0`**,
without requiring the incoming link to belong to the vehicle. Every connected
Vehicle subscribes to `MAVLinkProtocol::messageReceived`. A source-zero
RADIO_STATUS therefore passes this gate for every vehicle, even on an unrelated
port. The APM incoming adjustment accepts that message, and the radio FactGroup
uses it without another identity check. `VehicleLinkManager::mavlinkMessageReceived()`
ignores RADIO_STATUS for liveness/link addition, so this particular case copies
radio values without adding links.

For other source-zero messages, that same link manager adds a previously unknown
link or refreshes its liveness timer. A parsed source-zero message can therefore
associate an unrelated port with multiple vehicles before later protocol-specific
filters run. This is a source identity exception, **not** the common
`target_system == 0` broadcast-address convention. Do not confuse the two.

For ordinary nonzero vehicle identities, RC RSSI also follows the vehicle receive
filter: `Vehicle::_handleRCChannels()` updates that vehicle's RSSI.
`src/Toolbar/TelemetryRSSIIndicator.qml` and `RCRSSIIndicator.qml` display the
active vehicle's values, not a fleet aggregate. Multiple actual radio links
belonging to one vehicle write the same radio Facts; there is no per-link selection
inside `RadioStatusFactGroup`. This does not hurt the stated one-link topology but
matters if redundant links or accidental link association appear.

**Follow-up:** inject distinct RADIO_STATUS values on two links using normal radio
identities and source zero; assert the intended Fact and link-membership isolation.
Determine why source-zero acceptance exists before tightening it globally. No
Microhard RSSI availability, source identity or numerical accuracy is inferred from
these QGC consumers.

## Finding 5 — Independent disconnect can change an unrelated active vehicle

**Rank A; carry-forward, not a newly discovered defect.**

The first finding in [the COP stress report](COP_STRESS_TEST_FINDINGS.md) remains
present in this baseline. I rechecked
`src/Vehicle/MultiVehicleManager.cc:149`, `_deleteVehiclePhase1()`, which clears
active/parameter-ready availability for any removed vehicle, and
`_deleteVehiclePhase2()` at line 195, which unconditionally selects the first
remaining vehicle.

With Rover first, Hex explicitly active, and Stallion removed, phase 2 selects
Rover. `VehicleLinkManager::_linkDisconnected()` reaches this deletion path when
the removed link was the vehicle's last link. Separate physical/logical links make
such independent removals relevant; this is not a UDP parser or command-queue
misattribution. Mere radio silence does not necessarily destroy a UDPLink:
`_commLostCheck()` normally marks communication loss and closes the vehicle only
when its auto-disconnect policy applies.

`custom/test/COPStressTest.cc`,
`COPStressUITest::_disconnectOtherPreservesControl()`, already supplies the
three-MockLink repro. It was not run here. Preserve a surviving active selection
and validate asynchronous removal/activation before fixing this; do not duplicate
the prior report's separate control-switch race analysis in this audit.

## Findings ruled out or bounded by source

### Command ACKs: queue isolation prevents ordinary cross-vehicle matching

`Vehicle::_commonInit()` allocates `new MavCommandQueue(this)` per vehicle.
`Vehicle::_mavlinkMessageReceived()` rejects another nonzero sysid before
`_handleCommandAck()` delegates to that queue. In
`src/Vehicle/MavCommandQueue.cc`, `handleCommandAck()` calls `findEntryIndex()`;
that function matches the first local entry with the ACK's component and command.
It does not compare the original requested message ID.

Consequently, Rover and Hex can each await the same command from component 1:
Rover's ordinary ACK does not complete Hex's queue. The original missing-message-ID
qualification remains **within one vehicle**, notably when duplicate commands are
allowed by `_canBeDuplicated()`. This is not a new cross-vehicle defect and needs
no routing rewrite. Source-zero traffic in Finding 4 and duplicate sysids below
are explicit limits to this conclusion; the ACK handler itself has no additional
source-sysid guard.

### Parameter and mission transactions use the owning vehicle

`Vehicle` constructs separate ParameterManager, MissionManager, GeoFenceManager
and RallyPointManager instances. ParameterManager receives messages from its
owning Vehicle dispatcher; its component/parameter maps and waiting lists are
instance members (`src/FactSystem/ParameterManager.{cc,h}`). Its request helpers
use `_vehicle->id()` and that vehicle's primary link. Component-only indexing
inside such an instance is not evidence of a global component-ID collision.

`src/MissionManager/PlanManager.cc`, `_connectToMavlink()`, connects to its
own Vehicle's message signal, not the global protocol signal. Its send paths use
that vehicle's primary link and sysid; count/item/request/ACK handlers check
`mission_type`. `_ackTimeoutTimer`, transfer lists and expected state belong to the
PlanManager instance (`PlanManager.h`). No “one active UI vehicle owns all mission
transfers” assumption was found in these paths. This is source isolation evidence,
not a completed simultaneous-transfer test; source-zero acceptance remains an
upstream boundary. Retry policies are outside scope.

### Sysid identity survives port swaps; duplicate sysids do not become distinct vehicles

`MultiVehicleManager::_vehicleHeartbeatInfo()` identifies existing vehicles using
`getVehicleById(vehicleId)`, not a socket or port. For an existing nonzero sysid,
subsequent traffic can add the new link through
`VehicleLinkManager::mavlinkMessageReceived()` and `_addLink()`. Each vehicle has
its own primary link selection. `_sendGCSHeartbeat()` iterates all connected,
non-high-latency links; it is not limited to the active vehicle's link.

`custom/src/VehicleRoles/VehicleRoleController.{h,cc}` stores only sysid, role and
name. `_indexForSysid()`, `nameForSysid()`, `roleForSysid()`, `_load()` and `_save()`
contain no link/port mapping. Reassigning ports or swapping a radio while retaining
the vehicle sysid therefore does not require changing this controller's mapping.

The boundary is global sysid uniqueness: two real autopilots with the same sysid
on two dedicated ports become one logical Vehicle with multiple links. Telemetry
and ACK identity cannot then distinguish them. This is confirmed identity
semantics, not a newly introduced bug; its occurrence in this fleet is **rank C**,
unconfirmed and not grounds for changing identity keys. Preserve unique vehicle
sysids when replacing hardware. VehicleRoleController is presentation mapping,
not evidence that two physical vehicles have distinct identities.

Packet-loss reporting also needs interpretation: `MAVLinkProtocol::_updateCounters()`
tracks sequences per channel/sysid/component but accumulates totals per channel;
`_updateStatus()` emits those totals tagged with the current message's sysid.
`Vehicle::_mavlinkMessageStatus()` accepts by sysid. One vehicle per link makes
this closer to vehicle-specific reporting; it is not a mesh-wide RF drop counter,
and multiple sysids/components on a link still contribute to its totals. This is
an observability qualification, not an additional field failure claim.

## Finding 6 — Existing multi-link mocks do not exercise UDP endpoint topology

**Rank B. Source coverage gap confirmed in the inspected test tree; historical CI
execution was not queried.**

| Inspected source | What it actually covers | What it does not establish |
| --- | --- | --- |
| `test/Vehicle/MultiVehicleManagerTest.cc` | One VehicleTest fixture; duplicate allLinksRemoved delivery | N independent UDP sockets/vehicles |
| `test/Vehicle/VehicleLinkManagerTest.cc` | Single-link cases and two links deliberately using one sysid, including failover/communication loss | Multiple distinct vehicles over separate UDP ports |
| `custom/test/COPStressTest.cc`, `_start()` and `_threeVehicleChurn()` | Three distinct sysids with three separate MockLinks, initial readiness and churn; another case checks unrelated removal | UDP bind ownership, endpoint learning, source-port changes or partial-frame datagrams |
| `src/Comms/MockLink/MockLink.cc` | One `_vehicleSystemId` per MockLink, optional ID increment, direct `bytesReceived` emission and its own incoming MAVLink parser | The UDPWorker/socket path |
| `test/Comms/LinkConfigurationTest.cc` | UDP host/port setters, copy and persistence/hostname cases | Live UDP delivery or startup-toggle port preservation |
| `test/Comms/LinkManagerTest.cc` | Mock autoconnect/reconnect policy and serial reservations | UDP autoconnect/manual-port collisions |

The custom tests are compiled and registered under `QGC_BUILD_TESTING` in
`custom/CMakeLists.txt:306`; COPStressUITest has Integration/Vehicle labels.
Therefore “CI only ever tests one vehicle” would be incorrect. The prior COP report
also explicitly did not establish successful runtime execution of its repros.
Registration is not proof of a historical CI pass.

A search of `test/` and `custom/test/` for UDPLink, UDPWorker and QUdpSocket found
UDPConfiguration tests and other UDP consumers, but no direct live UDPLink topology
test. `test/Comms/CMakeLists.txt` likewise registers no UDPLink test. GPS transport,
UdpIODevice and UdpForwarder tests exercise other production classes; they do not
cover the worker audited here. The defensible gap is **N vehicles on N real
UDPLinks with fragmented input and learned return endpoints**, not absence of all
N-vehicle/N-link coverage. No claim about all past CI runs follows from this search.

Prioritized regression work:

1. A small UDPWorker socket fixture for learning, actual fanout counts and clean
   disconnect/recreation (Finding 1), then a LinkManager integration case for
   duplicate ports and manual startup configuration (Finding 2).
2. Three real UDPLinks with distinct sysids, deliberately split MAVLink frames,
   interleaved arrival between ports, and identical component/command ACKs pending
   on two vehicles. Assert each ACK completes only its vehicle and other links
   remain usable after one disconnect. Use dynamically allocated test ports.
3. Source-zero radio and link-membership injection (Finding 4), plus overlapping
   equal-basename metadata transfers (Finding 3). Reuse the existing COP removal
   repro for Finding 5 rather than adding another equivalent UI fixture.

## Changes and validation

This pass adds only this report. No production code, test code, protocol constants
or saved settings were changed. No build directory was created and no CMake build,
CTest, application or hardware test was run, following the source-only/disk-budget
instruction. Proposed regressions above are specifications, **not passing tests**.

No patch met the combined bar of a small isolated change and a verified regression
in this pass. Endpoint eviction needs compatibility policy; configuration defaults
must be separated without breaking dynamic listeners; metadata isolation requires
checking file ownership through processing and cleanup. The existing removal
finding already has a repro. These are actionable follow-ups, not claims of fixes.

`pre-commit run --files custom/test/MULTI_VEHICLE_PORT_TOPOLOGY_AUDIT.md` passed
(exit 0), including Markdown lint, spelling, whitespace and secret checks.
No full-repository lint or application test pass is claimed. Publication results
are recorded with delivery. The most useful next field evidence is a timestamped mapping of local
ports to sysids and observed remote IP/port tuples, including one radio reboot,
alongside captures of actual outbound datagrams. That can confirm or rule out
Finding 1 without reopening the already-addressed stream-reinitialization issue.
