"""Locations inside the varint repository and subprocess helpers."""

import os
import shlex
import subprocess
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SRC_DIR = REPO_ROOT / "src"
EXAMPLES_DIR = REPO_ROOT / "examples"


def run(
    argv: Sequence[str | Path],
    *,
    cwd: Path = REPO_ROOT,
    env: Mapping[str, str] | None = None,
    capture: bool = False,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    """Run a command, echoing it first; env entries extend os.environ."""
    args = [str(a) for a in argv]
    print(f"$ {shlex.join(args)}", file=sys.stderr, flush=True)
    merged = {**os.environ, **env} if env else None
    return subprocess.run(
        args,
        cwd=cwd,
        env=merged,
        text=True,
        capture_output=capture,
        check=check,
    )


def cpu_count() -> int:
    return os.cpu_count() or 1
