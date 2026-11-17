"""Typed access to GitHub Actions runs through the authenticated gh CLI."""

import json
import re
import subprocess
from dataclasses import dataclass
from typing import Any

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Every Actions log line starts with an ISO-8601 timestamp and a space.
_TIMESTAMP = re.compile(r"^\d{4}-\d\d-\d\dT[\d:.]+Z ", re.MULTILINE)


def _gh(*args: str) -> str:
    result = subprocess.run(["gh", *args], text=True, capture_output=True, check=True)
    return result.stdout


def _api(path: str) -> Any:
    return json.loads(_gh("api", path))


def _api_pages(path: str) -> list[Any]:
    return list(json.loads(_gh("api", "--paginate", "--slurp", path)))


@dataclass(frozen=True, slots=True)
class Step:
    name: str
    conclusion: str | None


@dataclass(frozen=True, slots=True)
class Job:
    id: int
    name: str
    status: str
    conclusion: str | None
    check_run_id: int
    steps: tuple[Step, ...]

    @property
    def failed(self) -> bool:
        return self.conclusion == "failure"

    @property
    def failed_steps(self) -> list[Step]:
        return [s for s in self.steps if s.conclusion == "failure"]


@dataclass(frozen=True, slots=True)
class Annotation:
    job: str
    level: str
    path: str
    line: int
    message: str


@dataclass(frozen=True, slots=True)
class Run:
    id: int
    title: str
    head_sha: str
    status: str
    conclusion: str | None
    url: str


def latest_run(branch: str) -> Run:
    runs = json.loads(
        _gh(
            "run",
            "list",
            "--branch",
            branch,
            "--limit",
            "1",
            "--json",
            "databaseId",
        )
    )
    if not runs:
        raise LookupError(f"no workflow runs for branch {branch}")
    return get_run(int(runs[0]["databaseId"]))


def get_run(run_id: int) -> Run:
    data = _api(f"repos/{{owner}}/{{repo}}/actions/runs/{run_id}")
    return Run(
        id=run_id,
        title=data["display_title"],
        head_sha=data["head_sha"],
        status=data["status"],
        conclusion=data["conclusion"],
        url=data["html_url"],
    )


def jobs(run_id: int) -> list[Job]:
    pages = _api_pages(
        f"repos/{{owner}}/{{repo}}/actions/runs/{run_id}/jobs?per_page=100"
    )
    return [
        Job(
            id=job["id"],
            name=job["name"],
            status=job["status"],
            conclusion=job["conclusion"],
            check_run_id=int(job["check_run_url"].rsplit("/", 1)[1]),
            steps=tuple(Step(s["name"], s["conclusion"]) for s in job.get("steps", [])),
        )
        for page in pages
        for job in page["jobs"]
    ]


def job_log(job: Job) -> str:
    """The job's log without ANSI colors or per-line timestamps."""
    raw = _gh(
        "api",
        "--allow-escape-sequences",
        f"repos/{{owner}}/{{repo}}/actions/jobs/{job.id}/logs",
    )
    return _TIMESTAMP.sub("", _ANSI.sub("", raw))


def annotations(job: Job) -> list[Annotation]:
    data = _api(f"repos/{{owner}}/{{repo}}/check-runs/{job.check_run_id}/annotations")
    return [
        Annotation(
            job=job.name,
            level=a["annotation_level"],
            path=a["path"],
            line=a["start_line"],
            message=a["message"],
        )
        for a in data
    ]
