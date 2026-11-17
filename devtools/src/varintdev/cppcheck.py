"""The project's cppcheck configuration.

CI, the CMake varint-check target, and local runs all go through this
module, so there is one definition of which checks run and which are
suppressed. Any finding fails the run.
"""

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from varintdev import diagnostics
from varintdev.repo import REPO_ROOT, SRC_DIR, cpu_count, run

ENABLED_CHECKS = ("warning", "style", "performance", "portability")

# Checks that do not apply to this code base:
#   missingIncludeSystem  system headers are not on cppcheck's include path
#   unusedFunction        public API functions have no in-tree caller
#   const*                const-qualification advice, not defects
#   unmatchedSuppression  inline suppressions for checks another
#                         cppcheck version does not emit
SUPPRESSIONS = (
    "missingIncludeSystem",
    "unusedFunction",
    "unmatchedSuppression",
    "constVariablePointer",
    "constVariable",
    "constParameter",
    "constParameterPointer",
)

TEMPLATE = "{file}:{line}:{column}: {severity}: {message} [{id}]"


@dataclass(frozen=True, slots=True)
class Config:
    paths: Sequence[Path] = (SRC_DIR,)
    exhaustive: bool = False
    jobs: int = cpu_count()

    def sources(self) -> list[Path]:
        files: list[Path] = []
        for path in self.paths:
            files += sorted(path.rglob("*.c")) if path.is_dir() else [path]
        return files

    def argv(self) -> list[str]:
        argv = [
            "cppcheck",
            f"--enable={','.join(ENABLED_CHECKS)}",
            "--std=c11",
            "--inline-suppr",
            "--quiet",
            f"--template={TEMPLATE}",
            f"-j{self.jobs}",
            f"-I{SRC_DIR.relative_to(REPO_ROOT)}",
        ]
        argv += [f"--suppress={s}" for s in SUPPRESSIONS]
        if self.exhaustive:
            argv.append("--check-level=exhaustive")
        argv += [str(p.relative_to(REPO_ROOT)) for p in self.sources()]
        return argv


def check(config: Config) -> int:
    """Run cppcheck; print every finding and a summary. Returns exit status."""
    result = run(config.argv(), capture=True, check=False)
    output = result.stdout + result.stderr
    found = diagnostics.parse(output.splitlines())
    for diagnostic in found:
        print(diagnostic)
    if result.returncode != 0 and not found:
        print(output)
        print(f"cppcheck exited with status {result.returncode}")
        return result.returncode
    if found:
        print()
        print(diagnostics.summarize(found))
        return 1
    print(f"cppcheck: no findings in {len(config.sources())} files")
    return 0
