/* ====================================================================
 * example_record.c — Schema-Driven Columnar Record Compression
 * ====================================================================
 * Demonstrates varintRecord end to end:
 *   1. Describe a C struct's layout with a field-descriptor schema
 *   2. Encode an array of records — each field becomes a column and
 *      competes across the whole codec family automatically
 *   3. Inspect the per-column results (which fields cost what)
 *   4. Decode with ZERO external metadata — the schema is in the stream
 *   5. Compare against row-oriented encoding to see why columns win
 *
 * Build:
 *   cc -O2 -I../../src example_record.c <varint sources> -o example_record
 * (or use the CMake target: make example_record)
 */

#include "varintDD.h"
#include "varintRecord.h"
#include "varintTagged.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------
 * The application's record type: an IoT sensor reading as it might
 * arrive from a device fleet. Note the mix: a regular-cadence
 * timestamp, a small device population, an enum, a signed jittery
 * measurement, a raw MAC tag, and struct padding the schema ignores.
 * -------------------------------------------------------------------- */
typedef struct SensorReading {
    uint64_t timestampMs; /* arrives every ~250ms — stride/delta gold */
    uint32_t deviceId;    /* 40 devices — dictionary/palette gold */
    int16_t temperature;  /* tenths of a degree, drifts slowly, signed */
    uint8_t status;       /* enum {OK, WARN, FAIL} — palette gold */
    uint8_t mac[6];       /* structured hardware tag — byte-plane gold */
    float voltage;        /* smooth analog sample — XOR/FLOAT lanes */
    varintDD precise;     /* 106-bit accumulated dose — DD_STREAM lane */
    uint8_t pad_[4];      /* struct padding: not described, not stored */
} SensorReading;

/* The schema: one line per field, layout captured at compile time by
 * offsetof/sizeof. This is the whole "data description language". */
static const varintRecordField sensorSchema[] = {
    VARINT_RECORD_FIELD(SensorReading, timestampMs, VARINT_RECORD_U64),
    VARINT_RECORD_FIELD(SensorReading, deviceId, VARINT_RECORD_U32),
    VARINT_RECORD_FIELD(SensorReading, temperature, VARINT_RECORD_I16),
    VARINT_RECORD_FIELD(SensorReading, status, VARINT_RECORD_U8),
    {offsetof(SensorReading, mac), 6, VARINT_RECORD_BYTES, 0},
    VARINT_RECORD_FIELD(SensorReading, voltage, VARINT_RECORD_F32),
    VARINT_RECORD_FIELD(SensorReading, precise, VARINT_RECORD_DD),
};
static const char *sensorFieldNames[] = {"timestampMs", "deviceId",
                                         "temperature", "status",
                                         "mac",         "voltage",
                                         "precise"};
#define SENSOR_FIELDS (sizeof(sensorSchema) / sizeof(sensorSchema[0]))

static uint64_t rngState = UINT64_C(0x9E3779B97F4A7C15);
static uint64_t rng(void) {
    rngState = rngState * UINT64_C(6364136223846793005) +
               UINT64_C(1442695040888963407);
    return rngState >> 16;
}

static void fillReadings(SensorReading *rows, size_t count) {
    int16_t temp = 215; /* 21.5 C */
    for (size_t i = 0; i < count; i++) {
        rows[i].timestampMs = UINT64_C(1700000000000) + i * 250;
        rows[i].deviceId = 9000 + (uint32_t)(rng() % 40);
        temp = (int16_t)(temp + (int16_t)(rng() % 5) - 2);
        rows[i].temperature = temp;
        rows[i].status = (rng() % 50 == 0) ? 2 : (rng() % 10 == 0) ? 1 : 0;
        /* Vendor OUI prefix is fleet-constant; the low bytes derive
         * from the device population. */
        rows[i].mac[0] = 0xA4;
        rows[i].mac[1] = 0x83;
        rows[i].mac[2] = 0xE7;
        rows[i].mac[3] = 0x10;
        rows[i].mac[4] = (uint8_t)(rows[i].deviceId >> 5);
        rows[i].mac[5] = (uint8_t)(0xA0 + (rows[i].deviceId % 40));
        rows[i].voltage = 3.30f + (float)((int64_t)(rng() % 9) - 4) * 0.005f;
        rows[i].precise.hi = 0.125 * (double)(rng() % 100000);
        rows[i].precise.lo = 0.0;
        memset(rows[i].pad_, 0, sizeof(rows[i].pad_));
    }
}

