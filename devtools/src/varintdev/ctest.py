"""Build the CMake tree in an instrumented mode and run every ctest suite.

src/CMakeLists.txt is the single source of truth for the test list, the
per-test defines and the per-test sources, so a suite added there is
covered here automatically.

Modes:
  none    plain Debug build
  asan    AddressSanitizer
  ubsan   UndefinedBehaviorSanitizer
  both    ASan + UBSan
  tsan    ThreadSanitizer
  uninit  Release build with every uninitialized stack variable filled
          with 0xAA and every heap allocation filled with junk, so a read
          of memory that was never written yields garbage instead of
          whatever the stack or allocator happened to hold
  valgrind  plain Debug build with every test command run under
          valgrind memcheck; any error fails the test (Linux)
  leaks   plain Debug build with every test command run under the macOS
          leaks tool, which fails the test on any leaked allocation at
          exit; macOS's AddressSanitizer has no leak detection, so this
          is the macOS counterpart of Linux ASan's LeakSanitizer
"""

import json
import platform
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from enum import StrEnum
from functools import partial
from pathlib import Path

from varintdev.repo import REPO_ROOT, cpu_count, run

BUILD_ROOT = REPO_ROOT / "build_tests"

_SANITIZER_FLAGS = "-fno-omit-frame-pointer -O1 -g"


class Mode(StrEnum):
    NONE = "none"
    ASAN = "asan"
    UBSAN = "ubsan"
    BOTH = "both"
    TSAN = "tsan"
    UNINIT = "uninit"
    VALGRIND = "valgrind"
    LEAKS = "leaks"


VALGRIND = (
    "valgrind",
    "--quiet",
    "--error-exitcode=99",
    "--leak-check=full",
    "--errors-for-leak-kinds=definite,indirect",
    "--track-origins=yes",
)

LEAKS = ("leaks", "--quiet", "--atExit", "--")


@dataclass(frozen=True, slots=True)
class Instrumentation:
    c_flags: str
    # Debug keeps the tree at -O0 so the sanitizer's -O1 governs; uninit
    # runs at the optimization level users ship with.
    build_type: str
    env: dict[str, str] = field(default_factory=dict)
    # Prefix for every test command, for tools that wrap the process.
    launcher: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class TestCommand:
    name: str
    argv: tuple[str, ...]
    cwd: Path


@dataclass(frozen=True, slots=True)
class TestResult:
    test: TestCommand
    returncode: int
    output: str


def _junk_heap_env() -> dict[str, str]:
    if platform.system() == "Darwin":
        return {"MallocPreScribble": "1", "MallocScribble": "1"}
    # glibc fills new allocations with the complement of this byte and
    # freed ones with the byte itself.
    return {"MALLOC_PERTURB_": "170"}


_NO_RECOVER = "-fno-sanitize-recover=undefined"

INSTRUMENTATION: dict[Mode, Instrumentation] = {
    Mode.NONE: Instrumentation("", "Debug"),
    Mode.ASAN: Instrumentation(f"-fsanitize=address {_SANITIZER_FLAGS}", "Debug"),
    Mode.UBSAN: Instrumentation(
        f"-fsanitize=undefined {_NO_RECOVER} {_SANITIZER_FLAGS}", "Debug"
    ),
    Mode.BOTH: Instrumentation(
        f"-fsanitize=address,undefined {_NO_RECOVER} {_SANITIZER_FLAGS}", "Debug"
    ),
    Mode.TSAN: Instrumentation(f"-fsanitize=thread {_SANITIZER_FLAGS}", "Debug"),
    Mode.UNINIT: Instrumentation(
        "-ftrivial-auto-var-init=pattern", "Release", _junk_heap_env()
    ),
    Mode.VALGRIND: Instrumentation("-g", "Debug", launcher=VALGRIND),
    Mode.LEAKS: Instrumentation("-g", "Debug", launcher=LEAKS),
}


def registered_tests(build_dir: Path) -> list[TestCommand]:
    """Every test ctest knows about, with its command and working directory."""
    listing = json.loads(
        run(
            ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
            capture=True,
        ).stdout
    )
    tests = []
    for test in listing["tests"]:
        properties = {p["name"]: p["value"] for p in test.get("properties", [])}
        tests.append(
            TestCommand(
                name=test["name"],
                argv=tuple(test["command"]),
                cwd=Path(properties.get("WORKING_DIRECTORY", build_dir)),
            )
        )
    return tests


def _run_launched(launcher: tuple[str, ...], test: TestCommand) -> TestResult:
    result = subprocess.run(
        [*launcher, *test.argv],
        cwd=test.cwd,
        text=True,
        capture_output=True,
        check=False,
    )
    return TestResult(test, result.returncode, result.stdout + result.stderr)


def _run_with_launcher(inst: Instrumentation, build_dir: Path) -> bool:
    tests = registered_tests(build_dir)
    print(f"Running {len(tests)} tests under {' '.join(inst.launcher)}")
    with ThreadPoolExecutor(max_workers=cpu_count()) as pool:
        results = list(pool.map(partial(_run_launched, inst.launcher), tests))
    for result in results:
        status = "ok" if result.returncode == 0 else f"FAILED ({result.returncode})"
        print(f"  {result.test.name:<28} {status}")
    failed = [r for r in results if r.returncode != 0]
    for result in failed:
        print(f"-- {result.test.name}")
        print(result.output.rstrip())
    return not failed


def _run_ctest(inst: Instrumentation, build_dir: Path) -> bool:
    result = run(
        [
            "ctest",
            "--test-dir",
            build_dir,
            "--output-on-failure",
            "--parallel",
            str(cpu_count()),
        ],
        env=inst.env,
        check=False,
    )
    return result.returncode == 0


def list_tests(mode: Mode) -> int:
    """Print the registered tests of an already-configured mode's tree."""
    build_dir = BUILD_ROOT / mode.value
    for test in registered_tests(build_dir):
        print(f"{test.name:<28} {' '.join(test.argv)}  (in {test.cwd})")
    return 0


def run_tests(mode: Mode) -> int:
    inst = INSTRUMENTATION[mode]
    if inst.launcher and shutil.which(inst.launcher[0]) is None:
        print(f"Mode {mode.value} needs {inst.launcher[0]}, which is not installed.")
        return 2
    build_dir = BUILD_ROOT / mode.value
    print(f"Mode {mode.value}: {inst.build_type} {inst.c_flags or '(no extra flags)'}")
    print(f"Build dir: {build_dir.relative_to(REPO_ROOT)}")

    run(
        [
            "cmake",
            "-S",
            REPO_ROOT,
            "-B",
            build_dir,
            f"-DCMAKE_BUILD_TYPE={inst.build_type}",
            f"-DCMAKE_C_FLAGS={inst.c_flags}",
        ]
    )
    run(["cmake", "--build", build_dir, "--parallel", str(cpu_count())])
    passed = (
        _run_with_launcher(inst, build_dir)
        if inst.launcher
        else _run_ctest(inst, build_dir)
    )
    if not passed:
        print(f"Some tests failed in mode {mode.value}.")
        return 1
    print(f"All tests passed in mode {mode.value}.")
    return 0
