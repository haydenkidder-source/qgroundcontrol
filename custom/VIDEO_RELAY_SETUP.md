# Video relay setup

## Status

The following results were verified on the bench on **2026-11-14**, using one GCS Pi and
one vehicle Pi, both Raspberry Pi 5 running Raspberry Pi OS 64-bit. Here, verified covers
installation, unattended service startup, relay availability, external RTSP playback and bench
picture quality; it does not establish field performance or RTSP playback inside
QGroundControl (QGC).

**Verified:**

- Both installers ran. The GCS installer fetched MediaMTX v1.21.3, the latest release at the time.
- After reboot, `mediamtx` started by itself on the GCS Pi, bound User Datagram Protocol (UDP) ports
  5602-5604 and served RTSP on 8554, with HLS on 8888 and WebRTC on 8889.
- `vehicle-stream` started by itself at boot. Its pipeline reached PLAYING about 3 seconds
  after boot and used about 20% of one core at 720p10 with a 1 Mbit/s target bitrate.
- After both Pis rebooted, both services came up unattended. About 2 seconds after the vehicle
  service started, MediaMTX logged `stream is available and online, 1 track (H264)` for its path.
- RTSP readers connected using Transmission Control Protocol (TCP), locally on the GCS Pi
  and from a Windows laptop.
  Laptop playback worked with at least two different players; the exact players were not
  recorded. GCS Pi CPU use stayed under about 15%.
- After the radio power fix, picture quality was judged reasonable on the bench, with
  occasional frames that are half grey. Tuning was deliberately not done for this test.

**Not verified:**

