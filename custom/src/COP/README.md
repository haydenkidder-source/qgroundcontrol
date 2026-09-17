# Common Operating Picture

The custom plugin supplies navigation, a COP overlay, and a notification footer through
optional `QGCCorePlugin` URL hooks. Stock builds leave these URLs empty. The main window
passes itself to each loaded item's `hostWindow` property; navigation uses its existing
validation guard and view-switch functions.

COP is the landing page. Vehicle tabs retain first-seen order in `QSettings` and include
saved Vehicle Roles entries. A role change updates existing labels. Selecting a tab does
not change the active vehicle. **Assume Control** selects a connected vehicle, or remembers
one system ID to select when it connects. The pending request can be cancelled. This action
changes QGC's active vehicle; it does not arm, change flight mode, or transfer radio authority.

Disconnected or inactive tabs show the last received position, flight mode, and update time.
The normal FlyView is exposed only when that tab's vehicle is connected and active. These
telemetry snapshots last for the application session; tab order and video addresses persist
across restarts. A system ID must identify the same physical vehicle throughout a session.

## Video

Each connected vehicle has its own `VideoReceiver` and output sink. The session uses that
vehicle's advertised camera stream, or the saved URI entered in its tile. URI overrides
are COP display settings and do not change vehicle parameters or FlyView video settings.
Use distinct endpoints, such as `udp://0.0.0.0:5600` and `udp://0.0.0.0:5601`, or distinct
RTSP URLs. Duplicate endpoints are rejected instead of displaying one feed under two labels.
No synthetic feed is substituted when a vehicle has not advertised a stream.

The existing video backend initializes asynchronously. Receivers are stopped when their
vehicle disconnects, their tile becomes hidden, or the operator leaves COP. Receiver
workers finish before their sinks are released. A default-enabled core-plugin startup
hook prevents the standard FlyView receiver from competing for COP's UDP endpoints.

## Map and notifications

The map reuses `FlightMap` and displays connected vehicles with valid coordinates. The
reference marker comes from validated RTCM 1005/1006 antenna-reference ECEF coordinates
observed on the existing correction router's selected source, including NTRIP. It is not
an inferred vehicle position or proof of an RTK fix. A source/session change clears it;
30 seconds without routed correction traffic expires it. Until a station-position message
arrives, no reference marker is shown. Field layout was checked against
[RTKLIB's RTCM decoder](https://github.com/tomojitakasu/RTKLIB/blob/master/src/rtcm3.c).

The footer retains up to 200 acknowledged messages and up to 1,000 outstanding messages
with timestamps. If outstanding messages exceed that limit, it displays the number omitted
until the operator acknowledges them. Vehicle status text,
link loss/restoration, the rover relay advisory, and application messages enter this queue.
New messages request an AudioOutput alert, respecting QGC's audio mute and volume settings,
and turn the border orange. Acknowledgement clears the highlight and retains the latest
200 messages as read history.
Existing confirmation dialogs retain their original behavior.

## Verification

`COPControllerTest` covers role labels, persistent ordering, selection and pending control,
notification bounds/acknowledgement, and valid/corrupt RTCM station coordinates.
`COPVehicleLifecycleTest` covers a MockLink disconnect/reconnect with retained state and a
pending control request. Both are compiled only in test builds and registered from `custom/`.

Hardware validation should include two distinct simultaneous feeds, switching between COP
and FlyView, link loss/reconnection, and an actual NTRIP stream containing station coordinates.
