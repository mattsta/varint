#include "varintFOR.h"
#include "ctest.h"
#include <string.h>
#include <stdlib.h>

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
        uint64_t values[] = {100, 105, 110, 115, 120};
        size_t count = 5;
        uint8_t buffer[256];
        varintFORMeta meta;

        size_t encoded = varintFOREncode(buffer, values, count, &meta);
        if (encoded == 0) {
            ERRR("Failed to encode FOR array");
        }

        uint64_t decoded[5];
        size_t decoded_count = varintFORDecode(buffer, decoded, 5);

        if (decoded_count != count) {
            ERR("Decoded count %zu != original count %zu", decoded_count, count);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%zu] = %lu, expected %lu", i, decoded[i], values[i]);
            }
        }
    }

    TEST("FOR metadata analysis") {
        uint64_t values[] = {1000, 1010, 1020, 1030};
        size_t count = 4;
        varintFORMeta meta;

        varintFORAnalyze(values, count, &meta);

        if (meta.minValue != 1000) {
            ERR("Min value = %lu, expected 1000", meta.minValue);
        }
        if (meta.maxValue != 1030) {
            ERR("Max value = %lu, expected 1030", meta.maxValue);
        }
        if (meta.range != 30) {
            ERR("Range = %lu, expected 30", meta.range);
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
            values[i] = base + i;
        }

        uint8_t buffer[1024];
        varintFORMeta meta;
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

        for (int i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value[%d] mismatch: %lu != %lu", i, decoded[i], values[i]);
                break;
            }
        }
    }

    TEST("Random access with varintFORGetAt") {
        uint64_t values[] = {500, 510, 520, 530, 540};
        size_t count = 5;
        uint8_t buffer[256];
        varintFORMeta meta;

        varintFOREncode(buffer, values, count, &meta);

        /* Access individual elements */
        for (size_t i = 0; i < count; i++) {
            uint64_t val = varintFORGetAt(buffer, i);
            if (val != values[i]) {
                ERR("GetAt(%zu) = %lu, expected %lu", i, val, values[i]);
            }
        }
    }

    TEST("Single value array") {
        uint64_t value[] = {12345};
        uint8_t buffer[64];
        varintFORMeta meta;

        size_t encoded = varintFOREncode(buffer, value, 1, &meta);
        uint64_t decoded[1];
        size_t count = varintFORDecode(buffer, decoded, 1);

        if (count != 1) {
            ERR("Decoded count = %zu, expected 1", count);
        }
        if (decoded[0] != value[0]) {
            ERR("Decoded value = %lu, expected %lu", decoded[0], value[0]);
        }
    }

    TEST("Large range values") {
        uint64_t values[] = {0, 100000000, 200000000};
        size_t count = 3;
        uint8_t buffer[256];
        varintFORMeta meta;

        size_t encoded = varintFOREncode(buffer, values, count, &meta);
        uint64_t decoded[3];
        size_t decoded_count = varintFORDecode(buffer, decoded, 3);

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Large value[%zu] = %lu, expected %lu", i, decoded[i], values[i]);
            }
        }
    }

    TEST("All identical values") {
        uint64_t values[10];
        for (int i = 0; i < 10; i++) {
            values[i] = 777;
        }

        uint8_t buffer[256];
        varintFORMeta meta;
        size_t encoded = varintFOREncode(buffer, values, 10, &meta);

        /* Should be very efficient (all offsets = 0) */
        uint64_t decoded[10];
        size_t count = varintFORDecode(buffer, decoded, 10);

        for (int i = 0; i < 10; i++) {
            if (decoded[i] != 777) {
                ERR("Identical value[%d] = %lu, expected 777", i, decoded[i]);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_FOR_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintFORTest(argc, argv);
}
#endif
