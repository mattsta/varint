# varintdev

Developer and CI tooling for the varint library: a typed Python package
run through [uv](https://docs.astral.sh/uv/). Every check CI runs is a
`varintdev` command, so a CI job is reproduced locally by running the same
command.

Run from the repository root; uv creates `devtools/.venv` on first use:

```bash
uv run --project devtools varintdev <command> [options]
```

## Commands

### `check [GATE ...]`

The pre-push check: runs every gate CI runs (workflow references, ruff,
mypy, strict warnings, cppcheck, and the `both`, `uninit`, `tsan`, `leaks`
and `valgrind` test modes), all of them even after a failure, then prints a
pass/fail/skip table. Gates whose tool this machine lacks (valgrind on
macOS, leaks on Linux) are reported as skipped. Name gates to run a subset.

### `workflows`

Parses every `.github/workflows/*.yml`, requires each to define jobs, and
resolves every `uses: owner/repo@ref` against GitHub, so a reference to a
tag or branch that does not exist fails before a push.

### `warnings [CC ...]`

Compiles every `src/*.c` on its own with the compiler family's strict flag
set and `-Werror`; any diagnostic fails. Defaults to `gcc clang`. The flag
set follows what the compiler is, not its name (on macOS `gcc` is Apple
Clang and gets the Clang flags).

```bash
varintdev warnings                 # gcc and clang
varintdev warnings gcc-15 clang-21 # specific versions
```

### `cppcheck [PATH ...] [--exhaustive]`

Runs the project's cppcheck configuration (enabled checks and suppressions
are defined in `cppcheck.py`) over `src/` or the given files/directories.
Prints each finding and a summary by check and file; any finding fails.

### `ctest [MODE ...] [--list]`

Builds the CMake tree into `build_tests/<mode>/` and runs every registered
ctest suite. `src/CMakeLists.txt` is the only test list.

| Mode | Build | What it catches |
|------|-------|-----------------|
| `none` | Debug | functional failures |
| `asan` | Debug, AddressSanitizer | out-of-bounds, use-after-free, leaks (Linux) |
| `ubsan` | Debug, UBSan, no recovery | signed overflow, bad shifts, misaligned access |
| `both` | Debug, ASan + UBSan | both of the above |
| `tsan` | Debug, ThreadSanitizer | data races |
| `uninit` | Release, `-ftrivial-auto-var-init=pattern`, junk-filling allocator | reads of memory that was never written |
| `valgrind` | Debug, each test under memcheck (Linux) | invalid access, uninitialized use, definite/indirect leaks |
| `leaks` | Debug, each test under the macOS `leaks` tool | allocations still leaked at exit (macOS ASan has no LeakSanitizer) |

`--list` prints each registered test's command and working directory from
an already-configured mode.

### `ci-logs [RUN_ID] [--workflow FILE]`

Downloads every job log and annotation of a GitHub Actions run (default:
newest `ci.yml` run of the current branch) into `ci_logs/<run-id>/`, then reports
failed jobs and steps, failed ctest suites, every compiler and cppcheck
diagnostic grouped by check and file, and annotations above notice level.
Requires an authenticated `gh`.

## Development

```bash
cd devtools
uv run ruff format src
uv run ruff check src
uv run mypy src          # strict
```

CI runs all three on every push.
