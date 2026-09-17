# COP and dynamic vehicle-tab adversarial review

This pass records **6 findings: 4 source-level defects or requirement violations and 2 false alarms ruled out by inspection**. None is a runtime-confirmed failure: the sandbox cannot become ubuntu, the application refuses root, and the configured build is not writable by the available identity. The four issues are an unrelated disconnect changing the controlled vehicle, forgotten video overrides, silent loss of unacknowledged notifications, and a superseded control request winning a race. The notification issue conflicts with the requested no-loss contract but matches the feature's documented 200-message history; the race is demonstrated at the controller API level, with physical-click reachability unverified. **Confidence in operational readiness is low; this is an incomplete runtime stress pass, not a safety or stability sign-off.**

## Execution evidence and scope

Branch inspected: `claude/gui-cop-update`, already checked out. Production code was not changed. New repro sources, their CMake registration, and this report remain uncommitted. No staging, commits, pushes, branch changes, or upstream remote operations were attempted.

The following are actual results, not predicted test outcomes:

- `sudo -u ubuntu -H bash -c '...'` failed with `setresuid(-1, 1, -1): Invalid argument` and sudoers/audit initialization errors.
- `runuser -u ubuntu -- id` failed with `cannot set groups: Operation not permitted`.
- `setpriv --reuid=1000 --regid=1000 --clear-groups id` failed with `setresuid failed: Invalid argument`.
- `/proc/self/uid_map` contains only `0 0 1`; the requested ubuntu identity is not mapped in this sandbox.
- Direct execution of `build/Debug/Custom-QGroundControl --unittest:COPControllerTest --allow-multiple`, with offscreen/software Qt, exited 255: `You are running Custom-QGroundControl as root ... will now exit.` No test method ran. The guard was not bypassed.
- `JOBS=2 just build` reached Ninja and failed: `Error writing to build log: Permission denied`. Existing build artifacts appear as `nobody:nogroup` here. Ownership and existing build artifacts were not changed to evade this failure.
- `just lint` failed while fetching the missing pre-commit-hooks environment: network connection to the configured proxy was unavailable. Its cache/log were directed inside the repository.
- The new test translation unit passed `-fsyntax-only` and compiled to an object using the existing configured compiler flags. Qt moc generation and compilation of its generated source also passed. Logs/objects are under `build/cop-stress-validation/`. Targeted clang-format, cmake-format, and `git diff --check` passed. Full application build/link, CTest execution, the final Unit label sweep, sanitizers, UI rendering, and live-video playback were not validated.

### Harness precedent and new coverage

MockLink implementation is under `src/Comms/MockLink/` in this checkout. Existing tests live under `test/Comms/`, but there is no `test/Comms/MockLink/` implementation directory. `VehicleLinkManagerTest::_startMockLink` (`test/Vehicle/VehicleLinkManagerTest.cc:388`) supplies the shared-configuration ownership and `createConnectedLink` pattern, including simultaneous links. Its two-link tests deliberately use one sysid for one vehicle. The new tests enable ID increment for distinct vehicles and wait on the vehicle model/per-vehicle readiness, rather than expecting `activeVehicleChanged` for every connection. `InitialConnectTest` and `MultiVehicleManagerTest` were also inspected; neither supplies the requested three-vehicle UI fixture.

`COPStressUITest::_threeVehicleChurn` uses existing MockLinks for ArduPilot ground rover, quadrotor labelled Hex (the requested “or similar” multirotor), and fixed wing labelled Stallion. It assigns their actual generated sysids to VehicleRoleController. A 1 ms event-loop timer alternates COP/tab selections during connection and deletion; separate real QmlUITestBase mouse clicks exercise COP, Hex, disconnected Stallion, and Assume Control. The timer calls the production selection API, so it is not evidence of human mouse-click timing.

