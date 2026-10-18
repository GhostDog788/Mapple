#!/usr/bin/env python3
"""Select a usable Go toolchain even with Ubuntu's older auto-selector."""

import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def required_version():
    match = re.search(r"(?m)^go (\d+)\.(\d+)(?:\.(\d+))?\s*$", (ROOT / "user/go.mod").read_text())
    if not match:
        raise RuntimeError("Cannot read the Go version required by user/go.mod")
    return tuple(int(part or 0) for part in match.groups())


def select():
    required = required_version()
    try:
        result = subprocess.run(["go", "version"], env=dict(os.environ, GOTOOLCHAIN="local"),
                                capture_output=True, text=True, timeout=10, check=True)
        match = re.search(r"\bgo(\d+)\.(\d+)(?:\.(\d+))?(?!\d)", result.stdout)
        if match and tuple(int(part or 0) for part in match.groups()) >= required:
            return "auto"
    except (OSError, subprocess.SubprocessError):
        pass
    # Older Ubuntu Go releases try downloading "go1.26" for a go 1.26
    # directive. Released Go toolchain names include the patch component.
    return "go" + ".".join(map(str, required)) + "+auto"


def environment():
    env = dict(os.environ)
    if not env.get("GOTOOLCHAIN"):
        env["GOTOOLCHAIN"] = select()
    return env


if __name__ == "__main__":
    print(select())