- Hardware RTSP playback inside the custom QGC build remains untested; code review indicates
  RTSP URLs are supported (see [Pointing QGC at the relay](#pointing-qgc-at-the-relay)).
- Field-distance transmit power and behavior with more than one vehicle.
- Browser playback over HLS or WebRTC; listening ports alone do not verify playback.
- The new `TimeoutStopSec=10` directive in the GCS unit; it has not yet been exercised on hardware.

Heavy video artifacts on the bench led to the
[radio power investigation](FIELD_RADIO_SETUP.md#bench-versus-field-transmit-power).
The likely cause of the remaining half-grey frames (not measured) is a lost packet mid-frame,
leaving the rest of the frame grey until the next keyframe, which arrives within 0.5 s at the
documented settings. The configured video link has no forward error correction (FEC).
Ideas for later mitigation, not tested here, are reducing loss through correct radio power
for the distance, a shorter keyframe interval, or x264 intra-refresh.

The old direct-to-receiver pipeline used Real-time Transport Protocol (RTP), with
`rtph264pay`. The relay pipeline below uses MPEG transport stream (MPEG-TS), not RTP.

## Video path

The ground control station (GCS) and vehicles use the flat network in
[Network addressing](NETWORK_ADDRESSING.md). Configure addresses and pass the laptop ping sweep
before setting up video. MAVLink is the vehicle telemetry and command protocol; configure its
links using [Field radio setup](FIELD_RADIO_SETUP.md).

A **companion computer** is the Raspberry Pi at the GCS or on a vehicle. GStreamer is the media
pipeline tool on the vehicle Pi. It decodes Motion JPEG (MJPEG) from a USB webcam, encodes
low-latency H.264 baseline video with the x264 software encoder, and sends MPEG-TS over UDP.

```text
Vehicle USB webcam -> vehicle Pi / GStreamer -> Microhard mesh
                  -> GCS Pi 10.0.1.2 / MediaMTX -> operator viewers
```

MediaMTX is a small media server on the GCS Pi. In this configuration it listens on a separate
UDP port for each vehicle and republishes each stream under a **path**, a name such as
`vehicleA`. Only one copy of each stream crosses the radio link, however many viewers there
are. Vehicles send to the relay and never manage client connections.

The vehicle in unit `N` sends to `10.0.1.2` on UDP `5600 + N`. For example, Vehicle A (unit 2)
uses 5602. Units 3 and 4 use 5603 and 5604. Unit 1 is the GCS, so no vehicle uses 5601.
These are generic examples only; never share a port between vehicles.

Operators request `rtsp://10.0.1.2:8554/<path>` using Real Time Streaming Protocol (RTSP).
The configuration also enables HTTP Live Streaming (HLS) on port 8888 and browser viewing
through Web Real-Time Communication (WebRTC) on port 8889. See [Status](#status) for the
verified RTSP playback and the remaining checks.

## Installation prerequisites

The installers are [gcs-setup.sh](deploy/video-relay/gcs-setup.sh) and
[vehicle-setup.sh](deploy/video-relay/vehicle-setup.sh) in `custom/deploy/video-relay/`.
They are the source of truth for the configuration and pipeline reproduced below. Copy that
directory to each Pi, or use a checkout on each Pi, and run the appropriate script from it.
See the [installer README](deploy/video-relay/README.md) for usage and prerequisites.

- Both Pis need internet access over Wi-Fi for installation, not over the radio link.
- Both scripts require Bash and root privileges; run them through `sudo` from the account
  that will operate the vehicle camera.
- The GCS script requires 64-bit Raspberry Pi OS (`aarch64`), `curl`, `tar` and standard Linux
  user-management tools. It downloads and installs the latest MediaMTX release from GitHub
  by default. The installed
  version can therefore change between runs. Review the script before running it.
- The vehicle script requires Raspberry Pi OS with `apt-get`, systemd and a USB webcam. It
  installs GStreamer packages and camera tools, and gives the invoking account access to video
  devices. Both Pis need systemd for the boot services.
- The MediaMTX configuration has **no authentication**. It is for bench use; authentication
  and access control must be addressed before field use.

## GCS companion computer

On the GCS Pi, run the installer from the repository root:

```bash
cd custom/deploy/video-relay/
sudo ./gcs-setup.sh
```

The script installs the server at `/opt/mediamtx/mediamtx`, writes the configuration and service,
and enables and starts `mediamtx.service`. It installs the generic configuration below for
units 2-4. An existing configuration is backed up before it is replaced on each run. Adjust
paths and ports for your units, restart the service, and keep any actual mapping internal.

### MediaMTX configuration

Use `/etc/mediamtx/mediamtx.yml`. The three paths below are a worked example for units 2-4.

```yaml
# Vehicle streams arrive as MPEG-TS over UDP on port 5600+<third octet of the vehicle IP>.
# Clients pull from:  rtsp://<this Pi>:8554/<path>
logLevel: info

rtsp: true
rtspAddress: :8554
rtmp: false
srt: false
hls: true       # browser viewing: http://<this Pi>:8888/<path>
webrtc: true    # browser viewing: http://<this Pi>:8889/<path>

paths:
  vehicleA:                    # unit 2
    source: udp+mpegts://0.0.0.0:5602
    sourceOnDemand: false
  vehicleB:                    # unit 3
    source: udp+mpegts://0.0.0.0:5603
    sourceOnDemand: false
  vehicleC:                    # unit 4
    source: udp+mpegts://0.0.0.0:5604
    sourceOnDemand: false
  # To add a vehicle: copy a block above, change the name and port, then
  #   sudo systemctl restart mediamtx
```

To add a vehicle, add one entry under `paths:` with its path name and UDP `5600 + N`, then
restart MediaMTX. Give the vehicle that same destination port. Do not add a second top-level
`paths:` key.

### GCS boot service

The script writes `/etc/systemd/system/mediamtx.service`. systemd is the Linux service manager
that starts the server at boot and restarts it if it exits. The installer creates the dedicated
system account `mediamtx` if needed, and the service runs as that account.

```ini
[Unit]
Description=MediaMTX media server (vehicle video relay)
After=network.target
Wants=network.target

[Service]
User=mediamtx
ExecStart=/opt/mediamtx/mediamtx /etc/mediamtx/mediamtx.yml
Restart=always
RestartSec=2
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
```

The installer now writes `TimeoutStopSec=10` to limit the stop wait to 10 seconds. This
change has not yet been exercised on hardware. For existing installations, see
[Slow MediaMTX stop](#slow-mediamtx-stop) for the unit edit and reload command.

After changing the configuration, restart the service. Check status and follow its log:

```bash
sudo systemctl restart mediamtx.service
systemctl status mediamtx.service
journalctl -u mediamtx.service -f
```

## Vehicle companion computer

On each vehicle Pi, run the installer from the repository root:

```bash
cd custom/deploy/video-relay/
sudo ./vehicle-setup.sh
```

The installer writes the wrapper `/usr/local/bin/vehicle-stream.sh`, the environment file
`/etc/vehicle-stream.env` (if absent), and the unit `vehicle-stream.service`. It enables and
starts the service immediately, using the default destination port 5602 unless an existing
environment file specifies another port. Set the vehicle's own port before using its stream;
never leave two vehicles sending to the same port.

### Vehicle environment file

Edit `/etc/vehicle-stream.env` for the vehicle. The installer preserves this file if it already
exists, so check its contents even after rerunning setup. This example is for Vehicle A (unit 2):

```bash
# Where to send (the GCS Pi) and which port. Port = 5600 + third octet of this vehicle's IP.
DEST_HOST=10.0.1.2
DEST_PORT=5602
# Camera. Use a stable path from:  ls -l /dev/v4l/by-id/
CAM_DEVICE=/dev/video0
# mjpeg or yuyv (see: v4l2-ctl -d /dev/video0 --list-formats-ext)
CAM_FORMAT=mjpeg
WIDTH=1280
HEIGHT=720
FPS=10
# kbit/s
BITRATE=1000
# keyframe every KEYINT frames (5 at 10 fps = every 0.5 s)
KEYINT=5
```

- `DEST_HOST` is the GCS Pi; `DEST_PORT` must be `5600 + N` for this vehicle.
- `CAM_DEVICE` is the camera device. Prefer a stable path from `/dev/v4l/by-id/` when available.
- `CAM_FORMAT` is `mjpeg` or `yuyv`, matching a format the camera supports. MJPEG is compressed
  camera video; YUYV is raw camera video, selected as `YUY2` in the pipeline.
- `WIDTH`, `HEIGHT` and `FPS` set resolution and frames per second.
- `BITRATE` is the encoder bitrate in kilobits per second; 1000 is about 1 megabit per second
  (Mbps).
- `KEYINT` is the maximum interval between keyframes, which are complete reference frames.
  Five frames at 10 fps gives a keyframe every 0.5 seconds.

Inspect camera devices and supported formats on the vehicle Pi:

```bash
ls -l /dev/v4l/by-id/
v4l2-ctl -d /dev/video0 --list-formats-ext
```

### Vehicle pipeline

The script installs this wrapper at `/usr/local/bin/vehicle-stream.sh` and makes it executable.
The service supplies its variables from `/etc/vehicle-stream.env`.

```bash
#!/usr/bin/env bash
set -euo pipefail
case "${CAM_FORMAT:-mjpeg}" in
  mjpeg)
    SRC=(v4l2src device="$CAM_DEVICE"
      ! "image/jpeg,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1"
      ! jpegdec ! videoconvert)
    ;;
  yuyv)
    SRC=(v4l2src device="$CAM_DEVICE"
      ! "video/x-raw,format=YUY2,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1"
      ! videoconvert)
    ;;
  *) echo "CAM_FORMAT must be mjpeg or yuyv" >&2; exit 1 ;;
esac
exec gst-launch-1.0 -e "${SRC[@]}" \
  ! x264enc tune=zerolatency speed-preset=ultrafast bitrate="$BITRATE" key-int-max="$KEYINT" \
  ! "video/x-h264,profile=baseline" \
  ! h264parse config-interval=-1 \
  ! mpegtsmux alignment=7 \
  ! udpsink host="$DEST_HOST" port="$DEST_PORT"
```

This is the installed pipeline. It uses MPEG-TS packets aligned seven per UDP datagram
(1316 bytes) to avoid IP fragmentation. Do not substitute the old `rtph264pay` output for the
MPEG-TS input expected by this relay.

### Vehicle boot service

The installer writes `/etc/systemd/system/vehicle-stream.service`. In this listing only,
`STREAM_USER` is a placeholder for the invoking account from `${SUDO_USER}`; the installer
writes that account into `User=` automatically (falling back to `root` if `SUDO_USER` is unset
or empty). Keep the installed account value when editing the unit.

```ini
[Unit]
Description=Vehicle webcam stream to GCS
After=network.target

[Service]
User=STREAM_USER
SupplementaryGroups=video
EnvironmentFile=/etc/vehicle-stream.env
ExecStart=/usr/local/bin/vehicle-stream.sh
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

`RestartSec=3` restarts the pipeline three seconds after it exits, including when the camera is
not ready at boot. The installer already enables and starts the service. After checking or
changing the environment file, restart it and inspect its status and log:

```bash
sudo systemctl restart vehicle-stream.service
systemctl status vehicle-stream.service
journalctl -u vehicle-stream.service -f
```

## Viewing and checking the relay

Test the generic `vehicleA` path with FFmpeg's `ffplay` viewer on the GCS Pi or operator laptop:

```bash
ffplay -rtsp_transport tcp rtsp://10.0.1.2:8554/vehicleA
```

RTSP over TCP worked on the bench; see [Status](#status) for the scope of verification.
Code review indicates RTSP URLs are supported (see
[Pointing QGC at the relay](#pointing-qgc-at-the-relay)); hardware playback in QGC is still
untested.

To repeat the start-on-boot check after setup changes, reboot each Pi, inspect both services
with the status and log commands above, then test playback again. Both services already
passed the unattended reboot check on 2026-11-14.

### Windows laptop viewers

Configure the laptop adapter with a unique address from
[Network addressing](NETWORK_ADDRESSING.md), mask `255.255.0.0` and an empty gateway.
Windows `ping` uses `-n` for the number of requests, rather than Linux's `-c`.
The bench record confirms playback with at least two players but does not identify them;
the following are viewing instructions, not a record of which players were used.

- **VLC:** open Media > Open Network Stream and enter `rtsp://10.0.1.2:8554/vehicleA`.
  In Preferences, select All settings, then Input / Codecs > Demuxers > RTP/RTSP, and enable
  "Use RTP over RTSP (TCP)".
- **FFmpeg's ffplay:** install from a Windows terminal, then open the RTSP URL:

  ```powershell
  winget install Gyan.FFmpeg
  ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://10.0.1.2:8554/vehicleA
  ```

- **Browser pages:** use `http://<GCS Pi>:8888/<path>/` for HLS or
  `http://<GCS Pi>:8889/<path>/` for WebRTC. Replace `<GCS Pi>` with its IP address and `<path>`
  with the configured name, for example `vehicleA`. Browser playback was not verified.

Windows Firewall may block inbound UDP, affecting WebRTC or a direct-UDP receiver on the
laptop. This was not verified on the bench. See [Troubleshooting](#troubleshooting) for other checks.

Raspberry Pi OS has no firewall by default. If Uncomplicated Firewall (`ufw`) is enabled on
the GCS Pi, allow the configured video UDP ports and TCP ports 8554, 8888 and 8889. For the
units 2-4 example:

```bash
sudo ufw allow 5602:5604/udp
sudo ufw allow 8554/tcp
sudo ufw allow 8888/tcp
sudo ufw allow 8889/tcp
```

## Pointing QGC at the relay

This section comes from reading this fork's source code. It has not been tested on hardware.

**Standard fly view / application-wide setting.** Open Application Settings > Video, choose
the source **RTSP Video Stream**, and enter the full **RTSP URL**, for example
`rtsp://10.0.1.2:8554/vehicleA`. This setting is application-wide, not per vehicle. If the
active connected vehicle advertises its own camera stream over MAVLink, that stream takes
priority over the manual setting. The manual source controls are disabled and the manual
connection fields are hidden while an advertised stream is available.

**COP overview (custom build): per-vehicle tiles.** Each connected vehicle's tile in the Common
Operating Picture (COP) overview has a **Video URI** field. Enter the full RTSP URL there to
give that vehicle its own stream; leave it empty to use the stream that vehicle advertises.
There is no fallback to the application-wide manual URL. The value is saved per vehicle
(by MAVLink system ID) and restored on the next launch. Duplicate full RTSP URLs and duplicate
UDP ports across active tiles are rejected.

**Scope caveat.** The saved COP URIs apply to the COP overview only: tile sessions are enabled
only while the overview is selected. The fly view uses the active vehicle's advertised stream
or the application-wide manual setting. No code was found that applies the saved COP URIs to
the fly view. QGC does not automatically map `5600 + N` ports to vehicles for RTSP; the full
URL, including the path name such as `vehicleA`, must be entered or advertised.

**Transport.** The GStreamer RTSP receiver allows unicast UDP and TCP. The code's own comment
says it tries UDP first and retries over TCP when no UDP data arrives within the timeout
(about 5 seconds). A laptop that cannot receive UDP, for example because a firewall blocks it,
may therefore show a delay before video starts. The
[video settings guide](../docs/en/qgc-user-guide/settings_view/video.md#connection) documents
`rtspt://` to request TCP from the start, for example `rtspt://10.0.1.2:8554/vehicleA`.
The repository passes that scheme through and leaves TCP enforcement to GStreamer; confirm
on hardware that it negotiates TCP.

**Defaults worth knowing.** The GStreamer jitter-buffer latency defaults to **80 ms** and is
adjustable as **RTP jitter latency** in the video settings; Low Latency Mode bypasses it.
The RTSP stream-stall timeout defaults to **8 s**, with **Auto-reconnect on stream loss** on
by default. The receiver waits for the stream's H.264 parameter sets (SPS/PPS); the documented
sender settings repeat them at every keyframe with `h264parse config-interval=-1`.

**To verify on hardware:**

- [ ] The packaged Windows build plays the full RTSP URL.
- [ ] Check negotiated transport (UDP versus TCP), including whether `rtspt://` forces TCP.
- [ ] Check startup and reconnect after the vehicle or relay restarts.
- [ ] Check behavior when switching between the COP overview and the fly view.
- [ ] Check playback with more than one vehicle.

## Bandwidth and troubleshooting

The verified single-vehicle bench settings were 1280x720 at 10 fps, with a 1 Mbit/s target
bitrate and a keyframe every five frames (0.5 seconds). The vehicle pipeline used about 20%
of one core, and GCS Pi CPU use stayed under about 15%. These results do not establish
capacity for multiple vehicles. Keep bandwidth headroom for MAVLink and other vehicles
sharing radio airtime. See
[Bench versus field transmit power](FIELD_RADIO_SETUP.md#bench-versus-field-transmit-power)
for the measured link problems and the before/after results.

### Checking the link

Run order-sensitive commands **one at a time** and wait for each to finish. Pasting several
commands at once can cancel a service stop with a later start. See
[Terminal basics](#terminal-basics) if Linux commands are new to you.

1. **Check the services.** On the GCS Pi:

   ```bash
   systemctl status mediamtx --no-pager
   journalctl -u mediamtx -n 30 --no-pager
   ```

   On the vehicle Pi:

   ```bash
   systemctl status vehicle-stream --no-pager
   journalctl -u vehicle-stream -n 30 --no-pager
   ```

2. **Check who owns the video port.** On the GCS Pi, use this command for the generic unit 2
   port; substitute the assigned port for another unit:

   ```bash
   sudo ss -ulnp | grep 5602
   ```

   MediaMTX must own the port. See [Video port already in use](#video-port-already-in-use).

3. **Measure large-packet loss with video stopped.** On the vehicle Pi, stop video first:

   ```bash
   sudo systemctl stop vehicle-stream
   ```

   When that finishes, run the following, replacing `<GCS Pi IP>` with the GCS Pi's address:

   ```bash
   ping -c 100 -i 0.2 -s 1200 <GCS Pi IP>
   ```

   Tiny pings earlier showed 0% loss and 3-14 ms round trips even while the large-packet test
   later showed severe loss. Use the large-packet test for this baseline. Check each radio's
   signal strength and noise readings on its web page; menu locations are model-specific.
   Those readings were not collected during the bench investigation.

4. **Measure throughput.** On the GCS Pi, start the server:

   ```bash
   iperf3 -s
   ```

   In a terminal on the vehicle Pi, run the TCP client:

   ```bash
   iperf3 -c <GCS Pi IP> -t 5
   ```

   Press Ctrl-C on the GCS Pi to stop the server when done. Restart video on the vehicle Pi
   after completing the baseline checks:

   ```bash
   sudo systemctl start vehicle-stream
   ```

5. **Compare datagrams carefully.** The following helper was used on both Pis to inspect
   their UDP counters. Run it on both at about the same moment while video is running:

   ```bash
   f() { awk '/^Udp:/ && $2 ~ /^[0-9]/ {print "InDatagrams="$2, "OutDatagrams="$5}' /proc/net/snmp; }
   f; sleep 10; f
   ```

   Subtract the first reading from the second; compare the vehicle Pi's `OutDatagrams` change
   with the GCS Pi's `InDatagrams` change. These totals count **all UDP traffic**, not just video,
   so use them as a rough guide only. An earlier comparison of total counters misleadingly
   suggested "no loss". Do not treat total UDP counter differences as a video-loss measurement.

   The per-port loss figure in the bench investigation (about **44% over 10 seconds**) came
   from a short throwaway listener script that counted only datagrams on the video port.
   That script is not kept in this repository. Until a tool for per-port counting is added,
   the large-packet ping in step 3 is the recommended baseline.

In testing, an `ffprobe` run that was piped into `head` and ended by a timeout printed nothing,
and output buffering is the suspected cause (not confirmed). Use a packet-count limit and
write to a file. This example has not been run exactly as written on hardware:

```bash
ffprobe -rtsp_transport tcp -read_intervals "%+#20" -show_packets \
  rtsp://10.0.1.2:8554/vehicleA > stream-packets.txt 2>&1
```

This probe inspects media packets; it is not a count of video UDP datagrams.

### Troubleshooting

- **No connection:** repeat the laptop ping sweep and mask checks in
  [Network addressing](NETWORK_ADDRESSING.md#troubleshooting).
- **No relay video:** inspect both service logs using [Checking the link](#checking-the-link).
  Check the camera format, destination host, vehicle port and matching MediaMTX path.
  The relay requires MPEG-TS, not the old RTP stream.
- **Camera fails at boot:** inspect the vehicle log and device path. The service retries every
  three seconds after exit. Unattended startup worked in the bench test; recovery from a
  camera-not-ready condition was not separately verified.
- **Heavy artifacts or burst loss:** follow [Checking the link](#checking-the-link) and
  [Bench versus field transmit power](FIELD_RADIO_SETUP.md#bench-versus-field-transmit-power).
  Close radios at high power can overload each other's receivers; do not diagnose the link
  from tiny pings alone. After the power fix, bench picture quality was judged reasonable,
  with occasional half-grey frames; see [Status](#status) for the likely cause and untested ideas.
- **Windows laptop cannot view:** check the adapter mask and empty gateway, then follow
  [Windows laptop viewers](#windows-laptop-viewers) to use RTSP over TCP. Windows Firewall
  may block inbound UDP for WebRTC or direct UDP; this possibility was not verified.

#### Video port already in use

`bind: address already in use` for a video port in the MediaMTX log occurred when a leftover
`gst-launch-1.0 ... udpsrc` test receiver still held that port. Find its process ID (PID) on
the GCS Pi with `sudo ss -ulnp | grep 5602`, substituting the affected port. Stop the identified
test receiver with `kill <PID>` (replace `<PID>` with its number), or reboot.

The relay must own each video port on the GCS Pi. No other receiver, including QGC running
on that Pi, may listen on it; viewers should connect to the relay instead.

#### Timeouts for idle vehicle paths

Repeating `[path X] ... read udp4 ...: i/o timeout` messages every 15 seconds are expected
for each declared path with no vehicle sending. This is log noise, not a fault.

#### MoQ certificate warning

Startup warnings such as `[MoQ] certificate auto.key not found ... permission denied` concern
a MediaMTX listener this project does not use. The warning was harmless in testing.
MoQ has not yet been disabled.

#### Slow MediaMTX stop

`sudo systemctl stop mediamtx` can take a long time while MediaMTX waits for connected viewers
to disconnect; systemd's default stop timeout is 90 seconds. Close VLC, browser tabs and
leftover `ffmpeg` or `ffprobe` viewers first. On the GCS Pi, check for connections with:

```bash
sudo ss -tnp | grep -E ':8554|:8888|:8889'
```

The [GCS installer](deploy/video-relay/gcs-setup.sh) now adds `TimeoutStopSec=10` to the
[GCS boot service](#gcs-boot-service). This directive has **not yet been exercised on hardware**.
For an existing installation, add the line below under `[Service]` in
`/etc/systemd/system/mediamtx.service`:

```ini
TimeoutStopSec=10
```

Then reload systemd's unit definitions:

```bash
sudo systemctl daemon-reload
```

## Terminal basics

- `less script.sh` opens the script in a viewer and installs nothing. Press `q` to quit.
  Reading a script before running it as root is a review step; run the appropriate installer
  command from [Installation prerequisites](#installation-prerequisites) and the setup sections
  when ready to install.
- Ctrl-C stops a running command or a log follow such as `journalctl -f`.
- `sudo` runs a command as administrator.
- Tab completes file or command names. The Up arrow recalls the last command.
- A stray character such as `~` pasted onto a package name makes `apt` fail with
  "Unable to locate package"; nothing from that line installs. Check the pasted text before retrying.
