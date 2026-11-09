"""CI validates the real fork overlay in Debug and packages it in Release."""

import pytest
from ci.planning.detect_changes import PLATFORMS, has_relevant_changes

from ..._helpers import get_step, load_workflow


def test_custom_plugin_debug_runs_unit_and_integration_suites():
    job = load_workflow("linux.yml")["jobs"]["custom-plugin"]
    assert job["needs"] == "changes"
    assert job["if"] == "needs.changes.outputs.should_build == 'true'"
    assert "strategy" not in job
    steps = job["steps"]
    setup = get_step(steps, "Setup Build Environment")["with"]
    assert setup["build-type"] == "Debug"
    assert setup["cache-variant"] == "custom"
    assert "cache-key-suffix" not in setup
    assert get_step(steps, "Configure")["with"] == {
        "preset": "Linux-debug",
        "extra-args": "-DQGC_CUSTOM_DIR=custom",
    }
    tests = get_step(steps, "Run and report custom tests")
    assert tests["uses"] == "./.github/actions/test-phase"
    assert tests["if"] == "${{ !cancelled() && steps.build.outcome == 'success' }}"
    assert tests["with"]["suite"] == "custom"
    assert tests["with"]["exclude-labels"] == "Flaky|Network|StockUI"
    suites = load_workflow("linux.yml")["jobs"]["custom-plugin"]
    boot = get_step(suites["steps"], "Verify custom executable")
    assert boot["with"]["binary-path"] == "${{ runner.temp }}/build/Debug/Custom-QGroundControl"
    assert boot["if"] == tests["if"]


def test_release_legs_package_the_real_custom_overlay():
    job = load_workflow("linux.yml")["jobs"]["build"]
    steps = job["steps"]
    assert {entry["package"] for entry in job["strategy"]["matrix"]["include"]} == {
        "Custom-QGroundControl-x86_64",
        "Custom-QGroundControl-aarch64",
    }
    assert "-DQGC_CUSTOM_DIR=custom " in get_step(steps, "Configure")["with"]["extra-args"]
    assert get_step(steps, "Create AppImage")["run"] == "cmake --install . --config Release"
    assert get_step(steps, "Verify AppImage")["with"]["type"] == "appimage"
    assert get_step(steps, "Attest and Upload")["with"]["package-name"] == "${{ matrix.package }}"


@pytest.mark.parametrize("platform", PLATFORMS)
def test_custom_only_changes_build_every_platform(platform):
    assert has_relevant_changes(["custom/src/CustomPlugin.cc"], platform)
