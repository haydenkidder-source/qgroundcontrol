"""Exercise the Pi installers without root, hardware, systemd or network access."""

from __future__ import annotations

import hashlib
import io
import os
import shutil
import subprocess
import tarfile

import pytest

from ._helpers import REPO_ROOT

SCRIPTS = REPO_ROOT / "custom/deploy/video-relay"


@pytest.fixture
def rig(tmp_path):
    root = root_dir = tmp_path / "root"
    root.mkdir()
    bins = tmp_path / "bin"
    bins.mkdir()
    log = tmp_path / "calls"
    archive = tmp_path / "archive.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for name, data in [("mediamtx", b"binary"), ("../evil", b"bad")]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    stub = """#!/usr/bin/env bash
name=${0##*/}
printf '%s' "$name" >> "$CALL_LOG"
printf ' <%s>' "$@" >> "$CALL_LOG"
printf '\\n' >> "$CALL_LOG"
case "$name" in
  id) [[ "${1:-}" == -u ]] && { echo 0; exit 0; }; exit 1 ;;
  uname) echo aarch64 ;;
  curl) while (( $# )); do if [[ "$1" == -o ]]; then cp "$ARCHIVE" "$2"; break; fi; shift; done ;;
  tar) exec "$REAL_TAR" "$@" ;;
  systemctl)
    if [[ "$1" == is-active ]]; then
      if [[ -n "${ACTIVE_SEQUENCE:-}" ]]; then
        count=0; [[ -f "$STATE_FILE" ]] && count=$(cat "$STATE_FILE")
        echo $((count + 1)) > "$STATE_FILE"
        [[ "${ACTIVE_SEQUENCE:count:1}" == 1 ]]
      else [[ "${ACTIVE:-1}" == 1 ]]; fi
    elif [[ "$1" == is-enabled ]]; then [[ "${ENABLED:-1}" == 1 ]]; fi ;;
  hostname) echo 192.0.2.1 ;;
