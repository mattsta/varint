"""Strict-warning compile of every library source with each compiler.

Every src/*.c is compiled on its own (-c) with the compiler family's
strict flag set and -Werror; any diagnostic fails the check. The flag set
follows what the compiler is, not what it is called: on macOS "gcc" is
Apple Clang and gets the Clang flags.
"""

import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from enum import StrEnum
from functools import partial
from pathlib import Path

from varintdev import diagnostics
from varintdev.repo import REPO_ROOT, SRC_DIR, cpu_count


class Family(StrEnum):
    GCC = "gcc"
    CLANG = "clang"


FLAGS: dict[Family, tuple[str, ...]] = {
    Family.GCC: ("-O2", "-Wall", "-Wextra", "-Werror"),
    Family.CLANG: (
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Wsign-conversion",
        "-Wconversion",
        "-Werror",
    ),
}


@dataclass(frozen=True, slots=True)
class Compiler:
    command: str
    family: Family
    version: str

    @classmethod
    def detect(cls, command: str) -> "Compiler":
        if shutil.which(command) is None:
            raise FileNotFoundError(f"compiler not found: {command}")
        banner = subprocess.run(
            [command, "--version"], text=True, capture_output=True, check=True
        ).stdout
        family = Family.CLANG if "clang" in banner.lower() else Family.GCC
        return cls(command, family, banner.splitlines()[0])


@dataclass(frozen=True, slots=True)
class FileResult:
    source: Path
    ok: bool
    output: str


def _compile(compiler: Compiler, source: Path) -> FileResult:
    result = subprocess.run(
        [
            compiler.command,
            *FLAGS[compiler.family],
            "-fdiagnostics-color=never",
            f"-I{SRC_DIR}",
            "-c",
            str(source),
            "-o",
            "/dev/null",
        ],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    output = result.stdout + result.stderr
    return FileResult(source, result.returncode == 0 and not output.strip(), output)


def check(compiler_commands: list[str]) -> int:
    sources = sorted(SRC_DIR.glob("*.c"))
    failures = 0
    for command in compiler_commands:
        compiler = Compiler.detect(command)
        print(f"== {command}: {compiler.version} ({compiler.family.value} flags)")
        print(f"   {' '.join(FLAGS[compiler.family])}")
        with ThreadPoolExecutor(max_workers=cpu_count()) as pool:
            results = list(pool.map(partial(_compile, compiler), sources))
        failed = [r for r in results if not r.ok]
        for result in failed:
            print(f"-- {result.source.relative_to(REPO_ROOT)}")
            print(result.output.rstrip())
        found = diagnostics.parse(
            line for r in failed for line in r.output.splitlines()
        )
        print(f"== {command}: {len(sources) - len(failed)}/{len(sources)} files clean")
        if found:
            print(diagnostics.summarize(found))
        failures += len(failed)
    return 1 if failures else 0
