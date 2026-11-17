# CI/CD Documentation for Varint Library

## Overview

The varint library uses **GitHub Actions** for automated testing, validation,
and release management. Every check CI runs is a `varintdev` command (the
Python tooling in [`devtools/`](../devtools/README.md)) or a CMake/ctest
target, so any CI job can be reproduced locally with the same command.

Every workflow sets `defaults: run: shell: bash`, which runs each step with
`-eo pipefail`: a command piped into `tee` still fails its step.

---

## CI/CD Architecture

### Three-Tier Testing Strategy

```
┌─────────────────────────────────────────────────────┐
│  Tier 1: Continuous Integration (Every Push/PR)     │
│  - Strict warnings, warnings as errors (GCC + Clang)│
│  - Every unit test: ASan + UBSan, uninit audit, TSan│
│  - cppcheck static analysis                         │
│  - All examples under sanitizers                    │
│  - CMake builds on Linux and macOS                  │
└─────────────────────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────┐
│  Tier 2: Release Verification (On Tags)             │
│  - Warnings, unit tests and examples must pass      │
│  - Creates release artifacts                        │
└─────────────────────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────┐
│  Tier 3: Nightly Extended Testing (Daily @ 2 AM)    │
│  - Each sanitizer mode on its own                   │
│  - Compiler version matrix                          │
│  - Performance benchmarks                           │
│  - Valgrind memcheck over every unit test           │
└─────────────────────────────────────────────────────┘
```

Runners are pinned (`ubuntu-26.04`, `macos-latest`), so a new toolchain
arrives through an explicit workflow change. Jobs use the compilers and
cppcheck the runner image ships, which can trail upstream releases (the
ubuntu-26.04 image has GCC 13–15, Clang 20–22 and cppcheck 2.19 from the
Ubuntu archive); newer versions join the matrices when the image carries
them.

---

## Workflows

### 1. Continuous Integration (ci.yml)

**Trigger**: pushes to main/master/develop and `claude/**`, pull requests

| Job | Platforms | Command |
|-----|-----------|---------|
| Compiler Warnings | Ubuntu, macOS | `varintdev warnings gcc clang` |
| Unit Tests | Ubuntu, macOS | `varintdev ctest both`, then `varintdev ctest uninit` |
| Examples | Ubuntu, macOS | `scripts/test/test_all_comprehensive.sh` |
| CMake Build | Ubuntu, macOS × gcc, clang | Release configure, build, `ctest`, `varint-test-comprehensive` |
| ThreadSanitizer | Ubuntu | `varintdev ctest tsan` |
| Static Analysis | Ubuntu | `varintdev cppcheck` |
| Devtools | Ubuntu | `ruff format --check`, `ruff check`, `mypy` on `devtools/src`; `varintdev workflows` |
| CI Summary | Ubuntu | fails if any job above failed or was cancelled |

**Compiler Warnings** compiles every `src/*.c` on its own with
`-Werror`. The flag set follows what the compiler is, not its name (on macOS
`gcc` is Apple Clang):

```
GCC:   -O2 -Wall -Wextra -Werror
Clang: -O2 -Wall -Wextra -Wpedantic -Wsign-conversion -Wconversion -Werror
```

**Unit Tests** build the CMake tree once per mode and run every registered
ctest suite (`src/CMakeLists.txt` is the only test list):

- `both`: AddressSanitizer + UndefinedBehaviorSanitizer (no UB recovery;
  LeakSanitizer is part of ASan on Linux)
- `uninit`: Release build with `-ftrivial-auto-var-init=pattern` and a
  junk-filling allocator (`MALLOC_PERTURB_` on glibc, `MallocScribble` on
  macOS), so reads of memory that was never written see garbage
