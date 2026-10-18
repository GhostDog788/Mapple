#!/usr/bin/env python3
"""Fail before compilation when headers expose a known incompatible kernel API."""
from pathlib import Path
import re
import sys


def check(headers):
    header = headers / "include/linux/fs.h"
    if header.exists() and re.search(r"get_file_rcu\s*\(\s*struct file(?:\s+__rcu)?\s*\*\s*\*", header.read_text()):
        raise RuntimeError("These kernel headers changed get_file_rcu(struct file *) to struct file **. "
                           "Application C is unchanged: use make vm-setup to install the compatible debug kernel, "
                           "or select the existing 6.6.6 VM slot.")


if __name__ == "__main__":
    try:
        check(Path(sys.argv[1]))
    except RuntimeError as error:
        raise SystemExit(str(error))
