# Release notes

## v1.13.0

**Test build.** The UDP changes pass the automated Unit and Integration suites and a lab
burst benchmark, but have not run on the real Microhard mesh and radios. These
diagnostics are to find why file and parameter transfers are slow over UDP while serial
is fine. Treat v1.13.0 as a test build until the owner has accepted it on the bench.

Based on v1.12.1 plus the changes below, with no upstream merge. v1.13.0 also includes
the UDP disconnect fix (PR 53), which was merged after v1.12.1 but never released.

### Fixes in the fork

- **Disconnect while a UDP link is starting**: Disconnect before binding was ignored,
  leaving the link connected. Deleting it left a hidden link holding the port. Both now
  end the link and free the port. Connect twice no longer duplicates a connection's
  link.
- **Empty UDP datagram**: one zero-length datagram used to stop reception until
  reconnecting, though the link looked connected. Empty datagrams are now skipped
  quietly and never added as send targets.
- **UDP receive buffer**: each link now asks for 4 MiB after binding, up from the OS
  default (about 208 KB on Linux; Windows default unknown). Lab bursts of 1000 and 5000
  full-size frames lost up to 330 and 1,914 frames before, none after. The link uses
  whatever size the OS allows.

### New in the fork

- **FTP downloads** (`Vehicle.FTPManager`): one `FTP download summary:` per download,
  including parameter files: `remoteFile`, `result` (empty = success), `elapsedMs`,
  `bytesWritten`, `fileSize`, `packetsAccepted`, `duplicatePackets`, `latePackets`,
  `holesCreated`, `holeBytes`, `holeRepairRequests`, `burstRequests`, `burstTimeouts`,
  `holeRepairTimeouts`, `otherTimeouts`, `sessionReopens`, `smallestChunk`,
  `largestChunk` (239 bytes normally, 91 on radio, 78 on signed radio),
  `primaryLinkName`, `primaryLinkIsRadio`, `primaryLinkChanges`,
  `averageBytesPerSecond`. Many repair requests and timeouts with low throughput mean
  loss; no holes and low throughput mean a slow path. `primaryLinkChanges` above 0 means
  the transfer changed links. These logs appear only for enabled categories and cost
  almost nothing.
- **UDP traffic** (`Comms.UDPLink`): `UDP socket buffers:` at connection gives requested
  and actual receive buffer and send buffer. While datagrams flow, `UDP stats:` once per
  second gives `rxDatagrams`, `rxBytes`, `emptyDatagrams`, `drains`, `largestDrain`
  (datagrams read per pass), `writeCalls`, `writeAttempts` (above `writeCalls` means
  packets go to more than one destination), `txDatagrams`, `txBytes`, `learnedTargets`,
  `writeFailures`, `largestArrivalGapMs` (longest gap between received datagrams in that
  second).
- **Main thread stalls** (`Custom.MainThreadStall`): `Main thread stalled for N ms` when
  the main thread runs more than 250 ms late, at most once per second. One line right
  after startup is normal.

### Capturing a slow transfer

1. Before connecting, open Application Settings > App Log Viewer > Categories and enable
   `Vehicle.FTPManager`, `Comms.UDPLink` and `Custom.MainThreadStall`.
2. Note which links are connected and which is Primary. Run the transfer over UDP and
   note the time, then over serial.
3. Open Settings > Telemetry > Link Status and note loss per link.
4. On Windows, run `netstat -s -p udp` before and after. A rising "Receive Errors" count
   means Windows dropped datagrams.
5. Click Save on App Log Viewer to save the log.

### Changes to know about

- **UDP receive buffer**: memory is used only while datagrams are queued.
- **FTP timing**: unchanged. A lost packet still costs a 3 second wait, with 6 retries.
  No setting was added or moved.

### Known issues

- **Return guard**: only visible parameter, mission, fence and rally transfers
  and pending MAVLink signing changes delay return. Individual parameter reads,
  arm, takeoff and guided commands are not tracked, so a return can coincide with them.
- **Wait history**: fully removing a vehicle forgets its return-wait history.
- **Video handover**: after switching between a vehicle tab and the COP overview,
  a tile can show "Stream unavailable; retrying" for a few seconds while the other
  receiver releases the UDP port.
- **3D View test**: the stock test needs a 3D View button the fork does not show.
  It remains excluded in CI.
- **Hardware checks**: video and Plan UI tests have not run on real hardware.
  The return logic and new status line have only been exercised against simulated
  links, not real radios.
- **Slow UDP transfers**: the cause is not established; this release adds measurement
  and the buffer change, not a speed fix.
