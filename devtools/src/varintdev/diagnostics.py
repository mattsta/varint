"""Compiler and static-analyzer diagnostics in the GCC "file:line:col:" form.

GCC, Clang and cppcheck (with the template used by varintdev.cppcheck) all
print findings as

    path:line:column: severity: message [check-id]

which this module parses into Diagnostic records and summarizes.
"""

import re
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from enum import StrEnum

from varintdev.repo import REPO_ROOT


class Severity(StrEnum):
    FATAL = "fatal error"
    ERROR = "error"
    WARNING = "warning"
    STYLE = "style"
    PERFORMANCE = "performance"
    PORTABILITY = "portability"
    INFORMATION = "information"


_SEVERITIES = "|".join(re.escape(s.value) for s in Severity)
_DIAGNOSTIC = re.compile(
    rf"^(?P<path>[^\s:][^:]*):(?P<line>\d+):(?P<column>\d+): "
    rf"(?P<severity>{_SEVERITIES}): (?P<message>.*?)"
    rf"(?: \[(?P<check>[\w,=+-]+)\])?$"
)
# Absolute checkout prefixes: GitHub-hosted runners, then this checkout.
_WORKSPACE = re.compile(
    rf"^(?:/(?:home|Users)/runner/work/[^/]+/[^/]+|{re.escape(str(REPO_ROOT))})/"
)


def _check_name(tag: str | None) -> str | None:
    """The warning that fired: "-Werror,-Wsign-conversion" -> "-Wsign-conversion"."""
    if tag is None:
        return None
    return tag.rsplit(",", 1)[-1]


@dataclass(frozen=True, order=True, slots=True)
class Diagnostic:
    path: str
    line: int
    column: int
    severity: Severity
    message: str
    check: str | None

    def __str__(self) -> str:
        suffix = f" [{self.check}]" if self.check else ""
        return (
            f"{self.path}:{self.line}:{self.column}: "
            f"{self.severity.value}: {self.message}{suffix}"
        )


def parse_line(line: str) -> Diagnostic | None:
    match = _DIAGNOSTIC.match(line.strip())
    if match is None:
        return None
    return Diagnostic(
        path=_WORKSPACE.sub("", match["path"]),
        line=int(match["line"]),
        column=int(match["column"]),
        severity=Severity(match["severity"]),
        message=match["message"],
        check=_check_name(match["check"]),
    )


def parse(lines: Iterable[str]) -> list[Diagnostic]:
    """Unique diagnostics found in lines, sorted by location."""
    found = {d for d in map(parse_line, lines) if d is not None}
    return sorted(found)


def summarize(diagnostics: list[Diagnostic]) -> str:
    """Counts by check and by file, most frequent first."""
    by_check = Counter(d.check or d.severity.value for d in diagnostics)
    by_file = Counter(d.path for d in diagnostics)
    out = [f"{len(diagnostics)} diagnostics"]
    out.append("  by check:")
    out += [f"    {n:4d}  {name}" for name, n in by_check.most_common()]
    out.append("  by file:")
    out += [f"    {n:4d}  {name}" for name, n in by_file.most_common()]
    return "\n".join(out)
