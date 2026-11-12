#!/usr/bin/env python3
"""Build the GitHub release text for one version of obs-optical-flow.

    release-notes.py --version 1.1.0 [--check]
                     [--repo owner/name] [--previous-tag v1.0.0] [--obs-version 32.1.2]

--allow-undated accepts an "Unreleased" entry (release previews only).
--check only validates that CHANGELOG.md has a dated section for the
version (used to fail a release early, before anything is built).
Otherwise the filled-in .github/release-template.md is printed to stdout.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def changelog_section(version: str) -> tuple[str, str]:
    """Return (heading suffix, body) of the '## [version]' section."""
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(
        rf"^## \[{re.escape(version)}\](?P<head>[^\n]*)\n(?P<body>.*?)(?=^## \[|\Z)",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        sys.exit(f"CHANGELOG.md has no '## [{version}]' section — add one before releasing")
    return m.group("head").strip(" -"), m.group("body").strip()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--repo", default="")
    ap.add_argument("--previous-tag", default="")
    ap.add_argument("--obs-version", default="")
    ap.add_argument(
        "--allow-undated",
        action="store_true",
        help="accept an undated (Unreleased) entry, for release previews",
    )
    args = ap.parse_args()

    date, body = changelog_section(args.version)
    if not args.allow_undated and not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
        sys.exit(
            f"CHANGELOG.md section [{args.version}] is dated '{date or '(none)'}' — "
            "set the release date (YYYY-MM-DD) before publishing"
        )
    if not body:
        sys.exit(f"CHANGELOG.md section [{args.version}] is empty")
    if args.check:
        print(f"CHANGELOG.md: section [{args.version}] dated {date} OK")
        return

    tag = f"v{args.version}"
    if args.previous_tag and args.repo:
        compare = (
            f"**Full diff:** https://github.com/{args.repo}/compare/{args.previous_tag}...{tag}"
        )
    elif args.repo:
        compare = f"**Commits:** https://github.com/{args.repo}/commits/{tag}"
    else:
        compare = ""

    notes = (ROOT / ".github" / "release-template.md").read_text(encoding="utf-8")
    for key, value in {
        "VERSION": args.version,
        "DATE": date,
        "OBS_VERSION": args.obs_version or "32.1.2",
        "REPO": args.repo or "ShyVortex/obs-optical-flow",
        "CHANGELOG": body,
        "COMPARE": compare,
    }.items():
        notes = notes.replace("{{" + key + "}}", value)
    sys.stdout.write(notes)


if __name__ == "__main__":
    main()