- `tsan`: ThreadSanitizer (the job lowers `vm.mmap_rnd_bits` first, which
  TSan's shadow-memory layout needs on current kernels)

**Static Analysis** runs cppcheck with `warning,style,performance,
portability` enabled. Any finding fails the job; a finding that is a
deliberate test of a known result carries an inline
`// cppcheck-suppress <id>` on the line it reports.

---

### 2. Release Verification (release.yml)

**Trigger**: tags matching `v*`, release creation

On Ubuntu and macOS, each of these must pass:

1. `varintdev warnings gcc clang`
2. `varintdev ctest both uninit`
3. `scripts/test/test_all_comprehensive.sh`

Ubuntu then packages `varint-<version>.tar.gz` (sources, docs, examples,
build files) as a 90-day artifact.

---

### 3. Nightly Extended Testing (nightly.yml)

**Trigger**: 2 AM UTC daily, or manually (`workflow_dispatch`)

| Job | What it runs |
|-----|--------------|
| Extended Sanitizers | `varintdev ctest <mode>` for each of asan, ubsan, tsan, uninit, plus every example |
| Compiler Matrix | `varintdev warnings <cc>` for each compiler the image ships: gcc-13, gcc-14, gcc-15, clang-20, clang-21, clang-22 |
| Performance Benchmarks | `varintCompare` at `-O3` |
| Valgrind | `varintdev ctest valgrind`: every unit test under memcheck, errors and definite/indirect leaks fail |
| Nightly Summary | fails if any job above failed or was cancelled |

---

## Developer Tooling (varintdev)

Run from the repository root; uv creates the environment on first use.

```bash
uv run --project devtools varintdev warnings gcc clang    # strict warnings
uv run --project devtools varintdev cppcheck              # static analysis
uv run --project devtools varintdev ctest both uninit     # unit test modes
uv run --project devtools varintdev ctest --list none     # registered tests
uv run --project devtools varintdev ci-logs [RUN_ID]      # diagnose a CI run
uv run --project devtools varintdev versions              # what is behind upstream
uv run --project devtools varintdev check                 # every CI gate, locally
```

`ctest` modes: `none`, `asan`, `ubsan`, `both`, `tsan`, `uninit`,
`valgrind`. Each builds into `build_tests/<mode>/`.

`ci-logs` downloads every job log and annotation of a run (default: the
newest run of the current branch) into `ci_logs/<run-id>/` and reports
failed jobs and steps, failed ctest suites, every compiler and cppcheck
diagnostic grouped by check and file, and annotations above notice level.

The CMake targets `varint-check-warnings` and `varint-check` call the same
commands.

---

## GitHub Actions Integration

### Viewing CI Status

**PR Checks**:
- All CI jobs appear as checks on PRs
- Must pass before merge (if branch protection enabled)
- Click "Details" to see full logs, or run `varintdev ci-logs`

**Actions Tab**:
- All workflow runs visible at `/actions`
- Filter by workflow, branch, status
- Download artifacts from completed runs

**Badges**:
README.md displays real-time status:
```markdown
![CI Status](https://github.com/mattsta/varint/actions/workflows/ci.yml/badge.svg)
```

---

### Artifacts

1. **Test Outputs** (30-day retention)
   - Comprehensive example logs per platform
   - cppcheck output

2. **Release Archives** (90-day retention)
   - Source tarball

3. **Nightly Results** (7-day retention)
   - Per-mode unit test and example logs
   - Compiler matrix results
   - Valgrind output

---

## Quality Gates

### PR Merge Requirements

**Automated Checks** (enforced by CI):
```
✅ Compiler Warnings:  every src/*.c clean on GCC and Clang
✅ Unit Tests:         every ctest suite under ASan+UBSan, uninit, TSan
✅ Static Analysis:    zero cppcheck findings
✅ Examples:           every example passes under ASan+UBSan
✅ Build:              CMake build and ctest on Linux and macOS, gcc and clang
```

**Manual Review** (human reviewer):
- Code quality
- Documentation updates
- Test coverage
- Performance impact

---

### Release Quality Gates

**Blocking Criteria** (MUST pass):
```
1. ✅ All CI checks pass
2. ✅ All nightly tests pass (last run)
3. ✅ No open high-priority bugs
4. ✅ Documentation updated
5. ✅ CHANGELOG.md updated
6. ✅ Version number incremented
```

**Release Checklist**:
- [ ] All tests passing
- [ ] Performance benchmarks acceptable
- [ ] Documentation up to date
- [ ] Migration guide (if API changes)
- [ ] Git tag created (`v*.*.*`)
- [ ] Release notes written

---

## Troubleshooting CI Failures

Start with `uv run --project devtools varintdev ci-logs <run-id>`: it names
the failed jobs, steps and ctest suites and lists every diagnostic.

### Compiler Warnings Failure

1. The job log shows each failing file with the compiler's full output
2. Reproduce: `varintdev warnings gcc clang` (or the specific compiler,
   e.g. `varintdev warnings gcc-15`)
3. Fix the warning in code; the check has no suppressions

### Unit Test Failure

1. `ci-logs` lists the failing suite and mode; ctest's
   `--output-on-failure` output is in the job log
2. Reproduce: `varintdev ctest <mode>`
3. Run one suite directly from `build_tests/<mode>/src/`

### Static Analysis Failure

1. The job log lists each finding as `file:line:col: severity: message [id]`
2. Reproduce: `varintdev cppcheck` or `varintdev cppcheck src/<file>.c`
3. Fix the code; suppress inline only when the finding is a deliberate
   test of a known result

### Example Test Failure

1. Download `comprehensive-test-output-<os>` artifact
2. Find failing example in log
3. Run locally: `./scripts/test/test_all_comprehensive.sh`
4. Check for compilation errors, runtime crashes, sanitizer violations,
   timeouts

### Sanitizer Violation

**AddressSanitizer**: buffer overflows, use-after-free, leaks (Linux)
**UndefinedBehaviorSanitizer**: signed overflow, invalid shifts, misaligned
or null access
**ThreadSanitizer**: data races, lock order violations
**uninit mode**: a result that changes when unwritten memory holds junk

**Fix Process**:
1. Read the sanitizer report or failing assertion
2. Locate source file and line number
3. Understand the root cause
4. Fix it and rerun the same `varintdev ctest <mode>`

---

## Performance Monitoring

### Benchmark Tracking

**Current**: Manual comparison of `varintCompare` output
**Future**: Automated performance regression detection

**Metrics Tracked**:
- Encode cycles/operation
- Decode cycles/operation
- Throughput (ops/second)
- Memory usage
- Cache efficiency

**Baseline Storage**: Nightly artifacts (30-day retention)

---

## Local Testing

### Before Pushing

**Quick Check**:
```bash
uv run --project devtools varintdev warnings gcc clang
uv run --project devtools varintdev ctest both
```

**Full CI Equivalent**:
```bash
uv run --project devtools varintdev warnings gcc clang
uv run --project devtools varintdev cppcheck
uv run --project devtools varintdev ctest both uninit tsan
./scripts/test/test_all_comprehensive.sh
```

---

## CI/CD Maintenance

### Updating Workflows

**File Locations**:
- `.github/workflows/ci.yml` - Main CI
- `.github/workflows/release.yml` - Release validation
- `.github/workflows/nightly.yml` - Extended testing

Before pushing, `varintdev workflows` validates the YAML and resolves every
referenced action, and `varintdev check` runs every gate. Third-party
actions are pinned to a full commit SHA with the release tag in a trailing
comment (the form `astral-sh/setup-uv` documents; it publishes no moving
major tag); `.github/dependabot.yml` updates those pins and `devtools/uv.lock`
weekly. CI runs the tooling with `uv run --locked`, so a lockfile that no
longer matches `devtools/pyproject.toml` fails instead of re-resolving.

### Updating Toolchains and Dependencies (runbook)

Run from the repository root:

1. **See what is behind**: `uv run --project devtools varintdev versions`.
   It reads the versions in use from the workflow files and compares each
   GitHub Action with its latest release, the nightly compiler matrix with
   the compilers the runner image ships (GitHub's `actions/runner-images`
   manifest) and with the newest GCC and LLVM releases, and the cppcheck
   in the image's Ubuntu archive with upstream. It exits non-zero only for
   what can be updated now.
2. **Actions**: merge Dependabot's grouped pull request, or edit the
   `uses:` lines by hand. Use the reference form the action documents: a
   major tag where it publishes one (`actions/checkout@v7`), otherwise the
   release's full commit SHA with the tag in a trailing comment
   (`astral-sh/setup-uv`). `varintdev workflows` then confirms every
   reference resolves.
3. **Compilers**: CI uses only what the runner image ships. When
   `versions` reports the image ships a compiler the nightly matrix does
   not test, add it to `compiler-matrix` in `nightly.yml`. Releases the
   image does not ship yet wait for the image.
4. **Runner image**: workflows pin `ubuntu-26.04`. When a new Ubuntu LTS
   image is available, change every `runs-on` and matrix entry, then rerun
   `versions` for that image's toolchain.
5. **devtools dependencies**: merge Dependabot's uv pull request, or run
   `uv lock --upgrade` in `devtools/`.
6. **Verify before pushing**: `uv run --project devtools varintdev check`.
7. **Verify after pushing**: `uv run --project devtools varintdev ci-logs`.
   A green job is not enough: the report lists failed tests and every
   diagnostic, and each job's log should show the tests it ran (for
   example "100% tests passed out of 28") and the files it checked.

### Adding New Tests

- **Unit test suite**: register it in `src/CMakeLists.txt` (`add_executable`
  plus `add_test`). Every `varintdev ctest` mode, and so every CI job that
  runs one, picks it up automatically.
- **New check or tool**: add a subcommand to `devtools/src/varintdev/`
  and call it from the workflow, so CI and local runs share one
  implementation.

---

## Future Enhancements

### Planned Improvements

1. **Coverage Tracking**
   - Integrate gcov/lcov
   - Generate coverage reports
   - Track coverage trends

2. **Fuzzing Integration**
   - Add OSS-Fuzz integration
   - Continuous fuzzing
   - Crash corpus management

3. **Performance Regression**
   - Automated baseline comparison
   - Performance trend graphs
   - Alert on regressions

4. **Cross-Platform Expansion**
   - Windows MSVC testing
   - ARM64 Linux builds
   - Big-endian testing

---

## Contact & Support

**CI/CD Issues**: Open GitHub issue with `[CI]` prefix
**Workflow Questions**: See `.github/CONTRIBUTING.md`
**Failures**: Run `varintdev ci-logs` first, then open an issue
