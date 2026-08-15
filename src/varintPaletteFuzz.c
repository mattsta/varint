/* ====================================================================
 * varintPaletteFuzz — self-contained deterministic fuzzer
 * ====================================================================
 * No external fuzzing tooling: a plain executable driving three rotated
 * strategies from one LCG, so every failure reproduces from its seed.
 *
 *   1. Structured round-trip generation (self-checking oracle):
 *      random count/cardinality/skew/outlier parameters chosen to hit
 *      every format section (tail blocks, all-verbatim, full palette,
 *      1-symbol palette, extreme values) → encode → decode → compare.
 *   2. Mutation of valid encodings: header-biased byte flips and
 *      truncations; decode must never crash, and a successful decode
 *      must return exactly the mutated header's own count claim.
 *   3. Pure-random decode: arbitrary bytes are rejected safely.
 *
 * The same value streams are also pushed through the varintCompete
 * frame (encode/decode round-trip) on a share of iterations.
 *
 * Usage: varintPaletteFuzz [iterations] [seed]
 *   defaults: 20000 iterations, seed 0x5EED (sized for ctest).
 * On failure prints the exact reproduce command and exits nonzero.
 * Run under ASan/UBSan via scripts/test/run_palette_fuzz.sh for the
 * long sessions. */

#include "varintCompete.h"
#include "varintPalette.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_VALUES 4096

static uint64_t rngState_;
static uint64_t rng_(void) {
    rngState_ = rngState_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return rngState_ >> 16;
}

static uint64_t gIterations;
static uint64_t gSeed;
static uint64_t gIter;

static void fail_(const char *what, const char *detail) {
    fprintf(stderr,
            "FUZZ FAILURE: %s (%s)\n"
            "  iteration : %" PRIu64 "\n"
            "  reproduce : varintPaletteFuzz %" PRIu64 " %" PRIu64 "\n",
            what, detail, gIter, gIterations, gSeed);
    exit(1);
}

#define FUZZ_CHECK(cond, what, detail)                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fail_((what), (detail));                                           \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Structured generation
 * ==================================================================== */

static size_t genCount_(void) {
    /* Bias toward block boundaries and small counts; occasionally big. */
    static const size_t edges[] = {1, 2, 63, 64, 65, 127, 128, 129, 4096};
    const uint64_t pick = rng_() % 10;
    if (pick < 3) {
        return edges[rng_() % (sizeof(edges) / sizeof(edges[0]))];
    }
    if (pick < 9) {
        return 1 + (size_t)(rng_() % 512);
    }
    return 1 + (size_t)(rng_() % FUZZ_MAX_VALUES);
}

static size_t genValues_(uint64_t *values) {
    const size_t count = genCount_();
    const uint32_t style = (uint32_t)(rng_() % 5);
    uint32_t card = 1 + (uint32_t)(rng_() % 48);
    if (rng_() % 4 == 0) {
        /* Bias to the palette-capacity boundary. */
        card = 15 + (uint32_t)(rng_() % 3);
    }

    uint64_t alpha[48];
    const uint64_t base = rng_();
    for (uint32_t k = 0; k < card; k++) {
        alpha[k] = base + (uint64_t)k * 977;
    }
    if (rng_() & 1) {
        alpha[0] = 0;
    }
    if (card >= 2 && (rng_() & 1)) {
        alpha[1] = UINT64_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        switch (style) {
        case 0: /* constant */
            values[i] = alpha[0];
            break;
        case 1: /* uniform over alphabet */
            values[i] = alpha[rng_() % card];
            break;
        case 2: { /* geometric skew */
            uint64_t r = rng_();
            uint32_t sym = 0;
            while (sym + 1 < card && (r & 1)) {
                sym++;
                r >>= 1;
            }
            values[i] = alpha[sym];
            break;
        }
        case 3: /* all-unique */
            values[i] = (rng_() << 20) ^ i;
            break;
        default: /* hot alphabet + rare unique outliers */
            values[i] =
                (rng_() % 64 == 0) ? (rng_() | (1ULL << 60)) : alpha[i % card];
            break;
        }
    }
    return count;
}

/* ====================================================================
 * Strategies
 * ==================================================================== */

