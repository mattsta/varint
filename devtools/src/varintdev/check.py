"""Run every gate CI runs, locally, and report each one's outcome.

This is the pre-push check: the same workflow, lint, warning, static
analysis and test-mode commands the CI jobs run, in one place. Gates run
in order from fastest to slowest and all of them run even after a
failure, so one pass reports everything that would fail in CI. Test modes
whose tool this machine lacks (valgrind on macOS, leaks on Linux) are
reported as skipped, not passed.
"""

import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from enum import StrEnum

from varintdev import cppcheck, ctest, warnings, workflows
from varintdev.repo import REPO_ROOT


class Outcome(StrEnum):
    PASSED = "passed"
    FAILED = "failed"
    SKIPPED = "skipped"


@dataclass(frozen=True, slots=True)
class Gate:
    name: str
    run: Callable[[], int]
    requires: str | None = None


@dataclass(frozen=True, slots=True)
class Result:
    gate: Gate
    outcome: Outcome
    seconds: float


def _python_tool(*args: str) -> Callable[[], int]:
    def run() -> int:
        argv = [sys.executable, "-m", *args]
        print(f"$ {' '.join(argv)}", flush=True)
        return subprocess.run(argv, cwd=REPO_ROOT / "devtools", check=False).returncode

    return run


def _ctest(mode: ctest.Mode) -> Callable[[], int]:
    return lambda: ctest.run_tests(mode)


GATES: tuple[Gate, ...] = (
    Gate("workflows", workflows.check, requires="gh"),
    Gate("ruff format", _python_tool("ruff", "format", "--check", "src")),
    Gate("ruff check", _python_tool("ruff", "check", "src")),
    Gate("mypy", _python_tool("mypy", "src")),
    Gate("warnings", lambda: warnings.check(["gcc", "clang"])),
    Gate("cppcheck", lambda: cppcheck.check(cppcheck.Config()), requires="cppcheck"),
    Gate("ctest both", _ctest(ctest.Mode.BOTH)),
    Gate("ctest uninit", _ctest(ctest.Mode.UNINIT)),
    Gate("ctest tsan", _ctest(ctest.Mode.TSAN)),
    Gate("ctest leaks", _ctest(ctest.Mode.LEAKS), requires="leaks"),
    Gate("ctest valgrind", _ctest(ctest.Mode.VALGRIND), requires="valgrind"),
)


def run_all(selected: set[str] | None = None) -> int:
    results: list[Result] = []
    for gate in GATES:
        if selected is not None and gate.name not in selected:
            continue
        print(f"\n===== {gate.name} =====", flush=True)
        if gate.requires and shutil.which(gate.requires) is None:
            print(f"skipped: {gate.requires} is not installed")
            results.append(Result(gate, Outcome.SKIPPED, 0.0))
            continue
        start = time.monotonic()
        try:
            status = gate.run()
        except subprocess.CalledProcessError as exc:
            print(f"command failed with status {exc.returncode}")
            status = exc.returncode or 1
        outcome = Outcome.PASSED if status == 0 else Outcome.FAILED
        results.append(Result(gate, outcome, time.monotonic() - start))

    print("\n===== summary =====")
    for result in results:
        print(f"  {result.outcome:<8} {result.seconds:7.1f}s  {result.gate.name}")
    failed = [r for r in results if r.outcome is Outcome.FAILED]
    print(
        f"{len(failed)} of {len(results)} gates failed"
        if failed
        else "all gates passed"
    )
    return 1 if failed else 0
