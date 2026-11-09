"""Release workflow contracts: draft a GitHub Release for a hand-pushed tag, nothing more."""

from __future__ import annotations

from ..._helpers import get_step, load_workflow


def test_release_is_manual_on_master_and_publishes_with_semantic_release() -> None:
    workflow = load_workflow("release.yml")
    assert workflow["on"] == {"workflow_dispatch": None}
    assert workflow["permissions"] == {"contents": "read"}
    assert list(workflow["jobs"]) == ["semantic-release", "wait-for-builds", "upload-artifacts"]
    release = workflow["jobs"]["semantic-release"]
    assert release["if"] == "github.ref == 'refs/heads/master'"
    assert release["permissions"]["contents"] == "write"
    steps = release["steps"]
    assert get_step(steps, "Checkout")["with"] == {"fetch-depth": 0, "persist-credentials": False}
    assert get_step(steps, "Setup Node.js")["with"]["node-version-file"] == "docs/.nvmrc"
    command = get_step(steps, "Run semantic-release")["run"]
    assert "python3 tools/release.py --run" in command
    assert "tools/release/.release-version" in command


def test_release_dispatches_waits_and_downloads_frozen_runs() -> None:
    workflow = load_workflow("release.yml")
    wait = workflow["jobs"]["wait-for-builds"]
    assert wait["needs"] == "semantic-release"
    assert wait["permissions"] == {"contents": "read", "actions": "write"}
    dispatch = get_step(wait["steps"], "Dispatch and wait for release builds")
    assert "tools/ci/publish/release_builds.py" in dispatch["run"]
    assert '--sha "$GITHUB_SHA"' in dispatch["run"]
    upload = workflow["jobs"]["upload-artifacts"]
    assert upload["needs"] == ["semantic-release", "wait-for-builds"]
    assert upload["permissions"] == {"actions": "read", "contents": "write"}
    download = get_step(upload["steps"], "Download artifacts")["run"]
    assert "--runs-cache release-build-runs.json --strict-runs" in download
    assert "--workflows Linux,Windows" in download
    assert "--allow-missing" not in download
    validate = get_step(upload["steps"], "Validate release assets")["run"]
    assert "--runs-file release-build-runs.json" in validate
    assert "tools/ci/publish/release_assets.py" in validate
    assert "gh release upload" in get_step(upload["steps"], "Upload to Release")["run"]


def test_ci_scripts_installs_git_cliff_for_the_release_notes_tests() -> None:
    legs = load_workflow("ci-scripts.yml")["jobs"]["test-ci-scripts"]["strategy"]["matrix"][
        "include"
    ]
    assert legs
    assert all("release" in leg["groups"].split(",") for leg in legs)
