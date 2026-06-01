#!/usr/bin/env python3
"""Convert ruleset.md to YAML and ESP32-ready JSONL.

Produces three output files:
  Doc/ruleset.yaml            — full hierarchy  (human reference)
  Doc/ruleset_flat.yaml       — flat list with IDs (human reference)
  HouseQuest/data/ruleset.json — JSONL for LittleFS on the ESP32

Rule ID format:  <section-number>.<bullet-index>
  e.g.  2.1.3  = third bullet under section 2.1
        3.1.1.2 = second bullet under topic 3.1.1

Each rule carries:
  xp:    XP awarded for compliance     (default 0)
  hp:    HP deducted for violation     (default 10)
  level: game level at which rule applies (default 1)

ESP32 JSONL: one JSON object per line — easy to scan line-by-line on
embedded hardware without loading the whole file into RAM.

Usage:
    python ruleset_to_yaml.py [input.md [output_dir [esp32_data_dir]]]

Defaults (run from project root):
    input.md       → Doc/ruleset.md
    output_dir     → Doc/
    esp32_data_dir → HouseQuest/data/
"""

import json
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("pyyaml required — activate venv: source python/venv/bin/activate", file=sys.stderr)
    sys.exit(1)

ESP32_TEXT_MAX = 80   # chars stored in JSONL; fits two lines on a 320px TFT at size 1

# ── defaults ──────────────────────────────────────────────────────────────────

DEFAULT_XP    = 0
DEFAULT_HP    = 10
DEFAULT_LEVEL = 1

# ── regex ─────────────────────────────────────────────────────────────────────

HEADING_RE    = re.compile(r'^(#{1,5})\s+(.+)$')
BULLET_RE     = re.compile(r'^[-*]\s+(.+)$')
BOLD_RE       = re.compile(r'\*{1,2}(.+?)\*{1,2}')
ANNOT_RE      = re.compile(r'_\[xp:?(\d+),hp:?(\d+),l:?(\d+)\]_')
NUM_PREFIX_RE = re.compile(r'^(\d+(?:\.\d+)*)[.\s]')  # "3.1.1 Foo" → "3.1.1"


# ── helpers ───────────────────────────────────────────────────────────────────

def _strip_bold(text: str) -> str:
    return BOLD_RE.sub(r'\1', text).strip()


def _section_num(title: str) -> str | None:
    """Extract numeric prefix from a heading title, e.g. '2.1 Foo' → '2.1'."""
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


# ── markdown parser → hierarchy ───────────────────────────────────────────────

def parse_markdown(content: str) -> list:
    root  = []
    stack = []  # [(depth, node_dict), ...]
    sec_xp, sec_hp, sec_level = DEFAULT_XP, DEFAULT_HP, DEFAULT_LEVEL

    for raw in content.splitlines():
        line = raw.strip()

        if not line or line.startswith('---') or line.startswith('>') or line.startswith('|'):
            continue

        h_match = HEADING_RE.match(line)
        b_match = BULLET_RE.match(line)

        # standalone annotation → update section default
        annot = ANNOT_RE.fullmatch(line)
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


# ── flat output ───────────────────────────────────────────────────────────────

def _flatten(nodes: list, breadcrumb: list[str] | None = None) -> list:
    """Walk the hierarchy and emit one dict per rule with a stable ID."""
    if breadcrumb is None:
        breadcrumb = []

    flat = []

    for node in nodes:
        title    = node["title"]
        crumb    = breadcrumb + [title]
        sec_num  = _section_num(title)
        path_str = " > ".join(crumb)

        for i, rule in enumerate(node.get("rules", []), start=1):
            rule_id = f"{sec_num}.{i}" if sec_num else f"?.{i}"
            flat.append({
                "id":    rule_id,
                "path":  path_str,
                "text":  rule["text"],
                "xp":    rule["xp"],
                "hp":    rule["hp"],
                "level": rule["level"],
            })

        flat.extend(_flatten(node.get("children", []), crumb))

    return flat


# ── stats ─────────────────────────────────────────────────────────────────────

def _iter_rules(nodes: list):
    for node in nodes:
        yield from node.get("rules", [])
        yield from _iter_rules(node.get("children", []))


def _count_sections(nodes: list) -> int:
    return sum(1 + _count_sections(n.get("children", [])) for n in nodes)


# ── write ─────────────────────────────────────────────────────────────────────

def _write_yaml(data: dict | list, path: Path) -> None:
    with path.open("w", encoding="utf-8") as fh:
        yaml.dump(data, fh, allow_unicode=True, default_flow_style=False,
                  sort_keys=False, indent=2)


def _write_esp32_jsonl(flat: list, path: Path) -> None:
    """Write one JSON object per line — easy line-by-line scan on the ESP32."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for rule in flat:
            text = rule["text"]
            if len(text) > ESP32_TEXT_MAX:
                text = text[:ESP32_TEXT_MAX - 1] + "…"
            obj = {
                "id": rule["id"],
                "hp": rule["hp"],
                "xp": rule["xp"],
                "l":  rule["level"],
                "t":  text,
            }
            fh.write(json.dumps(obj, ensure_ascii=False) + "\n")


# ── main ──────────────────────────────────────────────────────────────────────

def convert(md_path: Path, out_dir: Path, esp32_dir: Path) -> None:
    content  = md_path.read_text(encoding="utf-8")
    tree     = parse_markdown(content)
    flat     = _flatten(tree)

    hier_path  = out_dir  / "ruleset.yaml"
    flat_path  = out_dir  / "ruleset_flat.yaml"
    json_path  = esp32_dir / "ruleset.json"

    _write_yaml({"ruleset": tree}, hier_path)
    _write_yaml({"rules": flat},   flat_path)
    _write_esp32_jsonl(flat,       json_path)

    rules    = sum(1 for _ in _iter_rules(tree))
    sections = _count_sections(tree)
    print(f"{md_path}")
    print(f"  → {hier_path}   ({sections} sections, {rules} rules)")
    print(f"  → {flat_path}   ({len(flat)} rules, flat)")
    print(f"  → {json_path}  ({len(flat)} rules, JSONL for ESP32)")


if __name__ == "__main__":
    md_path   = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("Doc/ruleset.md")
    out_dir   = Path(sys.argv[2]) if len(sys.argv) > 2 else md_path.parent
    esp32_dir = Path(sys.argv[3]) if len(sys.argv) > 3 else Path("HouseQuest/data")
    convert(md_path, out_dir, esp32_dir)
