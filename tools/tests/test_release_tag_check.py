"""Tests for tools/qgc_tools/release_tag_check.py."""

from __future__ import annotations

import pytest
from qgc_tools import release_tag_check

from ._helpers import completed


def _stub_git(monkeypatch: pytest.MonkeyPatch, stdout: str, returncode: int = 0) -> None:
    monkeypatch.setattr(release_tag_check, "run_git", lambda *a, **k: completed(stdout, returncode))


def test_lists_containing_stable_branches(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_git(monkeypatch, "  origin/Stable_V5.1\n\n  origin/Stable_V5.0\n")
    assert release_tag_check.stable_branches_containing() == [
        "origin/Stable_V5.1",
        "origin/Stable_V5.0",
    ]


def test_empty_when_not_on_stable(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_git(monkeypatch, "")
    assert release_tag_check.stable_branches_containing() == []


def test_raises_when_git_fails(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_git(monkeypatch, "", returncode=128)
    with pytest.raises(release_tag_check.GitQueryError):
        release_tag_check.stable_branches_containing()


def test_check_release_tag_does_not_require_git(monkeypatch: pytest.MonkeyPatch) -> None:
    _stub_git(monkeypatch, "", returncode=128)
    assert release_tag_check.check_release_tag("v1.12.0") is True


@pytest.mark.parametrize(
    ("ref_name", "expected"),
    [
        ("v5.1.4", True),
        ("v5.10.12", True),
        ("v5.1.5-rc1", False),
        ("v5.2.0-dev", False),
        ("5.1.4", False),
        ("master", False),
        ("refs/tags/v1.12.0", False),
        ("v1.12.0-rc1", False),
    ],
)
def test_is_release_tag(ref_name: str, expected: bool) -> None:
    assert release_tag_check.is_release_tag(ref_name) is expected


@pytest.mark.parametrize(
    ("ref_name", "stdout", "expected"),
    [
        ("v5.1.4", "origin/Stable_V5.1\n", True),
        ("v1.12.0", "", True),
        ("v5.1.5-rc1", "origin/Stable_V5.1\n", False),
    ],
)
def test_check_release_tag(
    monkeypatch: pytest.MonkeyPatch, ref_name: str, stdout: str, expected: bool
) -> None:
    _stub_git(monkeypatch, stdout)
    assert release_tag_check.check_release_tag(ref_name) is expected


def test_master_v1120_is_publishable_with_no_stable_refs(git_repo, monkeypatch):
    from ._helpers import git

    git(git_repo, "commit", "-q", "--allow-empty", "-m", "release candidate")
    git(git_repo, "tag", "v1.12.0")
    assert git(git_repo, "branch", "--show-current") == "master"
    assert git(git_repo, "for-each-ref", "refs/remotes/origin/Stable*") == ""
    monkeypatch.chdir(git_repo)
    assert release_tag_check.check_release_tag("v1.12.0") is True