- **UDP send targets**: every endpoint the link has received from still gets every
  outgoing packet, and none expire.
- **FTP loss recovery**: loss still costs 3 seconds; pieces are re-requested one by one.

## v1.12.1

**Test build.** The changes pass the automated Unit and Integration suites, but
the return-to-preferred-link logic has not yet been bench-tested on the real
vehicles and radios. Treat v1.12.1 as a test build until the owner has accepted it
on the bench.

Based on v1.12.0 plus the changes below, with no upstream merge. Every v1.12.0
behavior is kept except where Changes to know about says otherwise.

### Fixes in the fork

Bench follow-up to the first multilink test:

- **Return wait**: now 60 seconds by default, up from 15. The 5 to 300 second range
  is unchanged, and a wait saved earlier is kept. Repeated drops still lengthen
  the wait: with the default setting, 60, 120, 240, then up to 300 seconds.
- **Return after connecting**: connecting while armed or a failed parameter
  download no longer blocks return for good. The return used to wait for a
  parameter load that would never happen.
- **Return status**: each vehicle with two or more links shows a line on Vehicle
  Links: automatic return off, a countdown, paused for a transfer or while the
  vehicle finishes connecting, or a longer wait after repeated drops. Each state
  change is also logged under `Custom.LinkPreference`.
- **Vehicle Links location**: moved from Analyze to Application Settings, directly
  under Comm Links. Comm Links itself is unchanged.

### Changes to know about

- **Preferred link always wins**: picking another link by hand no longer holds
  it. In v1.12.0, a hand-picked link stayed selected until the preferred link
  dropped and came back. Leaving a healthy preferred link now starts the full
  return wait from that moment, with the status line counting down; it does not
  snap back instantly. The configured link must not be overridable by hand, so
  the system stays predictable.
- **Vehicle Links**: now under Application Settings, not Analyze.

### Known issues

- **Return guard**: only visible parameter, mission, fence and rally transfers
  and pending MAVLink signing changes delay return. Individual parameter reads,
  arm, takeoff and guided commands are not tracked, so a return can coincide with them.
- **Wait history**: fully removing a vehicle forgets its return-wait history.
- **Video handover**: after switching between a vehicle tab and the COP overview,
  a tile can show "Stream unavailable; retrying" for a few seconds while the other
  receiver releases the UDP port.
- **3D View test**: the stock test needs a 3D View button the fork does not show.
  It remains excluded in CI.
- **Hardware checks**: video and Plan UI tests have not run on real hardware.
  The return logic and new status line have only been exercised against simulated
  links, not real radios.

## v1.12.0

**Test build.** The merged upstream work and new features pass the automated Unit
and Integration suites, but none has been bench-tested on the real vehicles and
radios yet. Treat v1.12.0 as a test build until the owner has accepted it on the bench.

Brings the fork up to upstream QGroundControl master (6eb58f08), 151 commits after
the v1.11.0 base (b837ccd), and keeps every fork behavior from v1.11.0.

### New in the fork

- **Prefer this link**: mark a vehicle's link with the "Preferred" checkbox on the
  Vehicle Links page. Nothing changes until a link is marked. First automatic
  selection, at startup or when attached later, needs 6 seconds of steady
  communication; after failover, the configured wait applies (15 seconds by default).
- **Return wait**: change "Wait before returning (seconds)" from 5 to 300 seconds.
  Repeated losses within a minute of returning double the wait, up to 5 minutes.
  A link picked by hand stays selected until the preferred link loses and regains communication again.
- **Transfers first**: automatic return waits for visible parameter, mission, fence
  and rally transfers and pending MAVLink signing changes to finish.
  See the guard limitations under Known issues.
- **Link status**: each link shows "Primary", "Comm lost" or "Not connected to this
  vehicle" when applicable. An automatic return announces
  `"<vehicle>: communication back on <link>"`.