static void strategyRoundTrip_(uint64_t *values, size_t count, uint8_t *enc,
                               uint64_t *dec) {
    varintPaletteMeta analyzed;
    varintPaletteAnalyze(values, count, &analyzed);

    const size_t written = varintPaletteEncode(enc, values, count, NULL);
    FUZZ_CHECK(written > 0, "encode returned 0", "round-trip");
    FUZZ_CHECK(written <= varintPaletteMaxSize(count),
               "encode exceeded MaxSize", "round-trip");
    FUZZ_CHECK(analyzed.encodedSize == written, "Analyze != Encode size",
               "round-trip");

    const size_t got = varintPaletteDecode(enc, written, dec, count);
    FUZZ_CHECK(got == count, "decode count mismatch", "round-trip");
    FUZZ_CHECK(memcmp(dec, values, count * sizeof(*dec)) == 0,
               "decode value mismatch", "round-trip");
}

static void strategyMutate_(const uint8_t *pristine, size_t written,
                            uint8_t *scratch, uint64_t *dec) {
    memcpy(scratch, pristine, written);

    /* Sometimes also truncate. */
    size_t len = written;
    if (rng_() % 4 == 0) {
        len = rng_() % written;
    }

    const int flips = 1 + (int)(rng_() % 8);
    if (len > 0) {
        for (int f = 0; f < flips; f++) {
            const size_t bound = (rng_() & 1) && len > 48 ? 48 : len;
            scratch[rng_() % bound] ^= (uint8_t)(rng_() | 1);
        }
    }

    const size_t got = varintPaletteDecode(scratch, len, dec, FUZZ_MAX_VALUES);
    if (got != 0) {
        const size_t claimed = varintPaletteGetCount(scratch, len);
        FUZZ_CHECK(got == claimed, "decode ignored mutated count claim",
                   "mutation");
    }
}

static void strategyRandom_(uint8_t *scratch, uint64_t *dec) {
    const size_t len = 1 + (size_t)(rng_() % 600);
    for (size_t i = 0; i < len; i++) {
        scratch[i] = (uint8_t)rng_();
    }
    (void)varintPaletteDecode(scratch, len, dec, FUZZ_MAX_VALUES);
}

static void strategyCompete_(uint64_t *values, size_t count, uint8_t *enc,
                             uint64_t *dec) {
    const size_t written = varintCompeteEncodeUnsigned(
        enc, values, count, VARINT_COMPETE_DEFAULT_MASK, NULL);
    FUZZ_CHECK(written > 0, "compete encode returned 0", "compete");

    const size_t read = varintCompeteDecodeUnsigned(enc, written, count, dec);

    varintCompeteHeader h;
    const char *winner = "?";
    if (varintCompeteReadHeader(enc, written, &h) != 0) {
        winner = varintCodecName(h.codecID);
    }
    FUZZ_CHECK(read == written, "compete decode byte mismatch", winner);
    if (memcmp(dec, values, count * sizeof(*dec)) != 0) {
        size_t at = 0;
        while (at < count && dec[at] == values[at]) {
            at++;
        }
        fprintf(stderr,
                "compete mismatch: winner=%s count=%zu first-diff@%zu "
                "want=%" PRIu64 " got=%" PRIu64 "\n",
                winner, count, at, values[at], dec[at]);
        fail_("compete decode value mismatch", winner);
    }
}

int main(int argc, char *argv[]) {
    gIterations = (argc > 1) ? strtoull(argv[1], NULL, 0) : 20000;
    gSeed = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x5EED;
    rngState_ = gSeed | 1;

    uint64_t *values = malloc(FUZZ_MAX_VALUES * sizeof(*values));
    uint64_t *dec = malloc(FUZZ_MAX_VALUES * sizeof(*dec));
    uint8_t *enc = malloc(varintCompeteMaxEncodedSize(FUZZ_MAX_VALUES));
    uint8_t *scratch = malloc(varintCompeteMaxEncodedSize(FUZZ_MAX_VALUES));
    if (!values || !dec || !enc || !scratch) {
        fprintf(stderr, "fuzz: allocation failed\n");
        return 2;
    }

    for (gIter = 0; gIter < gIterations; gIter++) {
        const size_t count = genValues_(values);

        /* Every iteration exercises the oracle... */
        strategyRoundTrip_(values, count, enc, dec);
        const size_t written = varintPaletteEncode(enc, values, count, NULL);

        /* ...then rotates through the adversarial strategies. */
        switch (gIter % 4) {
        case 0:
        case 1:
            strategyMutate_(enc, written, scratch, dec);
            break;
        case 2:
            strategyRandom_(scratch, dec);
            break;
        default:
            strategyCompete_(values, count, scratch, dec);
            break;
        }
    }

    printf("varintPaletteFuzz: %" PRIu64 " iterations clean (seed %" PRIu64
           ")\n",
           gIterations, gSeed);
    free(values);
    free(dec);
    free(enc);
    free(scratch);
    return 0;
}