The test checks per-entry coordinates against vehicle coordinates, initial connection, retained entry identity, retained tab count, removal of QPointers after physical disconnect, an extra unrecognised sysid's `Vehicle N` label, and disconnects in a different order. Stallion is created with `incrementVehicleId=false`, following the existing COP lifecycle test's same-ID reconnect technique, while Rover and Hex consume distinct IDs. Stallion reconnects while control is pending and is disconnected from `vehicleAdded`, before deferred activation completes. It then reconnects again. This is an adversarial repro awaiting execution, not proof that the race is safe. Reconnect permutations for every role and rendered map-marker positions remain untested.

Optional video mode uses the **existing MockLink video server**, not a new simulator. MockLink hardcodes ports by stream type (`src/Comms/MockLink/MockLink.cc:335`): Rover uses H.264 UDP/5600, Hex H.265 UDP/5601, and Stallion RTSP/8554. The test requires three decoding sessions before killing Stallion's link and then requires two. Without `QGC_TEST_ENABLE_GSTREAMER`, the backend is deliberately disabled (`src/VideoManager/VideoReceiver/VideoBackend.cc:84`); a normal UI test run cannot establish live-receiver teardown safety. H.265/RTSP plugins and a functioning graphics backend are prerequisites, not simulated successes. Decoding counts alone also do not establish visual feed identity or uninterrupted frame delivery.

No rescue-mission implementation was found by searching `src/` and `custom/` for `rescue` in this checkout. The COP rover relay timer is at `custom/src/COP/COPController.cc:190`; rover manual-takeover warning dialogs are in `custom/src/CustomGuidedActionsController.qml`. The notification repro injects through the actual `operatorNotification` signal. It does **not** claim to have executed those upstream advisory/takeover flows or waited through the real 60-second relay timer.

## Finding 1 — Disconnecting Stallion takes active control away from Hex

**Severity: High. Status: source-confirmed logic defect; runtime repro not executed.**

**Exact repro:**

1. Connect Rover first, Hex second, Stallion third, each with its own ArduPilot MockLink/sysid.
2. Select Hex and press Assume Control; wait until Hex is the active vehicle.
3. Physically disconnect only Stallion's MockLink, leaving Rover and Hex connected.
4. After Stallion is destroyed, inspect the active vehicle and selected COP tab.

**Observed in source vs expected:** `_deleteVehiclePhase2` always assigns the first remaining vehicle, Rover, as active, regardless of which vehicle disconnected. The selected COP sysid remains Hex, so its normal FlyView becomes covered by the “connected, not active” page. Losing an unrelated link should preserve the operator's active Hex selection. No command being sent to the wrong aircraft was observed; the active-target change itself is the defect.

**Root cause:** `src/Vehicle/MultiVehicleManager.cc:202` selects model index 0 unconditionally; line 207 installs it. Phase 1 also clears active/parameter-ready availability unconditionally at line 169. COP delegates control to this manager at `custom/src/COP/COPController.cc:261`; `custom/src/COP/COPPage.qml:16` exposes the resulting selected/active mismatch. This is a defect in the existing manager used by COP, not an assertion that the manager logic originated in this branch.

**Regression repro added:** `COPStressUITest::_disconnectOtherPreservesControl`, in `COPStressTest.cc`. It waits for the removed Vehicle's QPointer to clear, then asserts Hex is still active. Expected to fail on this source; not runtime-confirmed.

## Finding 2 — Saved COP video URI overrides disappear on restart

**Severity: Medium. Status: source-confirmed defect; runtime repro not executed.**

**Exact repro:**

1. Connect a vehicle and enter `udp://0.0.0.0:5643` in its COP tile; finish editing.
2. Close and restart the app with the same settings.
3. Reconnect the same sysid and inspect its URI field/feed source.
4. For the mechanised repro, construct a fresh COPController with the same QSettings and role mapping after destroying the first controller.

**Observed in source vs expected:** The setter writes `COP/Video/<sysid>` to QSettings, but the constructor and restoration path never read that key. The fresh entry's URI is empty. Video therefore falls back to an advertised stream, or remains unavailable when none is advertised. The feature README explicitly promises that video addresses survive restarts; the override should be restored.

