# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""The English installation guide follows the French one: the same commands, in the same order, the version included.

docs/INSTALLATION.md is the reference; docs/INSTALLATION.en.md translates its text. Only the explanations differ, so
a command changed on one side and forgotten on the other (a version number, a path) fails here. The menu entries the
English guide names in italics must be iDrive's English texts (hmiapp/make_db.py), or the menu's English rows.
"""

import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLOCK = re.compile(r"^```sh\n(.*?)^```", re.M | re.S)
CODE = re.compile(r"^```.*?^```|`[^`\n]*`", re.M | re.S)  # code blocks and spans: their * are not italics
ITALIC = re.compile(r"(?<![*\w])\*([^*\n]+)\*(?![*\w])")
# Names in italics that are not menu rows: iDrive's own menus and buttons.
IDRIVE_NAMES = {"Media", "Android Auto"}


def blocks(path):
    return BLOCK.findall(path.read_text(encoding="utf-8"))


def english_texts():
    source = (ROOT / "hmiapp/make_db.py").read_text(encoding="utf-8")
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "TEXTS" for t in node.targets):
            return set(ast.literal_eval(node.value).values())
    raise SystemExit("FAIL docs: TEXTS not found in hmiapp/make_db.py")


def main():
    failures = []
    french, english = blocks(ROOT / "docs/INSTALLATION.md"), blocks(ROOT / "docs/INSTALLATION.en.md")
    if not french:
        failures.append("no sh block in docs/INSTALLATION.md")
    if len(french) != len(english):
        failures.append(f"{len(french)} sh blocks in French, {len(english)} in English")
    for number, (fr, en) in enumerate(zip(french, english), 1):
        if fr != en:
            failures.append(f"sh block {number} differs:\n--- French\n{fr}--- English\n{en}")
    texts = english_texts()
    text = CODE.sub("", (ROOT / "docs/INSTALLATION.en.md").read_text(encoding="utf-8"))
    for name in sorted(set(ITALIC.findall(text))):
        if name not in texts and name not in IDRIVE_NAMES:
            failures.append(f"menu entry *{name}* is not one of iDrive's English texts (hmiapp/make_db.py)")
    if failures:
        for failure in failures:
            print(f"FAIL docs: {failure}")
        return 1
    print(f"PASS docs: the English installation guide has the French one's {len(french)} command blocks, menu entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
