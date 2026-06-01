#!/usr/bin/env python3
"""Extract rules applicable at a given game level from ruleset.md.

Rule IDs always match the *original* full-ruleset numbering — 2.1.1 and
2.1.3 stay stable even when 2.1.2 (a higher-level rule) is filtered out.
This prevents confusion when the visible rule list changes as levels rise.

Usage:
    python level_rules.py <level> [Doc/ruleset.md]

    level     — integer; rules with l ≤ level are included
    input.md  — defaults to Doc/ruleset.md (run from project root)

Output: Markdown to stdout — redirect to save:
    python level_rules.py 1 > Doc/rules_level1.md
    python level_rules.py 2
"""

import re
import sys
from pathlib import Path

# ── regex / parser (mirrors ruleset_to_yaml.py, no pyyaml needed here) ───────

HEADING_RE    = re.compile(r'^(#{1,5})\s+(.+)$')
BULLET_RE     = re.compile(r'^[-*]\s+(.+)$')
BOLD_RE       = re.compile(r'\*{1,2}(.+?)\*{1,2}')
ANNOT_RE      = re.compile(r'_\[xp:?(\d+),hp:?(\d+),l:?(\d+)\]_')
NUM_PREFIX_RE = re.compile(r'^(\d+(?:\.\d+)*)[.\s]')

DEFAULT_XP, DEFAULT_HP, DEFAULT_LEVEL = 0, 10, 1


def _strip_bold(text: str) -> str:
    return BOLD_RE.sub(r'\1', text).strip()


def _section_num(title: str) -> str | None:
    m = NUM_PREFIX_RE.match(title.strip())
    return m.group(1) if m else None


def _make_rule(text: str, sec_xp: int, sec_hp: int, sec_level: int) -> dict:
    m = ANNOT_RE.search(text)
    if m:
        xp, hp, level = int(m.group(1)), int(m.group(2)), int(m.group(3))
        text = ANNOT_RE.sub("", text).strip()
    else:
        xp, hp, level = sec_xp, sec_hp, sec_level
    return {"text": _strip_bold(text), "xp": xp, "hp": hp, "level": level}


def parse_markdown(content: str) -> list:
    root  = []
    stack = []
    sec_xp, sec_hp, sec_level = DEFAULT_XP, DEFAULT_HP, DEFAULT_LEVEL

    for raw in content.splitlines():
        line = raw.strip()
        if not line or line.startswith('---') or line.startswith('>') or line.startswith('|'):
            continue

        h_match = HEADING_RE.match(line)
        b_match = BULLET_RE.match(line)
        annot   = ANNOT_RE.fullmatch(line)

        if annot and not b_match:
            sec_xp, sec_hp, sec_level = int(annot.group(1)), int(annot.group(2)), int(annot.group(3))
            continue

        if h_match:
            depth = len(h_match.group(1))
            title = h_match.group(2).strip()
            if depth == 1:
                continue
            node = {"title": title}
            while stack and stack[-1][0] >= depth:
                stack.pop()
            if stack:
                stack[-1][1].setdefault("children", []).append(node)
            else:
                root.append(node)
            stack.append((depth, node))

        elif b_match and stack:
            rule = _make_rule(b_match.group(1), sec_xp, sec_hp, sec_level)
            stack[-1][1].setdefault("rules", []).append(rule)

    return root


# ── renderer ──────────────────────────────────────────────────────────────────

def render(nodes: list, max_level: int, depth: int = 2) -> tuple[list[str], int, int]:
    """Recursively render applicable rules, returning (lines, shown, total)."""
    lines        = []
    shown_total  = 0
    all_total    = 0

    for node in nodes:
        title   = node["title"]
        sec_num = _section_num(title)

        # Assign original IDs from the *full* rule list for this section,
        # but only render rules whose level is within range.
        applicable = []
        all_rules  = node.get("rules", [])
        all_total += len(all_rules)
        for i, rule in enumerate(all_rules, start=1):
            if rule["level"] <= max_level:
                rule_id = f"{sec_num}.{i}" if sec_num else f"?.{i}"
                applicable.append((rule_id, rule))
                shown_total += 1

        child_lines, child_shown, child_all = render(
            node.get("children", []), max_level, depth + 1
        )
        shown_total += child_shown
        all_total   += child_all

        # Skip sections that have nothing to show at this level
        if not applicable and not child_lines:
            continue

        lines.append(f"{'#' * depth} {title}\n")
        for rule_id, rule in applicable:
            lines.append(f"- **{rule_id}** {rule['text']}\n")
        if child_lines:
            if applicable:
                lines.append("\n")
            lines.extend(child_lines)
        lines.append("\n")

    return lines, shown_total, all_total


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    if len(sys.argv) < 2 or not sys.argv[1].lstrip('-').isdigit():
        print("Usage: level_rules.py <level> [Doc/ruleset.md]", file=sys.stderr)
        sys.exit(1)

    max_level = int(sys.argv[1])
    md_path   = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("Doc/ruleset.md")

    if not md_path.exists():
        print(f"Not found: {md_path}", file=sys.stderr)
        sys.exit(1)

    tree = parse_markdown(md_path.read_text(encoding="utf-8"))
    lines, shown, total = render(tree, max_level)

    print(f"# HouseQuest Ruleset — Level {max_level}\n")
    print(f"> Rules applicable at game level ≤ {max_level}  "
          f"({shown} of {total} rules).")
    print(f"> IDs match the full ruleset — skipped numbers are higher-level rules.\n")
    print("---\n")
    print("".join(lines), end="")

    print(f"<!-- {shown}/{total} rules at level {max_level} -->", file=sys.stderr)


if __name__ == "__main__":
    main()
