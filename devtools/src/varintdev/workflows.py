"""Static checks of the GitHub Actions workflows.

Every .github/workflows/*.yml must parse, define jobs, and reference only
actions that exist: each `uses: owner/repo[/path]@ref` is resolved
against GitHub, so a tag that was never published fails here instead of
failing every job that uses it at "Set up job".
"""

import subprocess
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from varintdev.repo import REPO_ROOT

WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"


@dataclass(frozen=True, order=True, slots=True)
class ActionRef:
    owner: str
    repo: str
    ref: str

    @classmethod
    def parse(cls, uses: str) -> "ActionRef | None":
        """None for references that do not name a GitHub repository."""
        if uses.startswith(("./", "docker://")) or "@" not in uses:
            return None
        target, ref = uses.rsplit("@", 1)
        owner, repo, *_ = target.split("/")
        return cls(owner, repo, ref)

    def __str__(self) -> str:
        return f"{self.owner}/{self.repo}@{self.ref}"

    def resolves(self) -> bool:
        """True when ref names a branch, tag or commit of the repository."""
        result = subprocess.run(
            [
                "gh",
                "api",
                f"repos/{self.owner}/{self.repo}/commits/{self.ref}",
                "--jq",
                ".sha",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        return result.returncode == 0 and bool(result.stdout.strip())


@dataclass(frozen=True, slots=True)
class Workflow:
    path: Path
    doc: dict[str, Any]

    @property
    def name(self) -> str:
        return str(self.path.relative_to(REPO_ROOT))

    def uses(self) -> Iterator[str]:
        for job in self.doc.get("jobs", {}).values():
            if "uses" in job:
                yield job["uses"]
            for step in job.get("steps", []):
                if "uses" in step:
                    yield step["uses"]


def load() -> tuple[list[Workflow], list[str]]:
    """Parsed workflows, plus a problem description per broken file."""
    workflows: list[Workflow] = []
    problems: list[str] = []
    paths = sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(WORKFLOW_DIR.glob("*.yaml"))
    if not paths:
        problems.append(f"no workflow files under {WORKFLOW_DIR}")
    for path in paths:
        name = path.relative_to(REPO_ROOT)
        try:
            doc = yaml.safe_load(path.read_text())
        except yaml.YAMLError as exc:
            problems.append(f"{name}: invalid YAML: {exc}")
            continue
        if not isinstance(doc, dict) or not doc.get("jobs"):
            problems.append(f"{name}: parsed but defines no jobs")
            continue
        workflows.append(Workflow(path, doc))
    return workflows, problems


def check() -> int:
    workflows, problems = load()
    refs: dict[ActionRef, set[str]] = {}
    for workflow in workflows:
        print(f"OK   {workflow.name} ({len(workflow.doc['jobs'])} jobs)")
        for uses in workflow.uses():
            ref = ActionRef.parse(uses)
            if ref is not None:
                refs.setdefault(ref, set()).add(workflow.name)

    for ref in sorted(refs):
        if ref.resolves():
            print(f"OK   {ref}")
        else:
            users = ", ".join(sorted(refs[ref]))
            problems.append(f"{ref} does not resolve (used in {users})")

    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(f"{len(workflows)} workflows valid, {len(refs)} action references resolve")
    return 0