/* Row-oriented baseline: every field of every record tagged in place —
 * the natural "just varint everything" approach varintRecord replaces. */
static size_t encodeRowOriented(uint8_t *dst, const SensorReading *rows,
                                size_t count) {
    uint8_t *p = dst;
    for (size_t i = 0; i < count; i++) {
        p += varintTaggedPut64(p, rows[i].timestampMs);
        p += varintTaggedPut64(p, rows[i].deviceId);
        p += varintTaggedPut64(p, (uint64_t)(int64_t)rows[i].temperature);
        p += varintTaggedPut64(p, rows[i].status);
        memcpy(p, rows[i].mac, 6);
        p += 6;
        memcpy(p, &rows[i].voltage, 4);
        p += 4;
        memcpy(p, &rows[i].precise, 16);
        p += 16;
    }
    return (size_t)(p - dst);
}

int main(void) {
    printf("===========================================\n");
    printf("   varintRecord Example: Sensor Fleet\n");
    printf("===========================================\n\n");

    enum { N = 200000 };
    SensorReading *rows = calloc(N, sizeof(*rows));
    fillReadings(rows, N);
    const size_t rawBytes = N * sizeof(SensorReading);
    printf("Dataset: %d readings x %zu bytes = %.2f MB raw\n\n", N,
           sizeof(SensorReading), (double)rawBytes / (1024.0 * 1024.0));

    /* 1. Encode: one call, schema in, compressed stream out. */
    uint8_t *enc = malloc(varintRecordMaxEncodedSize(
        N, sizeof(SensorReading), sensorSchema, SENSOR_FIELDS));
    varintRecordMeta meta;
    const size_t written =
        varintRecordEncode(enc, rows, N, sizeof(SensorReading), sensorSchema,
                           SENSOR_FIELDS, 0, &meta);
    if (written == 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }

    printf("1. Columnar encode: %zu bytes (%.2fx compression)\n", written,
           (double)rawBytes / (double)written);

    /* 2. Per-column breakdown: winning strategy + cost per field. */
    printf("\n2. Per-column results (each field ran every strategy its\n"
           "   kind supports; the smallest verified result won):\n");
    for (size_t f = 0; f < SENSOR_FIELDS; f++) {
        const size_t colRaw = N * sensorSchema[f].size;
        printf("   %-12s %-5s %9zu B raw -> %8zu B (%8.2fx) via %s\n",
               sensorFieldNames[f],
               varintRecordKindName(sensorSchema[f].kind), colRaw,
               meta.columnBytes[f],
               (double)colRaw / (double)meta.columnBytes[f],
               varintRecordStrategyName(
                   (varintRecordStrategy)meta.columnStrategy[f]));
    }

    /* 3. Self-description: size and decode with no external metadata. */
    varintRecordHeader hdr;
    if (varintRecordReadHeader(enc, written, &hdr) == 0) {
        fprintf(stderr, "header parse failed\n");
        return 1;
    }
    printf("\n3. Stream is self-describing: header says %" PRIu64
           " records x %" PRIu64 " bytes, %" PRIu64 " fields\n",
           hdr.recordCount, hdr.recordSize, hdr.fieldCount);

    SensorReading *dec =
        malloc((size_t)hdr.recordCount * (size_t)hdr.recordSize);
    size_t decodedCount = 0;
    const size_t read = varintRecordDecode(
        enc, written, dec, (size_t)hdr.recordCount, sizeof(SensorReading),
        &decodedCount);
    if (read != written || decodedCount != N ||
        memcmp(dec, rows, rawBytes) != 0) {
        fprintf(stderr, "decode round trip failed\n");
        return 1;
    }
    printf("   Decoded %zu records, byte-identical to the originals ✓\n",
           decodedCount);

    /* 4. Why columns beat rows. */
    uint8_t *rowEnc = malloc(rawBytes * 2);
    const size_t rowBytes = encodeRowOriented(rowEnc, rows, N);
    printf("\n4. Row-oriented tagged baseline: %zu bytes (%.2fx)\n", rowBytes,
           (double)rawBytes / (double)rowBytes);
    printf("   Columnar advantage: %.2fx smaller than row-oriented\n",
           (double)rowBytes / (double)written);
    printf("   (Columns expose the structure — stride timestamps, a\n"
           "   40-device dictionary, near-constant status — that\n"
           "   per-record varints cannot see.)\n");

    free(rows);
    free(enc);
    free(dec);
    free(rowEnc);
    printf("\n✅ All operations completed successfully!\n");
    return 0;
}
