"""Tests for release_tag_check.py."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
import release_tag_check

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    ("ref_name", "expected"),
    [
        ("v5.1.4", True),
        ("v5.10.12", True),
        ("v5.1.5-rc1", False),
        ("v5.2.0-dev", False),
        ("5.1.4", False),
    ],
)
def test_is_release_tag(ref_name: str, expected: bool) -> None:
    assert release_tag_check.is_release_tag(ref_name) is expected


@pytest.mark.parametrize(
    ("ref_name", "expected"),
    [
        ("v5.1.4", "true"),
        ("v5.1.5-rc1", "false"),
        ("not-a-tag", "false"),
    ],
)
def test_main_writes_release_tag_output(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, ref_name: str, expected: str
) -> None:
    monkeypatch.setenv("GITHUB_REF_NAME", ref_name)
    output = tmp_path / "output"
    monkeypatch.setenv("GITHUB_OUTPUT", str(output))
    assert release_tag_check.main([]) == 0
    assert f"release_tag={expected}" in output.read_text()
