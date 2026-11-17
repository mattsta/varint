"""Report how the CI toolchain compares with what is available upstream.

For every GitHub Action the workflows use, the compilers in the nightly
compiler matrix, and the cppcheck CI installs, this shows what is in use,
what the runner image or Ubuntu archive offers, and the newest upstream
release. Versions in use are read from the workflow files themselves.

It exits non-zero only for things this repository can act on now: an
action behind its latest release, or a compiler the runner image ships
that the nightly matrix does not test. Upstream releases the runner image
does not carry yet are reported but are not failures; they join CI when
the image ships them.
"""

import base64
import json
import re
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any

from varintdev import github, workflows

LAUNCHPAD = "https://api.launchpad.net/devel"
_SHA = re.compile(r"^[0-9a-f]{40}$")
_IMAGE_TOOL = re.compile(r"^- (?P<tool>Clang|GNU C\+\+): (?P<versions>.+)$", re.M)


@dataclass(frozen=True, slots=True)
class Row:
    item: str
    in_use: str
    available: str
    latest: str
    status: str
    actionable: bool = False


def _latest_release(owner: str, repo: str) -> str:
    return str(github.api(f"repos/{owner}/{repo}/releases/latest")["tag_name"])


def _tag_commit(owner: str, repo: str, tag: str) -> str:
    return str(github.api(f"repos/{owner}/{repo}/commits/{tag}")["sha"])


def _major(version: str) -> int:
    match = re.search(r"\d+", version)
    return int(match[0]) if match else -1


def _version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", version))


def action_rows(flows: list[workflows.Workflow]) -> list[Row]:
    refs = sorted(
        {
            ref
            for flow in flows
            for uses in flow.uses()
            if (ref := workflows.ActionRef.parse(uses)) is not None
        }
    )
    rows = []
    for ref in refs:
        latest = _latest_release(ref.owner, ref.repo)
        if _SHA.match(ref.ref):
            current = _tag_commit(ref.owner, ref.repo, latest) == ref.ref
            in_use = f"{ref.ref[:12]} (SHA pin)"
        else:
            current = ref.ref == latest or latest.startswith(f"{ref.ref}.")
            in_use = ref.ref
        rows.append(
            Row(
                item=f"{ref.owner}/{ref.repo}",
                in_use=in_use,
                available=latest,
                latest=latest,
                status="current" if current else "behind latest release",
                actionable=not current,
            )
        )
    return rows


def _runner_images(flows: list[workflows.Workflow]) -> set[str]:
    images: set[str] = set()
    for flow in flows:
        for job in flow.doc["jobs"].values():
            runs_on = job.get("runs-on", "")
            images.update(re.findall(r"ubuntu-\d+\.\d+", json.dumps(runs_on)))
            matrix = job.get("strategy", {}).get("matrix", {})
            images.update(re.findall(r"ubuntu-\d+\.\d+", json.dumps(matrix)))
    return images


def image_toolchain(image: str) -> dict[str, list[str]]:
    """Compiler versions an Ubuntu runner image ships, from runner-images."""
    readme = f"Ubuntu{image.removeprefix('ubuntu-').replace('.', '')}-Readme.md"
    content = github.api(
        f"repos/actions/runner-images/contents/images/ubuntu/{readme}"
    )["content"]
    text = base64.b64decode(content).decode()
    return {
        m["tool"]: [v.strip() for v in m["versions"].split(",")]
        for m in _IMAGE_TOOL.finditer(text)
    }


def _matrix_compilers(flows: list[workflows.Workflow]) -> list[str]:
    for flow in flows:
        job = flow.doc["jobs"].get("compiler-matrix")
        if job is not None:
            return [str(c) for c in job["strategy"]["matrix"]["compiler"]]
    return []


