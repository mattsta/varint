#include "varintBitstream.h"
#include "ctest.h"
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/* Bit 'b' of the stream: the stream runs from the top bit of slot 0 */
static uint64_t refBit(const vbits *s, size_t b) {
    return (s[b / BITS_PER_SLOT] >> (BITS_PER_SLOT - 1 - b % BITS_PER_SLOT)) &
           1;
}

/* The 'width' bits at 'off', read one at a time (the reference) */
static uint64_t refGet(const vbits *s, size_t off, size_t width) {
    uint64_t v = 0;
    for (size_t i = 0; i < width; i++) {
        v = (v << 1) | refBit(s, off + i);
    }
    return v;
}

static uint64_t nextRandom(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

int varintBitstreamTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("a width of 0 reads 0 and writes nothing") {
        vbits s[2] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
        const vbits before[2] = {s[0], s[1]};
        for (size_t off = 0; off < 2 * BITS_PER_SLOT; off++) {
            if (varintBitstreamGet(s, off, 0) != 0) {
                ERR("a width-0 read at %zu was not 0", off);
            }
            varintBitstreamSet(s, off, 0, 0);
        }
        if (memcmp(s, before, sizeof(s)) != 0) {
            ERRR("a width-0 write changed the stream");
        }
    }

    TEST("every width at every offset round-trips; other bits keep") {
        uint64_t rng = 24151;
        for (size_t width = 1; width <= 64; width++) {
            const uint64_t mask = width == 64 ? ~0ULL : (1ULL << width) - 1;
            for (size_t off = 0; off + width <= 2 * BITS_PER_SLOT; off++) {
                vbits s[2] = {nextRandom(&rng), nextRandom(&rng)};
                const vbits before[2] = {s[0], s[1]};
                const uint64_t v = nextRandom(&rng) & mask;
                varintBitstreamSet(s, off, width, v);
                const uint64_t got = varintBitstreamGet(s, off, width);
                if (got != v || refGet(s, off, width) != v) {
                    ERR("width %zu at %zu: wrote %016" PRIx64
                        ", read %016" PRIx64 ", bits hold %016" PRIx64,
                        width, off, v, got, refGet(s, off, width));
                }
                for (size_t b = 0; b < 2 * BITS_PER_SLOT; b++) {
                    if ((b < off || b >= off + width) &&
                        refBit(s, b) != refBit(before, b)) {
                        ERR("width %zu at %zu changed bit %zu", width, off, b);
                        break;
                    }
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_BITSTREAM_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintBitstreamTest(argc, argv);
}
#endif
