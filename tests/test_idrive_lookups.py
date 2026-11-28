# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""iDrive's per-thread lookups stay behind the posted-task guard (hmiapp/posted_task.hpp).

hmiplugin::get<...>() and HmiMain3D's instance cache only work outside the app's posted tasks. The app asks them in one
place, hmiapp/idrive_coding.cpp, whose two entry points refuse to run inside a posted task; and the app's runnables run
marked. A lookup added anywhere else, or the guard or the marking dropped, fails here before it reaches a car.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOOKUP = re.compile(r"_ZN9hmiplugin|_ZNK9hmiplugin")  # hmiplugin::get, its cache and usage objects
HOME = "hmiapp/idrive_coding.cpp"


def entry_body(text, signature):
    """The body of a top-level function, from its signature to the closing brace at the start of a line."""
    start = text.index(signature)
    return text[start : text.index("\n}\n", start)]


def main():
    problems = []
    for path in sorted((ROOT / "hmiapp").glob("*.[ch]pp")):
        name = path.relative_to(ROOT).as_posix()
        if LOOKUP.search(path.read_text(encoding="utf-8")) and name != HOME:
            problems.append(f"{name} names an iDrive per-thread lookup: keep them in {HOME}, behind its guard")
    home = (ROOT / HOME).read_text(encoding="utf-8")
    if '#include "posted_task.hpp"' not in home:
        problems.append(f"{HOME} does not include posted_task.hpp")
    for signature in ("bool writeIdriveCoding() {", "DriveModePopup readDriveModePopup() {"):
        body = entry_body(home, signature) if signature in home else ""
        guard = body.find("lookupRefused(")
        first_lookup = min([i for i in (body.find("VariantServer"), body.find("readPopupSetting(")) if i >= 0] or [-1])
        if guard < 0 or (first_lookup >= 0 and first_lookup < guard):
            problems.append(f"{HOME}: {signature[:-2]} does not refuse a posted task before its lookups")
    framework = (ROOT / "hmiapp/bmw_framework.cpp").read_text(encoding="utf-8")
    if "vtbl[4] = reinterpret_cast<void*>(&runPostedTask<AppRunnable>);" not in framework:
        problems.append("hmiapp/bmw_framework.cpp: the app's runnables do not run through runPostedTask")
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print("PASS iDrive lookups: only in idrive_coding.cpp, refused inside posted tasks, runnables marked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
