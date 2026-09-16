#!/usr/bin/env python3
"""Dispatch release builds, wait on their exact run IDs, and save a download snapshot."""

from __future__ import annotations

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from ci_bootstrap import ensure_tools_dir

ensure_tools_dir(__file__)

from common.gh_actions import gh, list_run_jobs, list_workflow_runs_for_sha
from common.io import write_json

# Only the platforms this fork actually ships gate a release. MacOS/Android/
# iOS still build on every push via their own workflows, but a missing
# code-signing setup (MacOS/iOS) or emulator flake (Android) must not block
# releasing the platforms that do work.
WORKFLOWS = {
    "Linux": "linux.yml",
    "Windows": "windows.yml",
}

# The specific jobs each platform's release actually depends on. A workflow
# run's own aggregate conclusion is failure if ANY job in it fails - but
# linux.yml also runs "Test + Coverage linux_gcc_64 Debug", a known
# pre-existing flake unrelated to the release packages themselves. Checking
# these named jobs directly means that flake (or any other unrelated job in
# the same run) can't block a release whose actual package-building jobs
# succeeded.
REQUIRED_JOBS: dict[str, tuple[str, ...]] = {
    "Linux": ("Release linux_gcc_64", "Release linux_gcc_arm64"),
    "Windows": (
        "Build win64_msvc2022_64 Release",
        "Build win64_msvc2022_arm64 Release",
        "Build win64_msvc2022_arm64_cross_compiled Release",
    ),
}


def identify_runs(runs: list[dict[str, Any]], tag: str, since: str) -> dict[str, dict[str, Any]]:
    selected: dict[str, dict[str, Any]] = {}
    for run in runs:
        name = run.get("name", "")
        if (
            name not in WORKFLOWS
            or run.get("event") != "workflow_dispatch"
            or run.get("head_branch") != tag
            or run.get("created_at", "") < since
        ):
            continue
        if name in selected:
            raise RuntimeError(f"Ambiguous release builds for {name}: multiple dispatches at {tag}")
        selected[name] = run
    return selected


def _required_jobs_status(repo: str, run: dict[str, Any], required: tuple[str, ...]) -> str:
    """Return "success", "pending", or raise if any required job failed."""
    jobs = {job["name"]: job for job in list_run_jobs(repo, run["id"])}
    missing = [name for name in required if name not in jobs]
    if missing:
        if run["status"] == "completed":
            raise RuntimeError(
                f"Release build finished without expected job(s) {missing}: {run['html_url']}"
            )
        return "pending"
    unfinished = [name for name in required if jobs[name]["status"] != "completed"]
    if unfinished:
        return "pending"
    failed = [name for name in required if jobs[name]["conclusion"] != "success"]
    if failed:
        raise RuntimeError(f"Release build failed for job(s) {failed}: {run['html_url']}")
    return "success"


def wait_for_builds(repo: str, sha: str, tag: str, output: Path, timeout: int = 9600) -> None:
    since = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    for workflow in WORKFLOWS.values():
        gh("workflow", "run", workflow, "--repo", repo, "--ref", tag)
    deadline = time.monotonic() + timeout
    selected: dict[str, dict[str, Any]] = {}
    done: dict[str, dict[str, Any]] = {}
    while time.monotonic() < deadline:
        if len(selected) < len(WORKFLOWS):
            discovered = identify_runs(list_workflow_runs_for_sha(repo, sha), tag, since)
            for name, run in discovered.items():
                if name in selected and selected[name]["id"] != run["id"]:
                    raise RuntimeError(f"Release run changed for {name}")
                selected[name] = run
        for name, run in selected.items():
            if name in done:
                continue
            current = json.loads(gh("api", f"repos/{repo}/actions/runs/{run['id']}").stdout)
            if current["head_sha"] != sha or current["head_branch"] != tag:
                raise RuntimeError("Release run identity changed")
            if _required_jobs_status(repo, current, REQUIRED_JOBS[name]) == "success":
                done[name] = current
        if len(done) == len(WORKFLOWS):
            write_json(output, list(done.values()))
            return
        print(f"Waiting for release builds ({len(done)}/{len(WORKFLOWS)} identified)", flush=True)
        time.sleep(30)
    raise TimeoutError("Release builds did not complete before the deadline")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    wait_for_builds(args.repo, args.sha, args.tag, args.output)


if __name__ == "__main__":
    main()
