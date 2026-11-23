# Video relay installers

On **2026-11-14**, both installers ran on Raspberry Pi 5 computers with Raspberry Pi OS 64-bit.
The installer fetched MediaMTX v1.21.3, latest at the time. Both boot services started unattended
after reboot, and RTSP over TCP worked locally on the GCS Pi and from a Windows laptop.
RTSP-in-QGC remains **untested** on hardware. After the radio fix, bench picture quality was
judged reasonable, with occasional half-grey frames; field-distance power and multiple-vehicle
behavior remain unverified. See [Status](../../VIDEO_RELAY_SETUP.md#status) for the full scope
of verification.
QGC setup: see [Pointing QGC at the relay](../../VIDEO_RELAY_SETUP.md#pointing-qgc-at-the-relay).

See [Video relay setup](../../VIDEO_RELAY_SETUP.md) for configuration and pipeline listings,
[Network addressing](../../NETWORK_ADDRESSING.md) for the IP and port scheme, and
[Bench versus field transmit power](../../FIELD_RADIO_SETUP.md#bench-versus-field-transmit-power)
for the radio findings. The guide also provides
[link checks](../../VIDEO_RELAY_SETUP.md#checking-the-link),
[Windows viewing instructions](../../VIDEO_RELAY_SETUP.md#windows-laptop-viewers) and
[terminal basics](../../VIDEO_RELAY_SETUP.md#terminal-basics).
See [Tuning latency](../../VIDEO_RELAY_SETUP.md#tuning-latency) for the dated bench results,
[Camera controls](../../VIDEO_RELAY_SETUP.md#camera-controls) for `CAM_CTRLS`, and
[Measuring latency and frame rate](../../VIDEO_RELAY_SETUP.md#measuring-latency-and-frame-rate)
for repeatable checks. QGC was used for latency testing, but its video source type was not
recorded; this does not verify RTSP-in-QGC. Browser playback via the relay was reported near
zero delay, with the endpoint not recorded; HLS was not tested.

## Prerequisites

- Raspberry Pi OS with Bash, systemd, `sudo` and standard Linux user-management tools.
- Internet access over Wi-Fi for installation; use the radio link for vehicle traffic.
- GCS Pi: 64-bit OS (`aarch64`), `curl` and `tar`.
- Vehicle Pi: `apt-get` and a USB webcam with MJPEG or YUYV support.
- Static addresses and netmask configured as described in the network guide.

## Usage

Copy this directory to the appropriate Pi, or use a checkout there. From the repository root
on the GCS Pi:

```bash
cd custom/deploy/video-relay/
sudo ./gcs-setup.sh
```

The GCS installer pins MediaMTX v1.21.4 and verifies its arm64 archive before installation.
The SHA-256 pin is deliberately unset; a maintainer must copy the arm64 archive's digest from
that release's checksums file into `MEDIAMTX_PINNED_SHA256_LINUX_ARM64` before use. To override
both values, set `MEDIAMTX_VERSION` and `MEDIAMTX_SHA256` together through `sudo`. The installer
keeps an existing `/etc/mediamtx/mediamtx.yml` and refreshes `mediamtx.yml.example` alongside it;
a new configuration is created from that example. Every run enables and restarts the service.
The configuration has no authentication; review access control before field use.

The GCS unit now includes `TimeoutStopSec=10` under `[Service]` to limit the stop wait to
10 seconds. This directive has **not yet been exercised on hardware**. On existing installs,
add that line under `[Service]` in `/etc/systemd/system/mediamtx.service`, then run:

```bash
sudo systemctl daemon-reload
```

Close connected viewers before stopping the relay; see
[Slow MediaMTX stop](../../VIDEO_RELAY_SETUP.md#slow-mediamtx-stop).

On each vehicle Pi, from the repository root, run through `sudo` as the intended camera account:

```bash
cd custom/deploy/video-relay/
sudo bash vehicle-setup.sh --unit 3
# Equivalent environment-variable form:
sudo UNIT=3 bash vehicle-setup.sh
```

This installs GStreamer and camera tools, adds the invoking account to the `video` group, writes
`/usr/local/bin/vehicle-stream.sh` and enables and starts `vehicle-stream.service` after validating
the unit and port. The account comes from `SUDO_USER`, falling back to `root` if unset or empty.
Use this vehicle's unit `N` (2-254, plain decimal digits without leading zeros; unit 1 is the GCS).
The `--unit N` flag takes precedence over `UNIT=N`. A new `/etc/vehicle-stream.env` gets
`DEST_PORT` set to `5600 + N` (5603 for unit 3); other defaults are unchanged. Check camera settings.
Never leave two vehicles sending to the same port.

> **Unit required before streaming:** Without a flag or variable, terminal input prompts once.
> Invalid input exits with status 2 before package installation or file writes. Without terminal
> input or a unit, setup also exits with status 2 and usage text before changes, except when an
> environment file exists and the service is already enabled or active: that re-run updates the
> wrapper and unit, prints the existing `DEST_PORT`, preserves the enabled state, and restarts only an active service. An existing environment file is always preserved. With a unit,
> its `DEST_PORT` must match `5600 + N`; a mismatch exits with status 2 before changes, reports both
> ports and the file path, and leaves service state unchanged. Edit the file or remove it and
> re-run with `--unit N`. A matching re-run enables and restarts the service. A unitless re-run restarts it only if active.

For a new configuration, specify `--camera /dev/v4l/by-id/...` or set `CAMERA` to an
existing `/dev/v4l/by-id/` or `/dev/v4l/by-path/` device. Otherwise the installer selects the
single `*-video-index0` entry under `/dev/v4l/by-id/`. If none or several exist, it lists the
candidates, leaves `CAM_DEVICE` empty and does not start the service; edit the environment file
before starting it. Existing environment files are preserved, with a warning for empty or unstable
`/dev/video*` camera paths.

Both installers return 0 on success, 1 for verification or service failure, 2 for usage or pin
errors, and 3 when installed but the camera path is not set.

The wrapper applies `CAM_CTRLS` before starting GStreamer at every service start. It is a
space-separated list of `name=value` V4L2 controls; if unset, it defaults to
`exposure_dynamic_framerate=0` to prevent low-light frame-rate reduction. Set `CAM_CTRLS=""`
to apply nothing. Unknown control names make `v4l2-ctl` fail; the wrapper warns to stderr and
continues streaming on any control failure. For existing installs, rerun `vehicle-setup.sh`
to replace the wrapper, then restart the service. The environment file is preserved, and an
unset `CAM_CTRLS` still gets the default fix. Installer defaults remain 10 fps, 1000 kbit/s
and a keyframe every 5 frames; the tuned values are documented only. The updated wrapper's
automatic control application was **verified on hardware for a service restart on 2026-11-15**.
Application after a full reboot or camera unplug/replug has not yet been tested.

After updating the wrapper or editing the environment file:

```bash
sudo systemctl restart vehicle-stream.service
systemctl status vehicle-stream.service
journalctl -u vehicle-stream.service -f
```
