"""Fetch a CI run's logs and report what went wrong.

Writes ci_logs/<run-id>/ with one log per job, annotations.json and
diagnostics.txt, then prints failed jobs and steps, compiler and analyzer
diagnostics, failed ctest suites, and every annotation above notice level.
"""

import json
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path

from varintdev import diagnostics, github
from varintdev.repo import REPO_ROOT

LOG_ROOT = REPO_ROOT / "ci_logs"

_TEST_FAILURES = (
    # ctest's own result line.
    re.compile(
        r"Test +#\d+: (?P<name>\S+) \.+\s*\*+"
        r"(?P<reason>Failed|Exception|Timeout|SEGFAULT|Not Run)"
    ),
    # varintdev ctest's launcher modes (valgrind, leaks): "  name  FAILED (99)".
    re.compile(r"^\s+(?P<name>[\w.-]+)\s+(?P<reason>FAILED \(-?\d+\))$"),
)


def _test_failure(line: str) -> re.Match[str] | None:
    for pattern in _TEST_FAILURES:
        if (match := pattern.search(line)) is not None:
            return match
    return None


@dataclass(frozen=True, slots=True)
class TestFailure:
    job: str
    test: str
    reason: str


@dataclass(slots=True)
class Report:
    run: github.Run
    jobs: list[github.Job]
    diagnostics: list[diagnostics.Diagnostic]
    test_failures: list[TestFailure]
    annotations: list[github.Annotation]

    def render(self) -> str:
        out = [
            f"Run {self.run.id}: {self.run.title}",
            f"  {self.run.url}",
            f"  commit {self.run.head_sha[:12]}  "
            f"{self.run.status} {self.run.conclusion or ''}",
            "",
            "Jobs:",
        ]
        out += [f"  {job.conclusion or job.status:<10} {job.name}" for job in self.jobs]
        failed = [(j, s) for j in self.jobs for s in j.failed_steps]
        out += ["", "Failed steps:"]
        out += [f"  {j.name}: {s.name}" for j, s in failed] or ["  none"]
        out += ["", "Failed tests:"]
        out += [f"  {f.job}: {f.test} ({f.reason})" for f in self.test_failures] or [
            "  none"
        ]
        out += ["", "Diagnostics:"]
        if self.diagnostics:
            out.append(diagnostics.summarize(self.diagnostics))
        else:
            out.append("  none")
        notices = [a for a in self.annotations if a.level == "notice"]
        out += ["", "Annotations above notice level:"]
        out += [
            f"  [{a.level}] {a.job}: {a.message}"
            for a in self.annotations
            if a.level != "notice"
        ] or ["  none"]
        for message in sorted({a.message for a in notices}):
            count = sum(a.message == message for a in notices)
            out.append(f"  notice x{count}: {message}")
        return "\n".join(out)


def _slug(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-").lower()


def fetch(run_id: int | None, workflow: str = "ci.yml") -> Report:
    if run_id is None:
        branch = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
        run = github.latest_run(branch, workflow)
    else:
        run = github.get_run(run_id)

    out_dir = LOG_ROOT / str(run.id)
    out_dir.mkdir(parents=True, exist_ok=True)

    jobs = github.jobs(run.id)
    found: set[diagnostics.Diagnostic] = set()
    test_failures: list[TestFailure] = []
    annotations: list[github.Annotation] = []
    for job in jobs:
        annotations += github.annotations(job)
        if job.status != "completed":
            continue
        log = github.job_log(job)
        log_path: Path = out_dir / f"{job.id}-{_slug(job.name)}.log"
        log_path.write_text(log)
        lines = log.splitlines()
        found.update(diagnostics.parse(lines))
        test_failures += [
            TestFailure(job.name, m["name"], m["reason"])
            for m in map(_test_failure, lines)
            if m is not None
        ]

    report = Report(run, jobs, sorted(found), test_failures, annotations)
    (out_dir / "annotations.json").write_text(
        json.dumps([asdict(a) for a in annotations], indent=2)
    )
    (out_dir / "diagnostics.txt").write_text(
        "".join(f"{d}\n" for d in report.diagnostics)
    )
    print(f"Logs written to {out_dir.relative_to(REPO_ROOT)}/")
    return report
