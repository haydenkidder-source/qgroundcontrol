# Video relay setup

## Status

- **Reported bench observations:** a vehicle USB webcam stream sent directly over User Datagram
  Protocol (UDP) to the GCS side was received and displayed with some glitching, which the owner
  attributed to 2.4 GHz interference. The receiving application used for that test is not recorded.
  The owner reported the laptop ping sweeps as completed; the resulting address list matched the
  addressing scheme. At the time of writing, the stream had not yet been pulled into the operator
  laptop's QGroundControl (QGC).
- **Untested on hardware:** the MediaMTX relay and the start-on-boot services described below.
  Whether this QGC build accepts an `rtsp://` video source is also untested and must be
  confirmed in the application. Do not treat direct UDP playback as proof of relay playback.
- The old direct-to-receiver pipeline used Real-time Transport Protocol (RTP), with
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
through Web Real-Time Communication (WebRTC) on port 8889. These relay viewing options remain
untested on hardware.

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

[Install]
WantedBy=multi-user.target
```

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

From the operator laptop, test the generic `vehicleA` path with FFmpeg's `ffplay` viewer:

```bash
ffplay -rtsp_transport tcp rtsp://10.0.1.2:8554/vehicleA
```

Alternatively, in VLC open Media > Open Network Stream and enter that URL. Browser endpoints
for the same example are `http://10.0.1.2:8888/vehicleA` (HLS) and
`http://10.0.1.2:8889/vehicleA` (WebRTC). Confirm playback on hardware; these relay paths have
not yet been verified. Confirm whether this QGC build offers and accepts an RTSP source in
the application before trying to use the URL there.

To check start-on-boot behavior, reboot each Pi after setup, inspect both services with the
status and log commands above, then test playback again. This is a required hardware check,
not a result already obtained.

Raspberry Pi OS has no firewall by default. If Uncomplicated Firewall (`ufw`) is enabled on
the GCS Pi, allow the configured video UDP ports and TCP ports 8554, 8888 and 8889. For the
units 2-4 example:

```bash
sudo ufw allow 5602:5604/udp
sudo ufw allow 8554/tcp
sudo ufw allow 8888/tcp
sudo ufw allow 8889/tcp
```

## Bandwidth and troubleshooting

Bench parameters used so far are 1280x720 at 10 fps, about 1 Mbps, with a keyframe every five
frames (0.5 seconds). The Raspberry Pi 5 has no hardware H.264 encoder; x264 runs on its processor
and is comfortable at this size. These parameters do not establish that the relay works.

An example mesh measurement in a noisy 2.4 GHz environment gave about 8 Mbps Transmission
Control Protocol (TCP) throughput from vehicle to GCS, with some retransmissions. This is not
a guarantee. Budget video well below that measurement and keep headroom for MAVLink and for
other vehicles sharing radio airtime.

- **No connection:** repeat the laptop ping sweep and mask checks in
  [Network addressing](NETWORK_ADDRESSING.md#troubleshooting).
- **No relay video:** inspect both service logs. Check the camera format, destination host,
  vehicle port and matching MediaMTX path. The relay requires MPEG-TS, not the old RTP stream.
- **Camera fails at boot:** inspect the vehicle log and device path. The service retries every
  three seconds after exit; hardware behavior still needs verification.
- **Glitchy video under interference:** this occurred during direct UDP bench playback.
  Separate radios and antennas, try another radio channel, or shorten the keyframe interval.
  Smaller RTP/UDP packets are another mitigation to test; RTP applies to the old direct path,
  while this relay uses MPEG-TS over UDP. The relay pipeline already uses 1316-byte datagrams.
