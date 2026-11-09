"""Release dispatch identity must not silently select another build at the same SHA."""

import json
from unittest.mock import patch

import pytest
from ci.publish.release_builds import REQUIRED_JOBS, WORKFLOWS, identify_runs, wait_for_builds


def run(name="Linux", **kwargs):
    return {
        "name": name,
        "repository": {"full_name": "o/r"},
        "id": list(WORKFLOWS).index(name) + 1,
        "event": "workflow_dispatch",
        "head_branch": "v1",
        "head_sha": "abc",
        "created_at": "2026-09-14T00:00:01Z",
        "status": "completed",
        "conclusion": "success",
        "run_attempt": 1,
        **kwargs,
    }


def _jobs_for(name: str, *, all_success: bool = True) -> list[dict]:
    conclusion = "success" if all_success else "failure"
    return [
        {"name": job_name, "status": "completed", "conclusion": conclusion}
        for job_name in REQUIRED_JOBS[name]
    ]


def test_selection_ignores_old_dispatches_and_other_refs():
    runs = [
        run(created_at="2026-09-13T00:00:00Z"),
        run(head_branch="master"),
        run(event="push"),
        run(),
    ]
    assert identify_runs(runs, "v1", "2026-09-14T00:00:00Z") == {"Linux": run()}


def test_ambiguous_dispatches_fail():
    with pytest.raises(RuntimeError, match="Ambiguous"):
        identify_runs([run(), run(id=2)], "v1", "2026-09-14T00:00:00Z")


def test_completed_dispatches_save_exact_ids(tmp_path):
    from subprocess import CompletedProcess

    runs = [run(name, created_at="2099-01-01T00:00:00Z") for name in WORKFLOWS]

    def gh(*args, **kwargs):
        if args[0] == "api":
            return CompletedProcess([], 0, json.dumps(runs[int(args[1].split("/")[-1]) - 1]))
        return CompletedProcess([], 0, "")

    def jobs_for_run(repo, run_id):
        return _jobs_for(runs[int(run_id) - 1]["name"])

    with (
        patch("ci.publish.release_builds.gh", side_effect=gh),
        patch("ci.publish.release_builds.list_workflow_runs_for_sha", return_value=runs),
        patch("ci.publish.release_builds.list_run_jobs", side_effect=jobs_for_run),
    ):
        wait_for_builds("o/r", "abc", "v1", tmp_path / "runs.json")
    assert json.loads((tmp_path / "runs.json").read_text()) == runs


def test_run_with_failed_conclusion_but_successful_required_jobs_counts_as_done(tmp_path):
    """A workflow run's aggregate conclusion is failure if ANY job in it fails - e.g.
    linux.yml's own unrelated flaky test job - but that must not block a release whose
    actual release-relevant jobs succeeded."""
    from subprocess import CompletedProcess

    runs = [
        run(name, created_at="2099-01-01T00:00:00Z", conclusion="failure") for name in WORKFLOWS
    ]

    def gh(*args, **kwargs):
        if args[0] == "api":
            return CompletedProcess([], 0, json.dumps(runs[int(args[1].split("/")[-1]) - 1]))
        return CompletedProcess([], 0, "")

    def jobs_for_run(repo, run_id):
        return _jobs_for(runs[int(run_id) - 1]["name"])

    with (
        patch("ci.publish.release_builds.gh", side_effect=gh),
        patch("ci.publish.release_builds.list_workflow_runs_for_sha", return_value=runs),
        patch("ci.publish.release_builds.list_run_jobs", side_effect=jobs_for_run),
    ):
        wait_for_builds("o/r", "abc", "v1", tmp_path / "runs.json")
    assert json.loads((tmp_path / "runs.json").read_text()) == runs


def test_failed_dispatch_does_not_create_snapshot(tmp_path):
    from subprocess import CompletedProcess

    failed = run(conclusion="failure", html_url="https://example.test/run")
    with (
        patch("ci.publish.release_builds.identify_runs", return_value={"Linux": failed}),
        patch("ci.publish.release_builds.list_workflow_runs_for_sha", return_value=[]),
        patch(
            "ci.publish.release_builds.gh", return_value=CompletedProcess([], 0, json.dumps(failed))
        ),
        patch(
            "ci.publish.release_builds.list_run_jobs",
            return_value=_jobs_for("Linux", all_success=False),
        ),
        pytest.raises(RuntimeError, match="Release build failed"),
    ):
        wait_for_builds("o/r", "abc", "v1", tmp_path / "runs.json")
    assert not (tmp_path / "runs.json").exists()


def test_successful_jobs_wait_for_completed_run_before_snapshot(tmp_path):
    from subprocess import CompletedProcess

    runs = [run(name, created_at="2099-01-01T00:00:00Z") for name in WORKFLOWS]
    polls = {item["id"]: 0 for item in runs}
    output = tmp_path / "runs.json"

    def gh(*args, **kwargs):
        if args[0] != "api":
            return CompletedProcess([], 0, "")
        run_id = int(args[1].split("/")[-1])
        polls[run_id] += 1
        current = dict(runs[run_id - 1])
        if polls[run_id] == 1:
            current.update(status="in_progress", conclusion=None)
        return CompletedProcess([], 0, json.dumps(current))

    def sleep(_seconds):
        assert not output.exists()

    with (
        patch("ci.publish.release_builds.gh", side_effect=gh),
        patch("ci.publish.release_builds.list_workflow_runs_for_sha", return_value=runs),
        patch(
            "ci.publish.release_builds.list_run_jobs",
            side_effect=lambda repo, run_id: _jobs_for(runs[run_id - 1]["name"]),
        ),
        patch("ci.publish.release_builds.time.sleep", side_effect=sleep) as pause,
    ):
        wait_for_builds("o/r", "abc", "v1", output)
    pause.assert_called_once()
    assert json.loads(output.read_text()) == runs
    assert all(count == 2 for count in polls.values())


@pytest.mark.parametrize(
    "field", ["id", "name", "head_sha", "head_branch", "event", "repository", "run_attempt"]
)
def test_changed_producer_identity_never_creates_snapshot(tmp_path, field):
    from subprocess import CompletedProcess

    selected = run(created_at="2099-01-01T00:00:00Z")
    current = {**selected, field: 9 if field in ("id", "run_attempt") else "wrong"}
    if field == "repository":
        current[field] = {"full_name": "wrong/repo"}
    with (
        patch("ci.publish.release_builds.list_workflow_runs_for_sha", return_value=[selected]),
        patch(
            "ci.publish.release_builds.gh",
            return_value=CompletedProcess([], 0, json.dumps(current)),
        ),
        pytest.raises(RuntimeError, match="identity changed"),
    ):
        wait_for_builds("o/r", "abc", "v1", tmp_path / "runs.json")
    assert not (tmp_path / "runs.json").exists()


def test_dispatches_only_expected_platforms_on_the_exact_tag(tmp_path):
    from subprocess import CompletedProcess

    with (
        patch("ci.publish.release_builds.gh", return_value=CompletedProcess([], 0, "")) as gh,
        pytest.raises(TimeoutError),
    ):
        wait_for_builds("o/r", "abc", "v1.12.0", tmp_path / "runs.json", timeout=0)
    assert gh.call_args_list == [
        (("workflow", "run", filename, "--repo", "o/r", "--ref", "v1.12.0"),)
        for filename in WORKFLOWS.values()
    ]