**Root cause:** `custom/src/COP/COPController.cc:28` leaves `_videoUri` empty; line 41 saves it; `_remember` at line 118 and `initialize` at line 136 restore only sysid order. A search of the custom source found no reader of the saved video key.

**Regression repro added:** `COPStressTest::_videoOverrideSurvivesRestart`. It first verifies that the settings key was saved, then compares the restored entry's URI with the original. Expected empty-string mismatch; not an observed test failure.

## Finding 3 — Burst notifications silently evict unacknowledged messages

**Severity: Medium. Status: source-confirmed violation of this task's no-loss requirement; documented feature behaviour.**

**Exact repro:**

1. Leave the persistent notification panel unacknowledged.
2. Emit 205 distinct messages through `QGCCorePlugin::operatorNotification` in succession.
3. Inspect the retained message count and oldest message before clicking Acknowledge.

**Observed in source vs expected:** Only the newest 200 messages remain; messages 0–4 have been deleted without acknowledgement. There is no overflow counter or durable queue in COP. An early relay/takeover warning can be displaced by subsequent status text. The requested no-drop contract requires preserving outstanding messages or at least making loss explicit while keeping resource use bounded.

**Root cause:** `custom/src/COP/COPController.cc:279` unconditionally removes the oldest entry once size exceeds 200. A single `_unacknowledged` flag cannot distinguish acknowledged history from unseen outstanding messages. The panel exposes only the retained count (`custom/src/COP/COPNotifications.qml:23`).

**Qualification:** `custom/src/COP/README.md` explicitly documents a latest-200 history, and the existing `_notificationQueue` test expects it. This is not an accidental off-by-one or proof of an unbounded queue. It is a product-contract conflict requiring triage; if deliberate lossy history is accepted, reclassify accordingly.

**Regression repro added:** `COPStressTest::_unacknowledgedBurstIsNotDiscarded`, using the real core-plugin notification signal and muted audio. It intentionally expects 205 retained outstanding messages. Expected to fail with 200; not executed. No claim is made about panel crashes or orange-border rendering under load.

## Finding 4 — A superseded Assume Control request can win

**Severity: Medium. Status: source-confirmed controller/API race; physical UI reachability unverified.**

**Exact mechanised repro:**

1. Connect A (Rover) and B (Hex); wait until A is active.
2. Call `selectVehicle(B)`, then `assumeControl()`.
3. Before returning to the event loop, call `selectVehicle(A)`, then `assumeControl()`.
4. Wait for the deferred active-vehicle change and compare it with the latest requested target A.

**Observed in source vs expected:** The request for B schedules a 20 ms callback. The following request for A is discarded because A is still the current active vehicle. B's older callback subsequently activates B. A newer explicit control request should supersede an older queued request. The pending-control property stays zero for both connected targets, so COP does not represent this transaction.

**Root cause:** `custom/src/COP/COPController.cc:259` clears pending state for a connected target and forwards requests without serialization. `src/Vehicle/MultiVehicleManager.cc:223` compares only with the committed active pointer, while line 234 queues each switch independently. There is no latest-request token/cancellation in that path. As with Finding 1, the manager behaviour predates this review; branch attribution was not established.

**Reachability limitation:** `custom/src/COP/COPNavigation.qml:83` disables Assume Control for the currently active target, so the exact second call is not established as a possible physical button click within 20 ms. This test attacks the public QML-invokable controller API. It does not establish a field-reproducible wrong-aircraft command or a crash.

**Regression repro added:** `COPStressUITest::_lastControlRequestWins`. It boots the real UI and uses two MockLinks, then invokes the controller in one event-loop turn. Expected final-target mismatch; not executed.

## Finding 5 — One click clearing orange is bulk acknowledgement, not first-message acknowledgement

**Severity: Not-a-bug. Status: suspected semantic problem ruled out by source inspection.**

