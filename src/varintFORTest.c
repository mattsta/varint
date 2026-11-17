#include "varintFOR.h"
#include "ctest.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int varintFORTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Width computation") {
        /* Range 0-255 should need 1 byte */
        if (varintFORComputeWidth(255) != VARINT_WIDTH_8B) {
            ERR("Width for range 255 = %d, expected %d",
                varintFORComputeWidth(255), VARINT_WIDTH_8B);
        }

        /* Range 0-65535 should need 2 bytes */
        if (varintFORComputeWidth(65535) != VARINT_WIDTH_16B) {
            ERR("Width for range 65535 = %d, expected %d",
                varintFORComputeWidth(65535), VARINT_WIDTH_16B);
        }

        /* Range 0-16777215 should need 3 bytes */
        if (varintFORComputeWidth(16777215) != VARINT_WIDTH_24B) {
            ERR("Width for range 16777215 = %d, expected %d",
                varintFORComputeWidth(16777215), VARINT_WIDTH_24B);
        }
    }

    TEST("Basic FOR encode/decode") {
        const uint64_t values[] = {100, 105, 110, 115, 120};
        size_t count = 5;
        uint8_t buffer[256];
        varintFORMeta meta = {0};

        size_t encoded = varintFOREncode(buffer, values, count, &meta);
        if (encoded == 0) {
            ERRR("Failed to encode FOR array");
        }

        uint64_t decoded[5];
        size_t decoded_count = varintFORDecode(buffer, decoded, 5);

        if (decoded_count != count) {
            ERR("Decoded count %zu != original count %zu", decoded_count,
                count);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
            }
        }
    }

    TEST("FOR metadata analysis") {
        const uint64_t values[] = {1000, 1010, 1020, 1030};
        size_t count = 4;
        varintFORMeta meta;

        varintFORAnalyze(values, count, &meta);

        if (meta.minValue != 1000) {
            ERR("Min value = %" PRIu64 ", expected 1000", meta.minValue);
        }
        if (meta.maxValue != 1030) {
            ERR("Max value = %" PRIu64 ", expected 1030", meta.maxValue);
        }
        if (meta.range != 30) {
            ERR("Range = %" PRIu64 ", expected 30", meta.range);
        }
        if (meta.count != 4) {
            ERR("Count = %zu, expected 4", meta.count);
        }
    }

    TEST("Tight cluster compression") {
        /* Values clustered tightly (range 100) */
        uint64_t values[100];
        uint64_t base = 1000000;
        for (int i = 0; i < 100; i++) {
            values[i] = base + (uint64_t)i;
        }

        uint8_t buffer[1024];
        varintFORMeta meta = {0};
        size_t encoded = varintFOREncode(buffer, values, 100, &meta);

        /* Should use 1 byte per offset (range < 256) */
        /* Naive encoding: 100 * 8 = 800 bytes */
        /* FOR encoding: header + 100 * 1 byte ≈ 110-120 bytes */
        if (encoded >= 200) {
            ERR("FOR not efficient for tight cluster: %zu bytes", encoded);
        }

        /* Verify correctness */
        uint64_t decoded[100];
        size_t decoded_count = varintFORDecode(buffer, decoded, 100);
        (void)decoded_count; /* Intentionally unused in test */

        for (int i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value[%d] mismatch: %" PRIu64 " != %" PRIu64 "", i,
                    decoded[i], values[i]);
                break;
            }
        }
    }

    TEST("Random access with varintFORGetAt") {
        const uint64_t values[] = {500, 510, 520, 530, 540};
        size_t count = 5;
        uint8_t buffer[256];
        varintFORMeta meta = {0};

        varintFOREncode(buffer, values, count, &meta);

        /* Access individual elements */
        for (size_t i = 0; i < count; i++) {
            uint64_t val = varintFORGetAt(buffer, i);
            if (val != values[i]) {
                ERR("GetAt(%zu) = %" PRIu64 ", expected %" PRIu64 "", i, val,
                    values[i]);
            }
        }
    }

    TEST("Single value array") {
        const uint64_t value[] = {12345};
        uint8_t buffer[64];
        varintFORMeta meta = {0}; /* Encode trusts a meta whose count matches */

        size_t encoded = varintFOREncode(buffer, value, 1, &meta);
        (void)encoded; /* Intentionally unused in test */
        uint64_t decoded[1];
        size_t count = varintFORDecode(buffer, decoded, 1);

        if (count != 1) {
            ERR("Decoded count = %zu, expected 1", count);
        }
        if (decoded[0] != value[0]) {
            ERR("Decoded value = %" PRIu64 ", expected %" PRIu64 "", decoded[0],
                value[0]);
        }
    }

    TEST("Large range values") {
        const uint64_t values[] = {0, 100000000, 200000000};
        size_t count = 3;
        uint8_t buffer[256];
        varintFORMeta meta = {0}; /* Encode trusts a meta whose count matches */

        size_t encoded = varintFOREncode(buffer, values, count, &meta);
        uint64_t decoded[3];
        (void)encoded; /* Intentionally unused in test */
        size_t decoded_count = varintFORDecode(buffer, decoded, 3);
        (void)decoded_count; /* Intentionally unused in test */

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Large value[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
            }
        }
    }

    TEST("All identical values") {
        uint64_t values[10];
        for (int i = 0; i < 10; i++) {
            values[i] = 777;
        }

        uint8_t buffer[256];
        varintFORMeta meta = {0}; /* Encode trusts a meta whose count matches */
        size_t encoded = varintFOREncode(buffer, values, 10, &meta);
        (void)encoded; /* Intentionally unused in test */

        /* Should be very efficient (all offsets = 0) */
        uint64_t decoded[10];
        size_t count = varintFORDecode(buffer, decoded, 10);
        (void)count; /* Intentionally unused in test */

        for (int i = 0; i < 10; i++) {
            if (decoded[i] != 777) {
                ERR("Identical value[%d] = %" PRIu64 ", expected 777", i,
                    decoded[i]);
            }
        }
    }

    TEST("Batch analysis matches scalar analysis across the full range") {
        /* Values straddle 2^63 so a signed lane compare would misorder them;
         * the extremes sit in different SIMD lanes and in the scalar tail. */
        uint64_t values[37];
        for (size_t i = 0; i < 37; i++) {
            values[i] =
                (UINT64_C(1) << 63) + (i * UINT64_C(0x9E3779B97F4A7C15));
        }
        values[5] = UINT64_MAX;
        values[18] = 0;
        values[30] = (UINT64_C(1) << 63) - 1;
        values[31] = UINT64_C(1) << 63;

        /* 16 and 32 run only full vectors; the rest add a scalar tail */
        const size_t counts[] = {16, 17, 19, 32, 33, 37};
        for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
            varintFORMeta scalar = {0};
            varintFORMeta batch = {0};
            varintFORAnalyze(values, counts[c], &scalar);
            varintFORBatchAnalyze(values, counts[c], &batch);

            if (batch.minValue != scalar.minValue ||
                batch.maxValue != scalar.maxValue ||
                batch.range != scalar.range ||
                batch.offsetWidth != scalar.offsetWidth ||
                batch.encodedSize != scalar.encodedSize) {
                ERR("count %zu: batch min %" PRIu64 " max %" PRIu64
                    ", scalar min %" PRIu64 " max %" PRIu64,
                    counts[c], batch.minValue, batch.maxValue, scalar.minValue,
                    scalar.maxValue);
            }
        }

        /* Data entirely above 2^63 with a narrow range */
        uint64_t high[20];
        for (size_t i = 0; i < 20; i++) {
            high[i] = UINT64_MAX - (i * 7);
        }
        varintFORMeta scalar = {0};
        varintFORMeta batch = {0};
        varintFORAnalyze(high, 20, &scalar);
        varintFORBatchAnalyze(high, 20, &batch);
        if (batch.minValue != scalar.minValue ||
            batch.maxValue != scalar.maxValue ||
            batch.offsetWidth != scalar.offsetWidth) {
            ERR("high data: batch min %" PRIu64 " max %" PRIu64
                ", scalar min %" PRIu64 " max %" PRIu64,
                batch.minValue, batch.maxValue, scalar.minValue,
                scalar.maxValue);
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_FOR_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintFORTest(argc, argv);
}
#endif
