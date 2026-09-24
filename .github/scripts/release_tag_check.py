#!/usr/bin/env python3
"""Decide whether the current tag ref is a publishable release tag.

A release tag must match ``vX.Y.Z`` exactly. Anything else (version markers on
master, ``-rc``/``-dev`` suffixes, manual dispatches on odd refs) must not
trigger the release pipeline or overwrite the public S3 ``latest/`` downloads.
The name check matters because ``workflow_dispatch`` on a tag bypasses the
workflows' ``push.tags`` glob.

This fork's semantic-release config (.releaserc.json) cuts every release tag
directly from master - unlike upstream QGroundControl, it has no Stable_V*
release-branch convention, so this intentionally does not check tag/branch
reachability the way upstream's release tooling does.

Emits ``release_tag=true|false`` to ``GITHUB_OUTPUT``.

Usage:
    python3 .github/scripts/release_tag_check.py [--context "skipping build"]
"""

from __future__ import annotations

import argparse
import os
import re

from ci_bootstrap import ensure_tools_dir

ensure_tools_dir(__file__)

from common.gh_actions import gh_warning, write_github_output

_RELEASE_TAG_RE = re.compile(r"^v\d+\.\d+\.\d+$")


def is_release_tag(ref_name: str) -> bool:
    return _RELEASE_TAG_RE.fullmatch(ref_name) is not None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--context",
        default="",
        help="Appended to the warning when the ref is not a publishable release tag",
    )
    args = parser.parse_args(argv)
    suffix = f"; {args.context}" if args.context else ""
    ref_name = os.environ.get("GITHUB_REF_NAME", "HEAD")

    if not is_release_tag(ref_name):
        gh_warning(f"Tag {ref_name} is not a vX.Y.Z release tag{suffix}")
        write_github_output({"release_tag": "false"})
        return 0

    print(f"Tag {ref_name} is a publishable release tag")
    write_github_output({"release_tag": "true"})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
