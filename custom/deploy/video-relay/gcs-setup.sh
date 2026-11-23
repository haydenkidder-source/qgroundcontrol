#!/usr/bin/env bash
# Run on the GCS Pi:  sudo bash gcs-setup.sh
# Installs MediaMTX (arm64), writes its config, and enables it at boot.
# INSTALL_ROOT prefixes filesystem paths for isolated installer tests.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo"; exit 1; }
[ "$(uname -m)" = "aarch64" ] || { echo "Expected a 64-bit Pi OS (aarch64), got $(uname -m)"; exit 1; }

MEDIAMTX_PINNED_VERSION="v1.21.4"
MEDIAMTX_PINNED_SHA256_LINUX_ARM64="0000000000000000000000000000000000000000000000000000000000000000"
INSTALL_ROOT="${INSTALL_ROOT:-}"
if [[ -n "${MEDIAMTX_VERSION+x}" || -n "${MEDIAMTX_SHA256+x}" ]]; then
  if [[ -z "${MEDIAMTX_VERSION:-}" || -z "${MEDIAMTX_SHA256:-}" ]]; then
    echo "MEDIAMTX_VERSION and MEDIAMTX_SHA256 must be supplied together" >&2
    exit 2
  fi
  VERSION="$MEDIAMTX_VERSION"
  EXPECTED_SHA256="$MEDIAMTX_SHA256"
else
  VERSION="$MEDIAMTX_PINNED_VERSION"
  EXPECTED_SHA256="$MEDIAMTX_PINNED_SHA256_LINUX_ARM64"
  if [[ "$EXPECTED_SHA256" =~ ^0{64}$ ]]; then
    echo "MediaMTX SHA256 pin is not set in gcs-setup.sh; use the version and digest of a verified release (see README.md), or supply MEDIAMTX_VERSION and MEDIAMTX_SHA256 together." >&2
    exit 2
  fi
fi
if [[ ! "$EXPECTED_SHA256" =~ ^[[:xdigit:]]{64}$ ]]; then
  echo "MEDIAMTX_SHA256 must contain 64 hexadecimal characters" >&2
  exit 2
fi
echo "Installing MediaMTX ${VERSION}"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL --proto '=https' --tlsv1.2 --retry 3 -o "$tmp/mtx.tar.gz" \
  "https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/mediamtx_${VERSION}_linux_arm64.tar.gz"
ACTUAL_SHA256="$(sha256sum "$tmp/mtx.tar.gz" | awk '{print $1}')"
if [[ "${ACTUAL_SHA256,,}" != "${EXPECTED_SHA256,,}" ]]; then
  echo "MediaMTX checksum mismatch: expected $EXPECTED_SHA256, actual $ACTUAL_SHA256" >&2
  exit 1
fi
tar -xzf "$tmp/mtx.tar.gz" -C "$tmp" mediamtx
mkdir -p "$INSTALL_ROOT/opt/mediamtx" "$INSTALL_ROOT/etc/mediamtx" "$INSTALL_ROOT/etc/systemd/system"
install -m 0755 "$tmp/mediamtx" "$INSTALL_ROOT/opt/mediamtx/mediamtx"

id mediamtx >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin mediamtx

cat > "$INSTALL_ROOT/etc/mediamtx/mediamtx.yml.example" <<'YML'
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
if [[ -e "$INSTALL_ROOT/etc/mediamtx/mediamtx.yml" ]]; then
  echo "kept the existing /etc/mediamtx/mediamtx.yml; the example for this version is /etc/mediamtx/mediamtx.yml.example"
else
  cp "$INSTALL_ROOT/etc/mediamtx/mediamtx.yml.example" "$INSTALL_ROOT/etc/mediamtx/mediamtx.yml"
  echo "Created /etc/mediamtx/mediamtx.yml from /etc/mediamtx/mediamtx.yml.example"
fi

cat > "$INSTALL_ROOT/etc/systemd/system/mediamtx.service" <<'UNIT'
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
systemctl enable mediamtx
systemctl restart mediamtx
sleep "${SETTLE_SECONDS:-3}"
if ! systemctl is-active --quiet mediamtx; then
  systemctl --no-pager --lines=30 status mediamtx || :
  echo "MediaMTX install did NOT succeed" >&2
  exit 1
fi
echo "MediaMTX ${VERSION} is running. That does not show that any vehicle is streaming: a path only becomes available when its vehicle sends video."
echo "Clients pull from rtsp://$(hostname -I | awk '{print $1}'):8554/vehicleA"
echo "Done."
