# Video relay installers

These installers and their relay, boot services and RTSP-in-QGC playback are **untested on hardware**.
See [Video relay setup](../../VIDEO_RELAY_SETUP.md) for configuration, pipeline listings and
hardware checks, and [Network addressing](../../NETWORK_ADDRESSING.md) for the IP and port scheme.

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

The GCS installer downloads the latest MediaMTX release by default. To pin a release, use
`sudo MEDIAMTX_VERSION=v1.x.y ./gcs-setup.sh` with an actual release tag. It backs up any existing
`/etc/mediamtx/mediamtx.yml`, replaces it with the generic `vehicleA`/5602, `vehicleB`/5603 and
`vehicleC`/5604 paths, and enables and starts `mediamtx.service` as the `mediamtx` system account.
The configuration has no authentication; review access control before field use.

On each vehicle Pi, from the repository root, run through `sudo` as the intended camera account:

```bash
cd custom/deploy/video-relay/
sudo ./vehicle-setup.sh
```

This installs GStreamer and camera tools, adds the invoking account to the `video` group, writes
`/usr/local/bin/vehicle-stream.sh` and enables and starts `vehicle-stream.service`. The account
comes from `SUDO_USER`, falling back to `root` if unset or empty. An existing
`/etc/vehicle-stream.env` is preserved. Check that file's camera settings and set `DEST_PORT`
to `5600 + N` for this vehicle's unit before using its stream; the default is 5602.
Never leave two vehicles sending to the same port. After editing the environment file:

```bash
sudo systemctl restart vehicle-stream.service
systemctl status vehicle-stream.service
journalctl -u vehicle-stream.service -f
```