- **Vehicles and links together**: set up vehicle types, nicknames and their links
  on one Vehicle Links page (#50).
- **Upload recovery**: mission, fence and rally uploads recover from lost messages,
  with up to 5 restarts per upload. ArduPilot restarts the upload; PX4 resends the
  last mission item when the final acknowledgement is lost.
- **Exclusion zones**: zones are read back from the vehicle and compared before
  success is reported.
- **COP video**: the overview uses the new video receivers, one per vehicle.
  A failed stream retries on its own, and a duplicate UDP video port is refused.
- **Vehicle labels and confirmations**: COP tabs read "type - nickname".
  Emergency Stop and plan removal confirmations stay with the vehicle they were opened for.
- **Larger system IDs**: IDs up to 4294967295 work everywhere the fork stores or shows them.

### New from upstream

- **Window and controls**: updated to Qt 6.12, with a new window palette and restyled controls.
- **Plan panel**: tabs replace the tree view. Survey lines split at concave polygon
  boundaries, rally points support altitude reference choices and terrain altitude,
  and mission estimates include loiter turns.
- **Experimental Plan map**: the GeoMap Plan map is an opt-in setting, off by default.
- **Main status**: the indicator has tabs, severity labels, acknowledgement and
  read/unread messages. It also shows when a reboot is required or an update is
  available. A checklist is available before connecting a vehicle.
- **Guided actions**: available actions follow the vehicle firmware's readiness.
  Emergency Stop uses the guided action confirmation.
- **Video**: the new video system adds stream statistics, a Qt Multimedia receiver,
  USB cameras, D3D12 support and recording services.
- **Spoken status text**: Linux uses a system-installed Piper for speech.
- **GPS and corrections**: GPS receivers and NTRIP corrections have been reworked.
- **Vehicle IDs and logs**: MAVLink supports 32-bit system IDs. Onboard log transfers
  are blocked while the vehicle is armed.
- **PX4 setup**: redesigned setup pages cover ports, failsafes, batteries, flight
  modes, airframes, sensors and more. These changes only affect PX4 vehicles.

### Changes to know about

- **First connection**: the parameter cache moved to the application cache folder.
  Each vehicle downloads its parameters in full the first time it connects after the update.
- **Video settings**: changing the video backend or decoder while COP video is
  showing applies after switching to a vehicle tab and back.
- **Custom colors**: custom code written against the old palette roles must use the new ones.
- **Builds**: Linux and Windows builds are automatic. Android, iOS and macOS builds stay manual.
- **Releases**: the release flow is unchanged: start it by hand on master.
  Breaking changes count as a minor release.

### Behavior kept from v1.11.0

- Every v1.11.0 fork behavior is kept, including vehicle-specific plans in Fly View,
  several vehicles' plans on the COP overview, plain-language messages and distinct nicknames.

### Known issues

- **Return guard**: individual parameter reads, arm, takeoff and guided commands
  are not tracked, so a return can coincide with them.
- **Wait history**: fully removing a vehicle forgets its return-wait history.
- **Video handover**: after switching between a vehicle tab and the COP overview,
  a tile can show "Stream unavailable; retrying" for a few seconds while the other
  receiver releases the UDP port.
- **3D View test**: `Viewer3DUITest` fails in custom builds because the fork tool
  strip has no 3D View button. It is excluded in CI.
- **Hardware checks**: the video, link and Plan UI tests have not run on real hardware.

## v1.11.0

Brings the fork up to upstream QGroundControl master (b837ccd) and keeps every
fork behavior from v1.10.0.

### New from upstream

- **Fly View map position**: the Fly View map position is saved and shared with Plan view.
- **Orbit and loiter radius editing**: edit the radius in both the 2D and 3D map.
- **Log viewer**: ULog array fields can be plotted, and chart interaction is fixed.
- **Spoken status text**: acronyms in vehicle status text are pronounced correctly.
- **Mission commands**: commands that were lost to broken enum translations are restored.
- **MAVLink enums in QML**: enum values are exposed to QML through `MAVLinkEnums`.
- **FTP**: paths are preserved when the `mftp://` scheme is removed.
- **Scripting page**: download and delete icons are centered on the selected script.
- **Terrain in 3D**: terrain cliffs are removed and items sit on the drawn mesh.

### Changes to know about

- **Critical vehicle message popup**: it no longer steals keyboard focus. Escape no
  longer dismisses it. Click outside the popup or use its acknowledge button.
- **FTP directory listing**: the fork keeps its broader fallback to plain listings.
  A generic failure on a timed listing still falls back, as in v1.10.0.
- **Builds**: Android, iOS and macOS builds are no longer produced automatically.
  Linux and Windows builds are unchanged. The three workflows can still be started
  by hand from the Actions tab.

### Behavior kept from v1.10.0

- The Fly View for a vehicle shows only that vehicle's plan. The COP overview can still
  overlay several vehicles' plans.
- Plain-language operator messages and distinct vehicle nicknames.

### Known issues

- Two automated tests still fail on slow CI runners and are tracked for follow-up:
  `Viewer3DUITest` (the fork tool strip has no 3D View button) and
  `RequestMetaDataTypeStateMachineTest` (FTP busy during the parameter download).
  Neither affects flight behavior.
