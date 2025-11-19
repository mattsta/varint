#!/bin/bash
# Comprehensive compiler warning verification for varint library
# Tests with both GCC and Clang strict warnings

set -e

RESULTS_DIR="compiler_check_results"
mkdir -p "$RESULTS_DIR"

echo "========================================"
echo "Varint Library Compiler Warning Check"
echo "========================================"
echo ""
echo "Testing with GCC and Clang strict warnings..."
echo ""

# Run GCC check
echo "=== Running GCC Check ==="
if /home/user/varint/check_warnings.sh gcc > "$RESULTS_DIR/gcc_results.txt" 2>&1; then
    echo "✓ GCC: ALL FILES CLEAN"
    GCC_STATUS="PASS"
else
    echo "✗ GCC: Some files have warnings"
    GCC_STATUS="FAIL"
fi

# Run Clang check
echo "=== Running Clang Check ==="
if /home/user/varint/check_warnings.sh clang > "$RESULTS_DIR/clang_results.txt" 2>&1; then
    echo "✓ Clang: ALL FILES CLEAN"
    CLANG_STATUS="PASS"
else
    echo "✗ Clang: Some files have warnings"
    CLANG_STATUS="FAIL"
fi

echo ""
echo "========================================"
echo "Summary"
echo "========================================"
echo "GCC:   $GCC_STATUS"
echo "Clang: $CLANG_STATUS"
echo ""
echo "Detailed results saved to: $RESULTS_DIR/"
echo ""

# Show summary from each
echo "=== GCC Summary ===="
grep -E "(Results:|Failed files:)" "$RESULTS_DIR/gcc_results.txt" -A 10 || echo "All files passed!"

echo ""
echo "=== Clang Summary ==="
grep -E "(Results:|Failed files:)" "$RESULTS_DIR/clang_results.txt" -A 10 || echo "All files passed!"

# Exit with error if either compiler failed
if [ "$GCC_STATUS" = "FAIL" ] || [ "$CLANG_STATUS" = "FAIL" ]; then
    exit 1
fi

exit 0
