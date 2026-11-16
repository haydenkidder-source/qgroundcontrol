#!/usr/bin/env bash
# Run on the GCS Pi:  sudo bash gcs-setup.sh
# Installs MediaMTX (arm64), writes its config, and enables it at boot.
# Needs internet for the download (use Wi-Fi, not the radio link).
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo"; exit 1; }
[ "$(uname -m)" = "aarch64" ] || { echo "Expected a 64-bit Pi OS (aarch64), got $(uname -m)"; exit 1; }

# Pin a version with: sudo MEDIAMTX_VERSION=v1.x.y bash gcs-setup.sh   (default: latest)
VERSION="${MEDIAMTX_VERSION:-$(curl -fsSLI -o /dev/null -w '%{url_effective}' \
  https://github.com/bluenviron/mediamtx/releases/latest | sed 's|.*/tag/||')}"
echo "Installing MediaMTX ${VERSION}"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/mtx.tar.gz" \
  "https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/mediamtx_${VERSION}_linux_arm64.tar.gz"
mkdir -p /opt/mediamtx /etc/mediamtx
tar -xzf "$tmp/mtx.tar.gz" -C "$tmp"
install -m 0755 "$tmp/mediamtx" /opt/mediamtx/mediamtx

id mediamtx >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin mediamtx

[ -f /etc/mediamtx/mediamtx.yml ] && cp /etc/mediamtx/mediamtx.yml "/etc/mediamtx/mediamtx.yml.bak.$(date +%s)"
cat > /etc/mediamtx/mediamtx.yml <<'YML'
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
YML

cat > /etc/systemd/system/mediamtx.service <<'UNIT'
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
UNIT

systemctl daemon-reload
systemctl enable --now mediamtx
sleep 2
systemctl --no-pager --lines=15 status mediamtx || true
echo; echo "Done. Clients pull from rtsp://$(hostname -I | awk '{print $1}'):8554/vehicleA"