esac
"""
    for name in (
        "id",
        "uname",
        "curl",
        "tar",
        "systemctl",
        "useradd",
        "usermod",
        "apt-get",
        "hostname",
        "sleep",
    ):
        path = bins / name
        path.write_text(stub)
        path.chmod(0o755)
    real_tar = shutil.which("tar")
    assert real_tar is not None
    env = os.environ.copy()
    for key in ("MEDIAMTX_VERSION", "MEDIAMTX_SHA256", "UNIT", "CAMERA", "SUDO_USER"):
        env.pop(key, None)
    env.update(
        PATH=f"{bins}:{env['PATH']}",
        INSTALL_ROOT=str(root),
        CALL_LOG=str(log),
        ARCHIVE=str(archive),
        REAL_TAR=real_tar,
        STATE_FILE=str(tmp_path / "state"),
        SETTLE_SECONDS="0",
    )

    archive_digest = hashlib.sha256(archive.read_bytes()).hexdigest()

    class Rig:
        root = root_dir
        digest = archive_digest

        def run(self, script, *args, **variables):
            return subprocess.run(
                ["bash", str(SCRIPTS / script), *args],
                env=env | variables,
                capture_output=True,
                text=True,
                check=False,
            )

        def calls(self):
            return log.read_text().splitlines() if log.exists() else []

        def camera(self, name="camera-video-index0", directory="by-id"):
            path = root / "dev/v4l" / directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
            return "/" + str(path.relative_to(root))

    return Rig()


def gcs(rig, **env):
    return rig.run(
        "gcs-setup.sh", MEDIAMTX_VERSION="v1.2.3", MEDIAMTX_SHA256=rig.digest.upper(), **env
    )


def test_gcs_verified_install_and_safe_extract(rig):
    """A verified HTTPS archive installs only the binary and restarts the enabled service."""
    result = gcs(rig)
    assert result.returncode == 0 and "Done." in result.stdout
    assert (rig.root / "opt/mediamtx/mediamtx").read_bytes() == b"binary"
    assert (rig.root / "etc/mediamtx/mediamtx.yml").exists()
    assert not (rig.root.parent / "evil").exists()
    calls = rig.calls()
    tar_calls = [call for call in calls if call.startswith("tar ")]
    assert len(tar_calls) == 1 and tar_calls[0].endswith(" <mediamtx>")
    curl = next(call for call in calls if call.startswith("curl "))
    assert "<--proto> <=https> <--tlsv1.2> <--retry> <3>" in curl
    assert "<https://github.com/" in curl and "releases/latest" not in curl
    assert (
        calls.index("systemctl <daemon-reload>")
        < calls.index("systemctl <enable> <mediamtx>")
        < calls.index("systemctl <restart> <mediamtx>")
    )
    assert str(rig.root) not in (rig.root / "etc/systemd/system/mediamtx.service").read_text()


def test_gcs_checksum_mismatch_leaves_installation_untouched(rig):
    """A mismatched digest fails before installation or systemd changes."""
    result = rig.run("gcs-setup.sh", MEDIAMTX_VERSION="v1", MEDIAMTX_SHA256="a" * 64)
    assert result.returncode == 1 and "expected" in result.stderr and "actual" in result.stderr
    assert not list(rig.root.iterdir()) and not any(c.startswith("systemctl") for c in rig.calls())


@pytest.mark.parametrize(
    "variables",
    [
        {"MEDIAMTX_VERSION": "v1"},
        {"MEDIAMTX_SHA256": "a" * 64},
        {"MEDIAMTX_VERSION": "v1", "MEDIAMTX_SHA256": "bad"},
        {},
    ],
)
def test_gcs_pin_and_override_validation(rig, variables):
    """An unset pin, incomplete override, or malformed digest refuses installation."""
    result = rig.run("gcs-setup.sh", **variables)
    assert result.returncode == 2
    assert ("pin is not set" if not variables else "MEDIAMTX_SHA256") in result.stderr
    if not variables:
        assert (
            "MediaMTX SHA256 pin is not set in gcs-setup.sh; use the version and digest of a "
            "verified release (see README.md), or supply MEDIAMTX_VERSION and MEDIAMTX_SHA256 together."
        ) in result.stderr
    assert not any(c.startswith("curl ") for c in rig.calls())


def test_gcs_rerun_preserves_config_and_refreshes_example(rig):
    """Reinstallation keeps operator config, refreshes the example, and restarts."""
    assert gcs(rig).returncode == 0
    config = rig.root / "etc/mediamtx/mediamtx.yml"
    example = config.with_name("mediamtx.yml.example")
    config.write_bytes(b"operator\n")
    example.write_text("old")
    result = gcs(rig)
    assert result.returncode == 0 and "kept the existing" in result.stdout
    assert config.read_bytes() == b"operator\n" and "paths:" in example.read_text()
    assert rig.calls().count("systemctl <restart> <mediamtx>") == 2


def test_gcs_inactive_fails(rig):
    """A failed service after the settle delay cannot report success."""
    result = gcs(rig, ACTIVE="0")
    assert (
        result.returncode == 1
        and "Done." not in result.stdout
        and "did NOT succeed" in result.stderr
    )
    assert "systemctl <--no-pager> <--lines=30> <status> <mediamtx>" in rig.calls()


@pytest.mark.parametrize(
    "args",
    [("--unit", unit) for unit in ("01", "010", "3x", "+3", "", "255")] + [()],
)
def test_vehicle_invalid_or_missing_unit(rig, args):
    """Invalid or absent unit numbers are rejected before installation."""
    result = rig.run("vehicle-setup.sh", *args)
    assert result.returncode == 2 and not list(rig.root.iterdir())


def test_vehicle_port_mismatch(rig):
    """A port mismatch leaves the existing service state unchanged."""
    env = rig.root / "etc/vehicle-stream.env"
    env.parent.mkdir()
    env.write_text("DEST_PORT=5604\n")
    result = rig.run("vehicle-setup.sh", "--unit", "3")
    assert result.returncode == 2 and "Service state unchanged" in result.stderr
    assert not any(c.startswith("systemctl") for c in rig.calls())


def test_vehicle_single_camera_and_restart(rig):
    """A unique stable camera is configured and the enabled service is restarted."""
    camera = rig.camera()
    result = rig.run("vehicle-setup.sh", "--unit", "3")
    assert result.returncode == 0 and "Done." in result.stdout
    assert f'CAM_DEVICE="{camera}"' in (rig.root / "etc/vehicle-stream.env").read_text()
    calls = rig.calls()
    assert (
        calls.index("systemctl <daemon-reload>")
        < calls.index("systemctl <enable> <vehicle-stream>")
        < calls.index("systemctl <restart> <vehicle-stream>")
    )
    assert str(rig.root) not in (rig.root / "etc/systemd/system/vehicle-stream.service").read_text()


@pytest.mark.parametrize("count", [0, 2])
def test_vehicle_ambiguous_camera_does_not_start(rig, count):
    """No or multiple camera candidates install safely without starting a service."""
    for i in range(count):
        rig.camera(f"camera{i}-video-index0")
    result = rig.run("vehicle-setup.sh", "--unit", "3")
    assert (
        result.returncode == 3
        and "CAM_DEVICE=\n" in (rig.root / "etc/vehicle-stream.env").read_text()
    )
    assert all(f"camera{i}-video-index0" in result.stdout for i in range(count))
    config = (rig.root / "etc/vehicle-stream.env").read_text()
    assert "v4l2-ctl -d <camera path> --list-formats-ext" in config
    assert '-d ""' not in config
    assert not any(
        c.startswith(("systemctl <enable>", "systemctl <start>", "systemctl <restart>"))
        for c in rig.calls()
    )
    wrapper = rig.root / "usr/local/bin/vehicle-stream.sh"
    run = subprocess.run(
        [str(wrapper)],
        capture_output=True,
        text=True,
        check=False,
        env={"PATH": os.environ["PATH"]},
    )
    assert run.returncode == 1 and "CAM_DEVICE is not set in /etc/vehicle-stream.env" in run.stderr


@pytest.mark.parametrize("camera", ["/dev/video0", "/dev/v4l/by-id/missing"])
def test_vehicle_invalid_camera(rig, camera):
    """Explicit camera paths must be stable and exist before installation."""
    result = rig.run("vehicle-setup.sh", "--unit", "3", "--camera", camera)
    assert result.returncode == 2 and not list(rig.root.iterdir())


@pytest.mark.parametrize("camera", ["/dev/video0", "/dev/serial/by-id/x"])
def test_vehicle_existing_camera_outside_stable_prefixes(rig, camera):
    """Existing devices outside the stable camera prefixes are rejected."""
    path = rig.root / camera.lstrip("/")
    path.parent.mkdir(parents=True)
    path.touch()
    before = set(rig.root.rglob("*"))
    result = rig.run("vehicle-setup.sh", "--unit", "3", "--camera", camera)
    assert result.returncode == 2
    assert "must start with /dev/v4l/by-id/ or /dev/v4l/by-path/" in result.stderr
    assert set(rig.root.rglob("*")) == before
    assert not any(c.startswith(("apt-get ", "systemctl ")) for c in rig.calls())


@pytest.mark.parametrize("source", ["option", "environment"])
def test_vehicle_existing_config_warns_ignored_camera(rig, source):
    """An explicit camera cannot silently override an existing configuration."""
    env = rig.root / "etc/vehicle-stream.env"
    env.parent.mkdir()
    contents = b"DEST_PORT=5603\nCAM_DEVICE=/dev/v4l/by-id/original\n"
    env.write_bytes(contents)
    camera = rig.camera("chosen")
    args = ("--camera", camera) if source == "option" else ()
    variables = {"CAMERA": camera} if source == "environment" else {}
    result = rig.run("vehicle-setup.sh", "--unit", "3", *args, **variables)
    assert result.returncode == 0 and env.read_bytes() == contents
    assert (
        "Ignoring --camera: /etc/vehicle-stream.env already exists; edit CAM_DEVICE there."
        in result.stderr
    )


def test_vehicle_explicit_camera_overrides_environment(rig):
    """The camera option selects an existing stable path ahead of CAMERA."""
    camera = rig.camera("chosen", "by-path")
    result = rig.run("vehicle-setup.sh", "--unit", "3", "--camera", camera, CAMERA="/dev/video0")
    assert (
        result.returncode == 0
        and f'CAM_DEVICE="{camera}"' in (rig.root / "etc/vehicle-stream.env").read_text()
    )


def test_vehicle_existing_config_warns_and_is_preserved(rig):
    """An existing unstable camera is warned about without changing the configuration."""
    camera = rig.camera()
    env = rig.root / "etc/vehicle-stream.env"
    env.parent.mkdir()
    contents = b"DEST_PORT=5603\nCAM_DEVICE=/dev/video0\n"
    env.write_bytes(contents)
    result = rig.run("vehicle-setup.sh", "--unit", "3")
    assert result.returncode == 0 and env.read_bytes() == contents
    assert "Warning" in result.stderr and camera in result.stderr


@pytest.mark.parametrize("active", ["0", "1"])
def test_vehicle_unitless_preserves_stopped_state(rig, active):
    """A unitless rerun restarts only an active service and never changes enablement."""
    env = rig.root / "etc/vehicle-stream.env"
    env.parent.mkdir()
    env.write_text("DEST_PORT=5603\nCAM_DEVICE=/dev/v4l/by-id/camera\n")
    result = rig.run("vehicle-setup.sh", ACTIVE=active)
    assert result.returncode == 0 and "Done." in result.stdout
    assert ("systemctl <restart> <vehicle-stream>" in rig.calls()) == (active == "1")
    assert not any(c.startswith("systemctl <enable>") for c in rig.calls())


def test_vehicle_inactive_after_restart_fails(rig):
    """A service that fails after restart cannot report a successful installation."""
    rig.camera()
    result = rig.run("vehicle-setup.sh", "--unit", "3", ACTIVE="0")
    assert (
        result.returncode == 1
        and "Done." not in result.stdout
        and "did NOT succeed" in result.stderr
    )
    assert "systemctl <--no-pager> <--lines=30> <status> <vehicle-stream>" in rig.calls()


def test_vehicle_environment_camera_and_empty_warning(rig):
    """CAMERA selects a stable path; an existing empty setting is preserved and warned about."""
    camera = rig.camera("choice", "by-path")
    result = rig.run("vehicle-setup.sh", "--unit", "3", CAMERA=camera)
    assert result.returncode == 0
    env = rig.root / "etc/vehicle-stream.env"
    assert f'CAM_DEVICE="{camera}"' in env.read_text()
    contents = b"DEST_PORT=5603\nCAM_DEVICE=\n"
    env.write_bytes(contents)
    result = rig.run("vehicle-setup.sh", "--unit", "3")
    assert result.returncode == 0 and env.read_bytes() == contents and "Warning" in result.stderr


def test_vehicle_unitless_restart_self_check(rig):
    """A unitless active service that fails after restart reports failure."""
    env = rig.root / "etc/vehicle-stream.env"
    env.parent.mkdir()
    env.write_text("DEST_PORT=5603\nCAM_DEVICE=/dev/v4l/by-id/camera\n")
    result = rig.run("vehicle-setup.sh", ACTIVE_SEQUENCE="10")
    assert (
        result.returncode == 1
        and "Done." not in result.stdout
        and "did NOT succeed" in result.stderr
    )
