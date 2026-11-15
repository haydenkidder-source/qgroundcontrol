#!/usr/bin/env bash
# Run on the vehicle Pi:  sudo bash vehicle-setup.sh
# Installs GStreamer, writes the stream script + config, and enables it at boot.
# Needs internet for apt (use Wi-Fi, not the radio link).
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo"; exit 1; }
RUN_USER="${SUDO_USER:-root}"

apt-get update
apt-get install -y gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav v4l-utils
usermod -aG video "$RUN_USER"

# Config: edit this file, then `sudo systemctl restart vehicle-stream`.
[ -f /etc/vehicle-stream.env ] || cat > /etc/vehicle-stream.env <<'ENV'
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
ENV

cat > /usr/local/bin/vehicle-stream.sh <<'SH'
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
SH
chmod 0755 /usr/local/bin/vehicle-stream.sh

cat > /etc/systemd/system/vehicle-stream.service <<UNIT
[Unit]
Description=Vehicle webcam stream to GCS
After=network.target

[Service]
User=${RUN_USER}
SupplementaryGroups=video
EnvironmentFile=/etc/vehicle-stream.env
ExecStart=/usr/local/bin/vehicle-stream.sh
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable --now vehicle-stream
sleep 3
systemctl --no-pager --lines=15 status vehicle-stream || true