**Exact review/repro sequence:** Deliver three messages, invoke Acknowledge once, then deliver a fourth. Inspect the controller's boolean and panel binding after each step.

**Observed in source vs expected:** Acknowledge is a global action, not a per-row action. It clears the outstanding flag for the whole current batch and keeps history. Any subsequent notification sets it again. Thus one click returning the border to normal does not establish that only the first message was acknowledged. The UI has no “acknowledge first message” operation. This does not excuse the silent eviction in Finding 3.

**Relevant code:** `custom/src/COP/COPController.cc:287`; new messages set the flag at line 282. The single button and border binding are at `custom/src/COP/COPNotifications.qml:24` and line 14.

**Regression test:** No redundant new test added. Existing `COPControllerTest::_notificationQueue` covers acknowledgement followed by a new notification. Neither that test nor the rendered panel was executed in this sandbox.

## Finding 6 — Repeated sysids do not create an unbounded number of COP entries

**Severity: Not-a-bug. Status: suspected entry-count growth ruled out by source inspection.**

**Exact review/repro sequence:** Repeatedly reconnect one valid sysid, then introduce distinct valid sysids beyond the three role names. Inspect entry count and fallback labels. Repeat a previously remembered sysid.

**Observed in source vs expected:** `_remember` deduplicates by sysid and rejects IDs outside 1–255. There can be at most 255 COP entries, including retained disconnected entries. Unknown roles fall back to `Vehicle <sysid>`. Retaining a disconnected tab is an explicit feature, not itself a leak. This conclusion is only about entry count: 255 video-session timers/delegates can still be expensive, and notification strings/audio work were not profiled.

**Relevant code:** `custom/src/COP/COPController.cc:110` validates/deduplicates entries; line 45 supplies fallback labels. `custom/src/COP/COPVideoGrid.qml:37` keeps delegates but hides disconnected tiles. Session teardown begins at `custom/src/COP/COPVideoSession.cc:96`.

**Regression coverage added:** `_threeVehicleChurn` checks one extra fallback tab, stable same-sysid entry identity, and retained tab count after disconnects. It does not exhaust all 255 IDs and was not executed. No dangling-pointer, leak-free, or live-video isolation guarantee follows from this inspection.

## Reproduction commands after the environment is repaired

Run the normal build and CTest workflow as ubuntu in a host where that user is mapped and owns the configured build. Keep test caches/settings/temp paths inside the working tree if preserving this task's filesystem constraint. CTest already assigns test-specific TMPDIR paths.

```bash
cd /home/user/qgroundcontrol
JOBS=2 just build
ctest --test-dir build --output-on-failure -R '^COPStress(Test|UITest)$'
```

The two unit repros intentionally assert currently unmet contracts. The UI suite includes expected-failure control tests and a lifecycle probe whose outcome is unknown. These are triage repros, not a passing merge gate. To isolate the lifecycle method, use the existing binary method-filter mechanism after rebuilding:

```bash
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  build/Debug/Custom-QGroundControl --unittest:COPStressUITest \
  --allow-multiple -- _threeVehicleChurn
```

For live video, use the existing CI-style xvfb/OpenGL setup and enable GStreamer explicitly; do not leave the software Quick backend forced:

```bash
env -u QT_QUICK_BACKEND QGC_TEST_ENABLE_GSTREAMER=1 \
  QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=opengl LIBGL_ALWAYS_SOFTWARE=1 \
  xvfb-run -a build/Debug/Custom-QGroundControl \
  --unittest:COPStressUITest --allow-multiple -- _threeVehicleChurn
```

These follow-up commands were **not** successfully run here. Required remaining work includes actual UI execution, rendered map/video identity checks, repeated reconnect permutations, real relay/takeover notification floods, sanitizer-assisted lifetime testing, and the normal build/lint/Unit sweep. Without those results, crashes, hangs, stale rendering, and receiver isolation remain open questions.
