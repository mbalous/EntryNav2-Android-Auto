# Copyright (c) 2026 Jiraiya43
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Verify the standard and live-data text archives can be generated independently."""

import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def texts(archive: zipfile.ZipFile, language: str) -> dict[str, str]:
    return dict(line.split("=", 1) for line in archive.read(f"{language}.txt").decode("ascii").splitlines())


def main() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        args = [sys.executable, "-I", str(ROOT / "hmiapp/make_db.py"), str(ROOT / "hmiapp/res"), tmp]
        subprocess.run(args, check=True)
        subprocess.run([*args, "--live-data"], check=True)
        version = (ROOT / "VERSION").read_text().strip()
        with (
            zipfile.ZipFile(Path(tmp) / "EntryAad_ALL_Texts.zip") as app,
            zipfile.ZipFile(Path(tmp) / "EntryLiveData_ALL_Texts.zip") as live,
        ):
            assert app.namelist() == live.namelist()
            for name in app.namelist():
                language = name.removesuffix(".txt")
                app_text = texts(app, language)
                live_text = texts(live, language)
                assert app_text["27"] == f'{app_text["16"]} - {version}'
                assert set(live_text) == {"100000", "1", "2", "3", "4"}
            assert texts(live, "fr-FR")["1"] == "Donnees en direct"
            assert texts(live, "pl")["1"] == "Live Data"
    print("PASS text databases: versioned Help and live data")


if __name__ == "__main__":
    main()
