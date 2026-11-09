"""Validate and enumerate the artifacts published with a QGC release."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import _bootstrap as _bootstrap
from common.gh_actions import gh_error
from common.io import read_json, verify_sha256_sidecar, write_text_if_changed

# Only Linux and Windows packages are required; see release_builds.py's
# WORKFLOWS comment for why MacOS/Android/iOS don't gate a release.
REQUIRED_PACKAGES: tuple[tuple[str, str], ...] = (
    ("Linux x86_64 AppImage", "Custom-QGroundControl-x86_64/*.AppImage"),
    ("Linux aarch64 AppImage", "Custom-QGroundControl-aarch64/*.AppImage"),
    ("Windows AMD64 installer", "Custom-QGroundControl-installer-AMD64/*.exe"),
    ("Windows ARM64 installer", "Custom-QGroundControl-installer-ARM64/*.exe"),
    (
        "Windows AMD64/ARM64 installer",
        "Custom-QGroundControl-installer-AMD64-ARM64/*.exe",
    ),
)

REQUIRED_PLATFORM_SBOMS: tuple[str, ...] = (
    "Custom-QGroundControl-x86_64.sbom.spdx.json",
    "Custom-QGroundControl-aarch64.sbom.spdx.json",
    "Custom-QGroundControl-installer-AMD64-windows.sbom.spdx.json",
    "Custom-QGroundControl-installer-ARM64-windows.sbom.spdx.json",
    "Custom-QGroundControl-installer-AMD64-ARM64-windows.sbom.spdx.json",
)

REQUIRED_DEPENDENCY_SBOMS: tuple[str, ...] = (
    "Custom-QGroundControl-x86_64.dependencies.cdx.json",
    "Custom-QGroundControl-aarch64.dependencies.cdx.json",
    "Custom-QGroundControl-installer-AMD64-windows.dependencies.cdx.json",
    "Custom-QGroundControl-installer-ARM64-windows.dependencies.cdx.json",
    "Custom-QGroundControl-installer-AMD64-ARM64-windows.dependencies.cdx.json",
)


def _require_unique(root: Path, pattern: str, label: str) -> Path:
    matches = sorted(path for path in root.glob(pattern) if path.is_file())
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one {label} matching {pattern!r}; found {len(matches)}")
    return matches[0]


def _validate_sbom(sbom: Path, *, require_components: bool = False) -> None:
    document = read_json(sbom)
    if not isinstance(document, dict):
        raise ValueError(f"SBOM must contain a JSON object: {sbom}")

    if sbom.name.endswith(".spdx.json"):
        version = document.get("spdxVersion")
        if not isinstance(version, str) or not version.startswith("SPDX-"):
            raise ValueError(f"Invalid SPDX SBOM: {sbom}")
    elif sbom.name.endswith(".cdx.json"):
        if document.get("bomFormat") != "CycloneDX":
            raise ValueError(f"Invalid CycloneDX SBOM: {sbom}")
        if require_components:
            components = document.get("components")
            if not isinstance(components, list) or not components:
                raise ValueError(f"CycloneDX dependency SBOM contains no components: {sbom}")
    else:
        raise ValueError(f"Unsupported SBOM filename: {sbom}")


def collect_release_assets(
    artifacts_dir: Path,
    source_sboms: list[Path],
    *,
    repo: str,
    head_sha: str = "",
    runs_file: Path | None = None,
) -> list[Path]:
    """Return the complete, validated release asset list."""
    if not artifacts_dir.is_dir():
        raise FileNotFoundError(f"Artifact directory does not exist: {artifacts_dir}")

    selected = {}
    if runs_file is not None:
        runs = read_json(runs_file)
        if not isinstance(runs, list):
            raise ValueError("Invalid release run snapshot")
        for name in ("Linux", "Windows"):
            matches = [run for run in runs if isinstance(run, dict) and run.get("name") == name]
            if len(matches) != 1:
                raise ValueError(f"Invalid release run snapshot for {name}")
            run = matches[0]
            if (
                run.get("repository", {}).get("full_name") != repo
                or run.get("head_sha") != head_sha
                or run.get("status") != "completed"
                or run.get("event") != "workflow_dispatch"
                or not isinstance(run.get("id"), int)
                or run["id"] <= 0
                or not isinstance(run.get("run_attempt"), int)
                or run["run_attempt"] <= 0
            ):
                raise ValueError(f"Invalid release run snapshot for {name}")
            selected[name] = run
    assets: list[Path] = []
    for label, pattern in REQUIRED_PACKAGES:
        package = _require_unique(artifacts_dir, pattern, label)
        checksum = package.with_name(f"{package.name}.sha256")
        digest = verify_sha256_sidecar(package, checksum)
        manifest = package.with_name(f"{package.name}.build.json")
        metadata = read_json(manifest)
        if not isinstance(metadata, dict) or metadata.get("schema_version") != 1:
            raise ValueError(f"Invalid build manifest: {manifest}")
        artifact = metadata.get("artifact", {})
        if (
            not isinstance(artifact, dict)
            or artifact.get("sha256") != digest
            or artifact.get("name") != package.name
        ):
            raise ValueError(f"Build manifest does not match package: {manifest}")
        if head_sha and metadata.get("commit") != head_sha:
            raise ValueError(f"Build manifest commit does not match release: {manifest}")
        if selected:
            workflow = "Linux" if package.suffix == ".AppImage" else "Windows"
            run = selected[workflow]
            if (
                metadata.get("repository") != repo
                or metadata.get("workflow") != workflow
                or str(metadata.get("run_id")) != str(run.get("id"))
                or str(metadata.get("run_attempt")) != str(run.get("run_attempt"))
                or metadata.get("ref") != f"refs/tags/{run.get('head_branch')}"
            ):
                raise ValueError(
                    f"Build manifest producer does not match selected release run: {manifest}"
                )
        assets.extend((package, checksum, manifest))

        if package.suffix == ".AppImage":
            zsync = package.with_name(f"{package.name}.zsync")
            if zsync.is_file():
                assets.append(zsync)
            elif repo == "mavlink/qgroundcontrol":
                raise FileNotFoundError(f"AppImage update metadata is missing: {zsync}")

    for sbom_name in REQUIRED_PLATFORM_SBOMS:
        sbom = _require_unique(artifacts_dir, f"**/{sbom_name}", f"SBOM {sbom_name}")
        _validate_sbom(sbom)
        assets.append(sbom)

    for sbom_name in REQUIRED_DEPENDENCY_SBOMS:
        sbom = _require_unique(artifacts_dir, f"**/{sbom_name}", f"SBOM {sbom_name}")
        _validate_sbom(sbom, require_components=True)
        assets.append(sbom)

    for source_sbom in source_sboms:
        if not source_sbom.is_file():
            raise FileNotFoundError(f"Source SBOM does not exist: {source_sbom}")
        _validate_sbom(source_sbom)
        assets.append(source_sbom)

    return sorted(set(assets), key=lambda path: path.as_posix())


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--artifacts-dir", required=True, type=Path)
    parser.add_argument("--source-sbom", action="append", required=True, type=Path)
    parser.add_argument(
        "--head-sha", required=True, help="Require package manifests to match the release commit"
    )
    parser.add_argument("--runs-file", required=True, type=Path, help="Frozen release run snapshot")
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        assets = collect_release_assets(
            args.artifacts_dir,
            args.source_sbom,
            repo=args.repo,
            head_sha=args.head_sha,
            runs_file=args.runs_file,
        )
        write_text_if_changed(args.output, "".join(f"{path}\n" for path in assets))
    except (OSError, ValueError) as exc:
        gh_error(str(exc))
        return 1

    print(f"Validated {len(assets)} release assets; manifest: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
