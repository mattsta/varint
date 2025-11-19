#include "varintDelta.h"
#include "ctest.h"
#include <string.h>
#include <stdlib.h>

int varintDeltaTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("ZigZag encoding/decoding") {
        /* Test ZigZag mapping: 0→0, -1→1, 1→2, -2→3, 2→4 */
        if (varintDeltaZigZag(0) != 0) {
            ERR("ZigZag(0) = %lu, expected 0", varintDeltaZigZag(0));
        }
        if (varintDeltaZigZag(-1) != 1) {
            ERR("ZigZag(-1) = %lu, expected 1", varintDeltaZigZag(-1));
        }
        if (varintDeltaZigZag(1) != 2) {
            ERR("ZigZag(1) = %lu, expected 2", varintDeltaZigZag(1));
        }
        if (varintDeltaZigZag(-2) != 3) {
            ERR("ZigZag(-2) = %lu, expected 3", varintDeltaZigZag(-2));
        }
        if (varintDeltaZigZag(2) != 4) {
            ERR("ZigZag(2) = %lu, expected 4", varintDeltaZigZag(2));
        }

        /* Test decode */
        if (varintDeltaZigZagDecode(0) != 0) {
            ERR("ZigZagDecode(0) = %ld, expected 0", varintDeltaZigZagDecode(0));
        }
        if (varintDeltaZigZagDecode(1) != -1) {
            ERR("ZigZagDecode(1) = %ld, expected -1", varintDeltaZigZagDecode(1));
        }
        if (varintDeltaZigZagDecode(2) != 1) {
            ERR("ZigZagDecode(2) = %ld, expected 1", varintDeltaZigZagDecode(2));
        }
    }

    TEST("Single delta encode/decode") {
        uint8_t buffer[16];
        int64_t delta = 42;

        size_t encoded = varintDeltaPut(buffer, delta);
        (void)encoded;  /* Intentionally unused in test */
        if (encoded == 0) {
            ERRR("Failed to encode delta");
        }

        int64_t decoded;
        size_t decoded_size = varintDeltaGet(buffer, &decoded);

        if (decoded_size != encoded) {
            ERR("Decoded size %zu != encoded size %zu", decoded_size, encoded);
        }
        if (decoded != delta) {
            ERR("Decoded value %ld != original %ld", decoded, delta);
        }
    }

    TEST("Negative delta encode/decode") {
        uint8_t buffer[16];
        int64_t delta = -123;

        size_t encoded = varintDeltaPut(buffer, delta);
        (void)encoded;  /* Intentionally unused in test */
        int64_t decoded;
        varintDeltaGet(buffer, &decoded);

        if (decoded != delta) {
            ERR("Decoded negative delta %ld != original %ld", decoded, delta);
        }
    }

    TEST("Delta array encode/decode - sorted sequence") {
        int64_t values[] = {100, 105, 110, 115, 120};
        size_t count = 5;
        uint8_t buffer[256];

        size_t encoded = varintDeltaEncode(buffer, values, count);
        (void)encoded;  /* Intentionally unused in test */
        if (encoded == 0) {
            ERRR("Failed to encode delta array");
        }

        int64_t decoded[5];
        size_t decoded_size = varintDeltaDecode(buffer, count, decoded);

        if (decoded_size != encoded) {
            ERR("Decoded size %zu != encoded size %zu", decoded_size, encoded);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%zu] = %ld, expected %ld", i, decoded[i], values[i]);
            }
        }
    }

    TEST("Delta array encode/decode - mixed positive/negative") {
        int64_t values[] = {1000, 1005, 995, 1010, 990};
        size_t count = 5;
        uint8_t buffer[256];

        size_t encoded = varintDeltaEncode(buffer, values, count);
        (void)encoded;  /* Intentionally unused in test */
        int64_t decoded[5];
        varintDeltaDecode(buffer, count, decoded);

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%zu] = %ld, expected %ld", i, decoded[i], values[i]);
            }
        }
    }

    TEST("Delta compression ratio - timestamps") {
        /* Simulate sorted timestamps */
        int64_t timestamps[100];
        int64_t base = 1700000000;
        for (int i = 0; i < 100; i++) {
            timestamps[i] = base + i;  /* Sequential timestamps */
        }

        uint8_t buffer[1024];
        size_t encoded = varintDeltaEncode(buffer, timestamps, 100);

        /* Naive encoding would be 8 bytes * 100 = 800 bytes */
        /* Delta should be much smaller (base + 99 small deltas) */
        if (encoded >= 800) {
            ERR("Delta encoding not efficient: %zu bytes (expected < 800)", encoded);
        }

        /* Verify correctness */
        int64_t decoded[100];
        varintDeltaDecode(buffer, 100, decoded);
        for (int i = 0; i < 100; i++) {
            if (decoded[i] != timestamps[i]) {
                ERR("Timestamp[%d] mismatch", i);
                break;
            }
        }
    }

    TEST("Large delta values") {
        int64_t values[] = {0, 1000000000LL, 2000000000LL};
        size_t count = 3;
        uint8_t buffer[256];

        size_t encoded = varintDeltaEncode(buffer, values, count);
        (void)encoded;  /* Intentionally unused in test */
        int64_t decoded[3];
        varintDeltaDecode(buffer, count, decoded);

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Large value[%zu] = %ld, expected %ld", i, decoded[i], values[i]);
            }
        }
    }

    TEST("Single value array") {
        int64_t value[] = {42};
        uint8_t buffer[16];

        size_t encoded = varintDeltaEncode(buffer, value, 1);
        (void)encoded;  /* Intentionally unused in test */
        int64_t decoded[1];
        varintDeltaDecode(buffer, 1, decoded);

        if (decoded[0] != value[0]) {
            ERR("Single value %ld != expected %ld", decoded[0], value[0]);
        }
    }

    TEST("Zero values") {
        int64_t values[] = {0, 0, 0, 0};
        size_t count = 4;
        uint8_t buffer[64];

        size_t encoded = varintDeltaEncode(buffer, values, count);
        (void)encoded;  /* Intentionally unused in test */
        int64_t decoded[4];
        varintDeltaDecode(buffer, count, decoded);

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != 0) {
                ERR("Zero value[%zu] = %ld, expected 0", i, decoded[i]);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_DELTA_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintDeltaTest(argc, argv);
}
#endif
