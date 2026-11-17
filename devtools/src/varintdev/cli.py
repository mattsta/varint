"""varintdev command line: varintdev <command> [options]."""

import argparse
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

from varintdev import check, ci_logs, cppcheck, ctest, warnings, workflows
from varintdev.repo import REPO_ROOT


def _check(args: argparse.Namespace) -> int:
    return check.run_all(set(args.gates) if args.gates else None)


def _ci_logs(args: argparse.Namespace) -> int:
    report = ci_logs.fetch(args.run_id, args.workflow)
    print(report.render())
    return 1 if report.run.conclusion == "failure" else 0


def _cppcheck(args: argparse.Namespace) -> int:
    paths = tuple((REPO_ROOT / p).resolve() for p in args.paths) or (
        cppcheck.Config().paths
    )
    return cppcheck.check(cppcheck.Config(paths=paths, exhaustive=args.exhaustive))


def _ctest(args: argparse.Namespace) -> int:
    action = ctest.list_tests if args.list else ctest.run_tests
    status = 0
    for mode in args.modes:
        status |= action(ctest.Mode(mode))
    return status


def _warnings(args: argparse.Namespace) -> int:
    return warnings.check(args.compilers)


def _workflows(_: argparse.Namespace) -> int:
    return workflows.check()


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="varintdev", description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    gates = commands.add_parser(
        "check", help="run every gate CI runs, locally (the pre-push check)"
    )
    gates.add_argument(
        "gates",
        nargs="*",
        choices=[g.name for g in check.GATES],
        metavar="GATE",
        help=f"run only these gates: {', '.join(g.name for g in check.GATES)}",
    )
    gates.set_defaults(handler=_check)

    logs = commands.add_parser(
        "ci-logs", help="fetch a GitHub Actions run's logs and report failures"
    )
    logs.add_argument(
        "run_id",
        nargs="?",
        type=int,
        help="run id (default: newest run of --workflow on the current branch)",
    )
    logs.add_argument(
        "--workflow",
        default="ci.yml",
        help="workflow file to take the newest run from (default: ci.yml)",
    )
    logs.set_defaults(handler=_ci_logs)

    static = commands.add_parser("cppcheck", help="run the project's cppcheck config")
    static.add_argument(
        "paths",
        nargs="*",
        type=Path,
        help="files or directories relative to the repo root (default: src)",
    )
    static.add_argument(
        "--exhaustive", action="store_true", help="use --check-level=exhaustive"
    )
    static.set_defaults(handler=_cppcheck)

    tests = commands.add_parser(
        "ctest", help="build an instrumented tree and run every ctest suite"
    )
    tests.add_argument(
        "modes",
        nargs="*",
        default=[ctest.Mode.NONE.value],
        choices=[m.value for m in ctest.Mode],
        metavar="MODE",
        help=f"one or more of: {', '.join(m.value for m in ctest.Mode)}",
    )
    tests.add_argument(
        "--list",
        action="store_true",
        help="print each mode's registered test commands instead of running",
    )
    tests.set_defaults(handler=_ctest)

    warn = commands.add_parser(
        "warnings", help="compile every src/*.c with strict warnings as errors"
    )
    warn.add_argument(
        "compilers",
        nargs="*",
        default=["gcc", "clang"],
        metavar="CC",
        help="compiler commands to check with (default: gcc clang)",
    )
    warn.set_defaults(handler=_warnings)

    flows = commands.add_parser(
        "workflows",
        help="validate workflow YAML and resolve every referenced action",
    )
    flows.set_defaults(handler=_workflows)
    return root


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        status: int = args.handler(args)
    except subprocess.CalledProcessError as exc:
        print(f"command failed with status {exc.returncode}", file=sys.stderr)
        if exc.stderr:
            print(exc.stderr, file=sys.stderr)
        return exc.returncode or 1
    return status
