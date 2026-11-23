#!/usr/bin/env bash
# Run on the vehicle Pi:  sudo bash vehicle-setup.sh --unit N
# Installs GStreamer, writes the stream script + config, and enables it at boot.
# INSTALL_ROOT prefixes filesystem paths for isolated installer tests.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "Run with sudo"; exit 1; }
INSTALL_ROOT="${INSTALL_ROOT:-}"
RUN_USER="${SUDO_USER:-root}"

usage() {
  echo "Usage: sudo bash vehicle-setup.sh --unit N" >&2
  echo "   or: sudo UNIT=N bash vehicle-setup.sh" >&2
  echo "Optional: --camera PATH (or CAMERA=PATH)" >&2
  echo "Vehicle unit N must be plain decimal digits from 2 to 254, without leading zeros (1 is the GCS)." >&2
}

CAMERA_PATH="${CAMERA-}"
CAMERA_GIVEN="${CAMERA+x}"
UNIT_NUMBER="${UNIT-}"
UNIT_GIVEN="${UNIT+x}"
while (( $# )); do
  case "$1" in
    --unit)
      if (( $# < 2 )); then
        echo "Missing value for --unit" >&2
        usage
        exit 2
      fi
      UNIT_NUMBER="$2"
      UNIT_GIVEN=x
      shift 2
      ;;
    --camera)
      if (( $# < 2 )); then
        echo "Missing value for --camera" >&2; exit 2
      fi
      CAMERA_PATH="$2"
      CAMERA_GIVEN=x
      shift 2
      ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done
if [[ -z "$UNIT_GIVEN" && -t 0 ]]; then
  IFS= read -r -p "Vehicle unit number (2-254): " UNIT_NUMBER || UNIT_NUMBER=""
  UNIT_GIVEN=x
fi
if [[ -n "$UNIT_GIVEN" ]]; then
  if [[ ! "$UNIT_NUMBER" =~ ^[1-9][0-9]{0,2}$ ]] || (( UNIT_NUMBER < 2 || UNIT_NUMBER > 254 )); then
    echo "Invalid vehicle unit: '$UNIT_NUMBER'" >&2
    usage
    exit 2
  fi
  DEST_PORT=$((5600 + UNIT_NUMBER))
fi

if [[ -f "$INSTALL_ROOT/etc/vehicle-stream.env" ]]; then
  # Keep assignment names until after capture so duplicate empty values cannot disappear.
  EXISTING_PORT=$(sed -n '/^[[:blank:]]*DEST_PORT[[:blank:]]*=/ {
    s/^[^=]*=[[:blank:]]*/DEST_PORT=/
    s/[[:blank:]]*$//
    p
  }' "$INSTALL_ROOT/etc/vehicle-stream.env")
  EXISTING_PORT=${EXISTING_PORT#DEST_PORT=}
  case "$EXISTING_PORT" in
    \"*\") EXISTING_PORT="${EXISTING_PORT:1:-1}" ;;
    \'*\') EXISTING_PORT="${EXISTING_PORT:1:-1}" ;;
  esac
  if [[ -n "$UNIT_GIVEN" && "$EXISTING_PORT" != "$DEST_PORT" ]]; then
    echo "/etc/vehicle-stream.env has DEST_PORT=${EXISTING_PORT:-<missing>}; unit $UNIT_NUMBER requires DEST_PORT=$DEST_PORT." >&2
    echo "Edit the file or remove it and re-run with --unit $UNIT_NUMBER. Service state unchanged." >&2
    exit 2
  fi
fi
if [[ -z "$UNIT_GIVEN" ]]; then
  if [[ -f "$INSTALL_ROOT/etc/vehicle-stream.env" ]] &&
      { systemctl is-enabled --quiet vehicle-stream || systemctl is-active --quiet vehicle-stream; }; then
    echo "No unit supplied; preserving service state and /etc/vehicle-stream.env (DEST_PORT=${EXISTING_PORT:-<missing>})."
  else
    echo "A vehicle unit number is required before installation; no changes made." >&2
    usage
    exit 2
  fi
fi

shopt -s nullglob
CAMERA_CANDIDATES=("$INSTALL_ROOT"/dev/v4l/by-id/*-video-index0)
shopt -u nullglob
CAMERA_MISSING=""
if [[ ! -f "$INSTALL_ROOT/etc/vehicle-stream.env" ]]; then
  if [[ -n "$CAMERA_GIVEN" ]]; then
    if [[ "$CAMERA_PATH" != /dev/v4l/by-id/* && "$CAMERA_PATH" != /dev/v4l/by-path/* ]]; then
      echo "Camera path must start with /dev/v4l/by-id/ or /dev/v4l/by-path/" >&2; exit 2
    fi
    if [[ ! -e "$INSTALL_ROOT$CAMERA_PATH" ]]; then
      echo "Camera path does not exist: $CAMERA_PATH" >&2; exit 2
    fi
  elif (( ${#CAMERA_CANDIDATES[@]} == 1 )); then
    CAMERA_PATH="${CAMERA_CANDIDATES[0]#"$INSTALL_ROOT"}"
  else
    CAMERA_PATH=""
    CAMERA_MISSING=x
  fi
else
  if [[ -n "$CAMERA_GIVEN" ]]; then
    echo "Ignoring --camera: /etc/vehicle-stream.env already exists; edit CAM_DEVICE there." >&2
  fi
  EXISTING_CAMERA=$(sed -n 's/^[[:blank:]]*CAM_DEVICE[[:blank:]]*=[[:blank:]]*//p' "$INSTALL_ROOT/etc/vehicle-stream.env")
  EXISTING_CAMERA="${EXISTING_CAMERA%"${EXISTING_CAMERA##*[![:space:]]}"}"
  case "$EXISTING_CAMERA" in
    \"*\") EXISTING_CAMERA="${EXISTING_CAMERA:1:-1}" ;;
    \'*\') EXISTING_CAMERA="${EXISTING_CAMERA:1:-1}" ;;
  esac
  if [[ -z "$EXISTING_CAMERA" || "$EXISTING_CAMERA" == /dev/video* ]]; then
    echo "Warning: CAM_DEVICE is empty or uses an unstable /dev/video path in /etc/vehicle-stream.env" >&2
    if (( ${#CAMERA_CANDIDATES[@]} == 1 )); then
      echo "Suggested camera: ${CAMERA_CANDIDATES[0]#"$INSTALL_ROOT"}" >&2
    fi
  fi
fi

apt-get update
apt-get install -y gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav v4l-utils
usermod -aG video "$RUN_USER"

# Quote paths for systemd EnvironmentFile parsing, including udev backslash escapes.
CAMERA_CONFIG="$CAMERA_PATH"
if [[ -n "$CAMERA_CONFIG" ]]; then
  CAMERA_CONFIG="${CAMERA_CONFIG//\\/\\\\}"
  CAMERA_CONFIG="${CAMERA_CONFIG//\"/\\\"}"
  CAMERA_CONFIG="\"$CAMERA_CONFIG\""
fi

# Config: edit this file, then `sudo systemctl restart vehicle-stream`.
mkdir -p "$INSTALL_ROOT/etc" "$INSTALL_ROOT/usr/local/bin" "$INSTALL_ROOT/etc/systemd/system"
[ -f "$INSTALL_ROOT/etc/vehicle-stream.env" ] || cat > "$INSTALL_ROOT/etc/vehicle-stream.env" <<ENV
# Where to send (the GCS Pi) and which port. Unit $UNIT_NUMBER: port = 5600 + $UNIT_NUMBER.
DEST_HOST=10.0.1.2
DEST_PORT=$DEST_PORT
# Camera. Use a stable path from:  ls -l /dev/v4l/by-id/
CAM_DEVICE=$CAMERA_CONFIG
# Space-separated name=value controls; prevent low-light frame-rate reduction by default.
# Set CAM_CTRLS="" to disable. Unknown control names make v4l2-ctl fail (stream continues).
CAM_CTRLS="exposure_dynamic_framerate=0"
# mjpeg or yuyv (see: v4l2-ctl -d <camera path> --list-formats-ext)
CAM_FORMAT=mjpeg
WIDTH=1280
HEIGHT=720
FPS=10
# kbit/s
BITRATE=1000
# keyframe every KEYINT frames (5 at 10 fps = every 0.5 s)
KEYINT=5
ENV

cat > "$INSTALL_ROOT/usr/local/bin/vehicle-stream.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ -z "${CAM_DEVICE:-}" ]]; then
  echo "CAM_DEVICE is not set in /etc/vehicle-stream.env" >&2; exit 1
fi
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
read -r -a CONTROLS <<< "${CAM_CTRLS-exposure_dynamic_framerate=0}"
if (( ${#CONTROLS[@]} )); then
  CONTROL_ARGS=()
  for CONTROL in "${CONTROLS[@]}"; do
    CONTROL_ARGS+=(-c "$CONTROL")
  done
  if ! v4l2-ctl -d "$CAM_DEVICE" "${CONTROL_ARGS[@]}"; then
    echo "Warning: could not apply CAM_CTRLS to $CAM_DEVICE; continuing stream" >&2
  fi
fi
exec gst-launch-1.0 -e "${SRC[@]}" \
  ! x264enc tune=zerolatency speed-preset=ultrafast bitrate="$BITRATE" key-int-max="$KEYINT" \
  ! "video/x-h264,profile=baseline" \
  ! h264parse config-interval=-1 \
  ! mpegtsmux alignment=7 \
  ! udpsink host="$DEST_HOST" port="$DEST_PORT"
SH
chmod 0755 "$INSTALL_ROOT/usr/local/bin/vehicle-stream.sh"

cat > "$INSTALL_ROOT/etc/systemd/system/vehicle-stream.service" <<UNIT
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
if [[ -n "$CAMERA_MISSING" ]]; then
  echo "Camera candidates:"
  for candidate in "${CAMERA_CANDIDATES[@]}"; do
    echo "  ${candidate#"$INSTALL_ROOT"}"
  done
  echo "Edit CAM_DEVICE in /etc/vehicle-stream.env to a stable camera path."
  echo "Installed, stream service not started because the camera path is not set."
  exit 3
fi
SERVICE_RESTARTED=""
if [[ -n "$UNIT_GIVEN" ]]; then
  systemctl enable vehicle-stream
  systemctl restart vehicle-stream
  SERVICE_RESTARTED=x
elif systemctl is-active --quiet vehicle-stream; then
  systemctl restart vehicle-stream
  SERVICE_RESTARTED=x
fi
if [[ -n "$SERVICE_RESTARTED" ]]; then
  sleep "${SETTLE_SECONDS:-3}"
  if ! systemctl is-active --quiet vehicle-stream; then
    systemctl --no-pager --lines=30 status vehicle-stream || :
    echo "Vehicle stream install did NOT succeed" >&2
    exit 1
  fi
fi
echo "Done."
