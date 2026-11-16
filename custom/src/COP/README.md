# Common Operating Picture

The custom plugin supplies navigation, a COP overlay, and a notification footer through
optional `QGCCorePlugin` URL hooks. Stock builds leave these URLs empty. The main window
passes itself to each loaded item's `hostWindow` property; navigation uses its existing
validation guard and view-switch functions.

COP is the landing page. Vehicle tabs retain first-seen order in `QSettings` and include
saved vehicle entries (Application Settings > Vehicle Links). A role change updates existing labels.
Selecting a tab does not change the active vehicle. **Assume Control** selects a connected
vehicle, or remembers one system ID to select when it connects. The pending request can be
cancelled. This action changes QGC's active vehicle; it does not arm, change flight mode,
or transfer radio authority.

Disconnected or inactive tabs show the last received position, flight mode, and update time.
The normal FlyView is exposed only when that tab's vehicle is connected and active. These
telemetry snapshots last for the application session; tab order and video addresses persist
across restarts. A system ID must identify the same physical vehicle throughout a session.

## Video

Each connected vehicle has its own `VideoReceiver` and output sink. The session uses that
vehicle's advertised camera stream, or the saved URI entered in its tile. URI overrides
also supply the stock primary preview while that vehicle is active. They take priority over
advertised streams and manual Video settings, without changing vehicle parameters or any
stock Video settings. An empty URI preserves stock preview behaviour: the advertised stream,
then the manual Video settings. The thermal stream is unchanged.
Use distinct endpoints, such as `udp://0.0.0.0:5600` and `udp://0.0.0.0:5601`, or distinct
RTSP URLs. Duplicate endpoints are rejected instead of displaying one feed under two labels.
No synthetic feed is substituted when a vehicle has not advertised a stream.

The existing video backend initializes asynchronously. Receivers are stopped when their
vehicle disconnects, their tile becomes hidden, or the operator leaves COP. Receiver
workers finish before their sinks are released. A default-enabled core-plugin hook keeps
the standard receivers stopped while the COP overview owns the endpoints. Leaving the
overview disables tile sessions and releases their endpoints before standard playback starts.

## Map and notifications

The map reuses `FlightMap` and displays connected vehicles with valid coordinates. The
reference marker comes from validated RTCM 1005/1006 antenna-reference ECEF coordinates
observed on the existing correction router's selected source, including NTRIP. It is not
an inferred vehicle position or proof of an RTK fix. A source/session change clears it;
30 seconds without routed correction traffic expires it. Until a station-position message
arrives, no reference marker is shown. Field layout was checked against
[RTKLIB's RTCM decoder](https://github.com/tomojitakasu/RTKLIB/blob/master/src/rtcm3.c).

A vehicle tab's **Plan Overlay** toggle draws that vehicle's mission, fences and rally points on
the map in the vehicle's color, and a legend explains the symbols. Line style tells the layers
apart: the route is a solid line with waypoint dots (S start, E end), a keep-in fence is a
long-dash outline, a keep-out zone is a short-dash outline over a red fill, and a rally point
is a diamond. Qt Location polygons and circles cannot dash their borders, so
`COPBoundaryOutline.qml` draws each outline as a `Shape` anchored at the boundary's first
vertex and re-projects it when the map zooms or resizes. The map polygons and circles remain
for the fill and for **Fit vehicles**.

The footer retains up to 200 acknowledged messages and up to 1,000 outstanding messages
with timestamps. If outstanding messages exceed that limit, it displays the number omitted
until the operator acknowledges them. Emergency/alert vehicle status text, communication
loss/disconnection, the rover relay advisory, and explicit operator notifications enter this
queue. Routine health/status messages and general application dialogs stay in their existing
vehicle message displays and dialogs; link restoration does not add a notification.
New messages request an AudioOutput alert, respecting QGC's audio mute and volume settings,
and turn the border orange. Acknowledgement clears the highlight and retains the latest
200 messages as read history.
Existing confirmation dialogs retain their original behavior.

## Serial connection notices

Active serial connection notices sit above the timestamped history in their own scroll area.
Each row identifies the link/device and port and wraps the core notice's explanation and remedy.
New actionable transitions expand the footer, turn its border orange and request the same audio
alert as other operator notifications. Retries and resolution are silent. Notices already present
when the footer loads appear immediately, without replaying audio.

**Open Comm Links** opens Application Settings > Comm Links. A verified GNSS owner also offers
**Open GNSS Receiver**, which opens that settings page. Both respect the main window's navigation
validation; opening a page does not acknowledge a notice or change connection policy.
**Dismiss** hides that transition by acknowledging it; it does not stop retries or fix the link.
A different blocker or escalation can show the same incident again. The footer stays expanded
while any active notice is unacknowledged, even after ordinary messages are acknowledged or
history overflows. Buttons wrap onto another line when needed.

A row disappears when dismissed, resolved by the core model (verified serial connection or
applicable policy correction), explicitly disconnected, or its configuration is removed.
Discovery rows also disappear when the device detaches; a saved-link loss can remain while
the device is absent. Resolved serial incidents remain in the core's separate 32-entry history;
the footer does not add restoration messages to the ordinary history or claim telemetry recovery.

A physical serial disconnection retains the ordinary vehicle-disconnected message about last
known state, silently: the serial notice supplies the recovery alert and actions, including the
core's delayed escalation when automatic retry is eligible. Heartbeat loss while the serial port
is still open instead advises checking vehicle power and the radio link, without promising
unconditional reconnection. Non-serial vehicle loss and disconnection alerts are unchanged.

## Verification

`COPControllerTest` covers role labels, persistent ordering, selection and pending control,
notification bounds/acknowledgement, and valid/corrupt RTCM station coordinates.
`COPVehicleLifecycleTest` covers a MockLink disconnect/reconnect with retained state and a
pending control request. Both are compiled only in test builds and registered from `custom/`.

`COPSerialNoticesUITest` covers the rendered reason/action catalogue, guarded settings navigation,
startup notices, dismissal, transition audio requests, independent history bounds, minimisation,
multiple devices, resolution, real Linux PTY open failures/recovery, serial vehicle loss coupling,
and the empty stock footer hook. It is registered with Integration, Comms, QML and NoSanitizer labels.

Hardware validation should include two distinct simultaneous feeds, switching between COP
and FlyView, link loss/reconnection, and an actual NTRIP stream containing station coordinates.
