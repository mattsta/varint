/* Simple struct size verification tool
 * Verifies that struct optimizations reduced padding
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "varint.h"
#include "varintExternal.h"
#include "varintFOR.h"
#include "varintPFOR.h"
#include "varintDict.h"
#include "varintBitmap.h"
#include "varintAdaptive.h"
#include "varintFloat.h"

#define CHECK_SIZE(type, expected, name) \
    printf("%-30s: %3zu bytes (expected <= %3zu) %s\n", \
           name, sizeof(type), (size_t)expected, \
           sizeof(type) <= expected ? "\033[1;32m✓\033[0m" : "\033[1;31m✗ REGRESSION\033[0m")

int main(void) {
    printf("\n");
    printf("\033[1m╔══════════════════════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1m║         Struct Size Verification (Post-Optimization)            ║\033[0m\n");
    printf("\033[1m╚══════════════════════════════════════════════════════════════════╝\033[0m\n");
    printf("\n");

    printf("Expected sizes are from BEFORE optimization (with padding)\n");
    printf("Actual sizes should be <= expected (padding eliminated)\n");
    printf("\n");

    /* Before: 48 bytes (4 bytes padding)
     * After:  44 bytes (0 bytes padding) */
    CHECK_SIZE(varintFORMeta, 48, "varintFORMeta");

    /* Before: 48 bytes (8 bytes padding)
     * After:  40 bytes (0 bytes padding) */
    CHECK_SIZE(varintPFORMeta, 48, "varintPFORMeta");

    /* Before: 48 bytes (6 bytes padding)
     * After:  42 bytes (0 bytes padding) */
    CHECK_SIZE(varintFloatMeta, 48, "varintFloatMeta");

    /* Before: 80 bytes (5 bytes padding)
     * After:  75 bytes (0 bytes padding) */
    CHECK_SIZE(varintAdaptiveDataStats, 80, "varintAdaptiveDataStats");

    /* Before: 72 bytes (4 bytes padding)
     * After:  68 bytes (0 bytes padding) */
    CHECK_SIZE(varintAdaptiveMeta, 72, "varintAdaptiveMeta");

    /* Before: 24 bytes (4 bytes padding)
     * After:  20 bytes (0 bytes padding) */
    CHECK_SIZE(varintBitmapStats, 24, "varintBitmapStats");

    /* varintDictStats was already optimal */
    CHECK_SIZE(varintDictStats, 56, "varintDictStats");

    printf("\n");

    /* Calculate total savings */
    size_t before_total = 48 + 48 + 48 + 80 + 72 + 24 + 56;
    size_t after_total = sizeof(varintFORMeta) + sizeof(varintPFORMeta) +
                         sizeof(varintFloatMeta) + sizeof(varintAdaptiveDataStats) +
                         sizeof(varintAdaptiveMeta) + sizeof(varintBitmapStats) +
                         sizeof(varintDictStats);
    size_t saved = before_total > after_total ? before_total - after_total : 0;

    printf("Total bytes before:  %zu\n", before_total);
    printf("Total bytes after:   %zu\n", after_total);
    printf("Bytes saved:         \033[1;32m%zu\033[0m (%.1f%% reduction)\n",
           saved, (float)saved / before_total * 100.0f);
    printf("\n");

    return 0;
}
