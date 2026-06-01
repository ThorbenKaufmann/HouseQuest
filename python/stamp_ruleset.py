#!/usr/bin/env python3
"""Propagate section-level annotations to individual bullet points in ruleset.md.

Annotation format (matching the existing style in the file):
    _[xp:0,hp:10,l:1]_

How it works:
  - A standalone line  _[xpN,hpN,lN]_  after a heading sets the *section default*
    for all bullets that follow, until the next heading / annotation.
  - Bullets that already carry their own annotation are left untouched.
  - Bullets without an annotation get the current section default stamped on them.
  - The standalone section-default lines are removed afterwards (the information
    now lives on each bullet directly).

Usage:
    python stamp_ruleset.py [input.md]             # edits in place
    python stamp_ruleset.py [input.md] --dry-run   # preview only
    python stamp_ruleset.py [input.md] --keep-markers  # keep standalone lines
"""

import re
import sys
from pathlib import Path

# ── regex ─────────────────────────────────────────────────────────────────────

# matches both  _[xp0,hp10,l1]_  and  _[xp:0,hp:10,l:1]_
ANNOT_RE  = re.compile(r'_\[xp:?(\d+),hp:?(\d+),l:?(\d+)\]_')
BULLET_RE = re.compile(r'^([-*]\s+)')

GLOBAL_DEFAULT = (0, 10, 1)   # xp, hp, level


def _fmt(xp: int, hp: int, level: int) -> str:
    return f"_[xp:{xp},hp:{hp},l:{level}]_"


def stamp(md_path: Path, dry_run: bool = False, keep_markers: bool = False):
    lines = md_path.read_text(encoding="utf-8").splitlines(keepends=True)

    result        = []
    section_def   = GLOBAL_DEFAULT
    added = skipped = markers_removed = 0

    for line in lines:
        stripped = line.rstrip("\n\r")

        # Heading resets nothing — the annotation line right below it sets the default
        is_heading = stripped.lstrip().startswith("#")

        # Standalone annotation line (no bullet prefix)
        standalone = ANNOT_RE.fullmatch(stripped.strip())
        if standalone and not BULLET_RE.match(stripped):
            section_def = (int(standalone.group(1)),
                           int(standalone.group(2)),
                           int(standalone.group(3)))
            if keep_markers:
                result.append(line)
            else:
                markers_removed += 1
            continue

        # Bullet line
        if BULLET_RE.match(stripped):
            if ANNOT_RE.search(stripped):
                skipped += 1          # already annotated — leave it
                result.append(line)
            else:
                tag     = _fmt(*section_def)
                new     = stripped.rstrip() + "  " + tag + "\n"
                added  += 1
                result.append(new)
            continue

        result.append(line)

    if not dry_run:
        md_path.write_text("".join(result), encoding="utf-8")

    verb = "Would" if dry_run else ""
    if added:     print(f"{'Would stamp' if dry_run else 'Stamped'} {added} bullets")
    if skipped:   print(f"Skipped {skipped} already-annotated bullets")
    if markers_removed and not keep_markers:
        print(f"{'Would remove' if dry_run else 'Removed'} {markers_removed} standalone markers")
    if dry_run:
        print("(dry run — no file written)")


if __name__ == "__main__":
    args         = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run      = "--dry-run"      in sys.argv
    keep_markers = "--keep-markers" in sys.argv

    md_path = Path(args[0]) if args else Path("Doc/ruleset.md")
    stamp(md_path, dry_run=dry_run, keep_markers=keep_markers)
