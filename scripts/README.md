# Scripts Directory

Example and trie-system test scripts, plus workflow validation. Compiler
warning checks, static analysis, and the unit-test modes (sanitizers,
uninitialized-memory audit, valgrind) live in the `varintdev` Python tooling;
see [`devtools/README.md`](../devtools/README.md).

## Directory Structure

```
scripts/
├── build/
│   └── validate_workflows.py   # YAML-validate .github/workflows before pushing
└── test/           # Test execution scripts
    ├── run_palette_fuzz.sh             # Palette codec fuzz driver
    ├── test_all_comprehensive.sh       # All examples with sanitizers
    ├── test_all_examples_sanitizers.sh # All examples via CMake
    ├── test_trie_comprehensive.sh      # Trie system full test suite
    ├── test_trie_fast.sh               # Trie fast tests (no valgrind)
    ├── test_trie_memory_safety.sh      # Trie with valgrind
    ├── test_trie_sanitizers.sh         # Trie with ASan+UBSan
    └── test_trie_server.sh             # Trie server basic tests
```

## Build Scripts

### `scripts/build/validate_workflows.py`

Parses every `.github/workflows/*.yml` and fails naming the first file that
is not valid YAML or has no `jobs:`.

```bash
scripts/build/validate_workflows.py
```

## Test Scripts

### Example Tests

#### `scripts/test/test_all_comprehensive.sh`

Comprehensive sanitizer testing for all 43 examples.

**Usage:**

```bash
./scripts/test/test_all_comprehensive.sh
```

**What it tests:**

- 16 standalone examples
- 9 integration examples
- 15 advanced examples
- 3 reference examples
- All with AddressSanitizer + UndefinedBehaviorSanitizer

**Duration:** 2-5 minutes depending on system

#### `scripts/test/test_all_examples_sanitizers.sh`

Alternative comprehensive example testing using CMake build system.

**Usage:**

```bash
./scripts/test/test_all_examples_sanitizers.sh
```

### Trie Server/Client Tests

These scripts test the advanced trie server/client example system.

#### `scripts/test/test_trie_comprehensive.sh`

Full trie system test suite with persistence and valgrind.

**Usage:**

```bash
./scripts/test/test_trie_comprehensive.sh
```

**What it tests:**

- All 10 trie commands (PING, INSERT, SEARCH, etc.)
- Persistence (save/load)
- Memory safety with valgrind
- Repeated automation tests

**Duration:** 1-3 minutes

#### `scripts/test/test_trie_fast.sh`

Fast trie tests without valgrind (for quick validation).

**Usage:**

```bash
./scripts/test/test_trie_fast.sh
```

**Duration:** 10-30 seconds

#### `scripts/test/test_trie_memory_safety.sh`

Focused memory safety testing with valgrind.

**Usage:**

```bash
./scripts/test/test_trie_memory_safety.sh
```

**Valgrind options:**

- Full leak checking
- Track origins
- Show all leak kinds
- Error exit code on failure

#### `scripts/test/test_trie_sanitizers.sh`

Modern sanitizer testing (ASan + UBSan) for trie system.

**Usage:**

```bash
./scripts/test/test_trie_sanitizers.sh
```

**Faster than valgrind, catches:**

- Buffer overflows
- Use-after-free
- Memory leaks
- Undefined behavior
- Integer overflows

#### `scripts/test/test_trie_server.sh`

Basic trie server functionality tests.

**Usage:**

```bash
./scripts/test/test_trie_server.sh
```

## Quick Reference

**Pre-commit checks:**

```bash
uv run --project devtools varintdev warnings gcc clang  # Compiler warnings
uv run --project devtools varintdev cppcheck            # Static analysis
uv run --project devtools varintdev ctest both uninit   # Unit tests
./scripts/test/test_all_comprehensive.sh                # All examples
```

**Fast validation:**

```bash
make test                                # CMake unit tests
./scripts/test/test_trie_fast.sh        # Quick trie tests
```

**Memory safety deep dive:**

```bash
uv run --project devtools varintdev ctest valgrind  # Valgrind (Linux)
./scripts/test/test_trie_memory_safety.sh           # Trie under valgrind
./scripts/test/test_trie_sanitizers.sh              # Trie with ASan+UBSan
./scripts/test/test_all_comprehensive.sh            # All examples
```

## CMake Integration

```bash
make test                       # Runs all unit tests via CTest
make varint-test-comprehensive  # Runs test_all_comprehensive.sh
make varint-check-warnings      # Runs varintdev warnings gcc clang
make varint-check               # Runs varintdev cppcheck, then all tests
```

## CI/CD Integration

See [`.github/CI_CD_DOCUMENTATION.md`](../.github/CI_CD_DOCUMENTATION.md).
`test_all_comprehensive.sh` runs in the ci.yml Examples job, the release
gate, and every nightly sanitizer job.

## Output Artifacts

Scripts create the following artifacts (all gitignored):

- `build/` - CMake build artifacts
- `build_tests/` - Unit test build artifacts (one directory per varintdev mode)
- `build_examples_sanitizers/` - Example builds with sanitizers
- `build_sanitizers/` - Trie server sanitizer builds

## Exit Codes

All scripts follow standard exit code conventions:

- `0` - Success, all tests/checks passed
- `1` - Failure, one or more tests/checks failed
- `>1` - Error in script execution

## Notes

- All scripts automatically navigate to repository root
- Scripts can be run from any directory
- Build artifacts are created in repository root
