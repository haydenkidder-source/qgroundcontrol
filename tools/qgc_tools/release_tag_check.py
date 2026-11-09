"""Decide whether the current tag ref is a publishable release tag.

This fork releases directly from master: a release tag must match ``vX.Y.Z``
exactly. Anything else (version markers on master, ``-rc``/``-dev`` suffixes,
manual dispatches on odd refs) must not trigger the release pipeline or overwrite
the public S3 ``latest/`` downloads. The name check matters because
``workflow_dispatch`` on a tag bypasses the workflows' ``push.tags`` glob.
Stable branch queries remain available for callers with a separate branch policy.
"""

from __future__ import annotations

import re

from common.gh_actions import gh_warning
from common.git import run_git

_STABLE_BRANCH_GLOB = "origin/Stable*"
_RELEASE_TAG_RE = re.compile(r"^v\d+\.\d+\.\d+$")


class GitQueryError(RuntimeError):
    """git branch --contains failed; the verdict is unknown, not "false"."""


def is_release_tag(ref_name: str) -> bool:
    return _RELEASE_TAG_RE.fullmatch(ref_name) is not None


def stable_branches_containing(ref: str = "HEAD") -> list[str]:
    result = run_git("branch", "-r", "--contains", ref, "--list", _STABLE_BRANCH_GLOB)
    if result.returncode != 0:
        raise GitQueryError(result.stderr.strip() or f"git exited {result.returncode}")
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def check_release_tag(ref_name: str, context: str = "") -> bool:
    """Return whether *ref_name* is publishable, warning (with *context*) when it is not.

    No Stable branch ancestry is required for this fork.
    """
    suffix = f"; {context}" if context else ""
    if not is_release_tag(ref_name):
        gh_warning(f"Tag {ref_name} is not a vX.Y.Z release tag{suffix}")
        return False

    print(f"Tag {ref_name} is a publishable release tag")
    return True
