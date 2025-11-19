#!/bin/bash
# Automated warning checker for varint library
# Checks all source files with both GCC and Clang strict warnings

set -e

COMPILER="${1:-clang}"
SRC_DIR="src"
FAILED_FILES=()
TOTAL=0
PASSED=0

echo "================================="
echo "Checking with: $COMPILER"
echo "================================="
echo ""

if [ "$COMPILER" = "clang" ]; then
    FLAGS="-O2 -Wall -Wextra -Wpedantic -Wsign-conversion -Wconversion -Werror"
else
    FLAGS="-O2 -Wall -Wextra -Werror"
fi

# Find all .c files
for file in $(find "$SRC_DIR" -name "*.c" | sort); do
    TOTAL=$((TOTAL + 1))
    printf "Checking %-40s ... " "$file"

    if $COMPILER $FLAGS -I src -c "$file" -o /tmp/test.o 2>/tmp/err.log; then
        echo "✓"
        PASSED=$((PASSED + 1))
    else
        echo "✗"
        FAILED_FILES+=("$file")
        echo "  Errors:"
        head -5 /tmp/err.log | sed 's/^/    /'
        echo ""
    fi
done

echo ""
echo "================================="
echo "Results: $PASSED/$TOTAL files passed"
echo "================================="

if [ ${#FAILED_FILES[@]} -gt 0 ]; then
    echo ""
    echo "Failed files:"
    for file in "${FAILED_FILES[@]}"; do
        echo "  - $file"
    done
    exit 1
fi

exit 0