def _latest_gcc() -> str:
    refs = github.api("repos/gcc-mirror/gcc/git/matching-refs/tags/releases/gcc-")
    versions = [
        str(r["ref"]).removeprefix("refs/tags/releases/gcc-")
        for r in refs
        if re.fullmatch(r"refs/tags/releases/gcc-\d+\.\d+\.\d+", r["ref"])
    ]
    return max(versions, key=_version_key)


def _latest_llvm() -> str:
    return _latest_release("llvm", "llvm-project").removeprefix("llvmorg-")


def compiler_rows(flows: list[workflows.Workflow], image: str) -> list[Row]:
    shipped = image_toolchain(image)
    matrix = _matrix_compilers(flows)
    rows = []
    for name, tool, prefix, latest in (
        ("GCC", "GNU C++", "gcc-", _latest_gcc()),
        ("Clang", "Clang", "clang-", _latest_llvm()),
    ):
        image_majors = sorted({_major(v) for v in shipped.get(tool, [])})
        tested = sorted(
            int(c.removeprefix(prefix)) for c in matrix if c.startswith(prefix)
        )
        untested = [m for m in image_majors if m not in tested]
        if untested:
            status = f"image ships {untested} not in the nightly matrix"
        elif image_majors and _major(latest) > max(image_majors):
            status = f"{image} does not ship {_major(latest)} yet"
        else:
            status = "current"
        rows.append(
            Row(
                item=f"{name} (nightly matrix)",
                in_use=", ".join(map(str, tested)) or "none",
                available=f"{image}: {', '.join(map(str, image_majors))}",
                latest=latest,
                status=status,
                actionable=bool(untested),
            )
        )
    return rows


def _launchpad(path: str, **params: str) -> Any:
    query = f"?{urllib.parse.urlencode(params)}" if params else ""
    with urllib.request.urlopen(f"{LAUNCHPAD}/{path}{query}", timeout=30) as reply:
        return json.load(reply)


def ubuntu_package_version(ubuntu_version: str, source: str) -> str | None:
    """Published version of a source package in an Ubuntu release."""
    series = next(
        (
            entry
            for entry in _launchpad("ubuntu/series")["entries"]
            if entry["version"] == ubuntu_version
        ),
        None,
    )
    if series is None:
        return None
    published = _launchpad(
        "ubuntu/+archive/primary",
        **{
            "ws.op": "getPublishedSources",
            "source_name": source,
            "exact_match": "true",
            "distro_series": series["self_link"],
            "status": "Published",
        },
    )["entries"]
    versions = [str(e["source_package_version"]) for e in published]
    return max(versions, key=_version_key) if versions else None


def cppcheck_row(image: str) -> Row:
    archive = ubuntu_package_version(image.removeprefix("ubuntu-"), "cppcheck")
    latest = _latest_release("danmar", "cppcheck")
    in_archive = archive.split("-")[0] if archive else "not packaged"
    behind = archive is not None and _version_key(in_archive) < _version_key(latest)
    return Row(
        item="cppcheck (apt)",
        in_use=in_archive,
        available=f"{image} archive: {in_archive}",
        latest=latest,
        status=f"{image} archive trails upstream" if behind else "current",
    )


def report() -> int:
    flows, problems = workflows.load()
    for problem in problems:
        print(f"FAIL {problem}")
    images = sorted(_runner_images(flows), key=_version_key)
    image = images[-1] if images else None

    rows = action_rows(flows)
    if image is not None:
        rows += compiler_rows(flows, image)
        rows += [cppcheck_row(image)]

    headers = ("item", "in use", "available", "latest upstream", "status")
    table = [headers] + [
        (r.item, r.in_use, r.available, r.latest, r.status) for r in rows
    ]
    widths = [max(len(line[i]) for line in table) for i in range(len(headers))]
    for line in table:
        print("  ".join(cell.ljust(w) for cell, w in zip(line, widths, strict=True)))

    actionable = [r for r in rows if r.actionable]
    if actionable or problems:
        print(f"\n{len(actionable) + len(problems)} item(s) need updating here")
        return 1
    print("\nnothing to update here; upstream-only releases wait for the image")
    return 0
