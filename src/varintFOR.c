#include "varintFOR.h"
#include <assert.h>
#include <string.h>

/* Compute optimal offset width for a given range */
varintWidth varintFORComputeWidth(uint64_t range) {
    varintWidth width;
    varintExternalUnsignedEncoding(range, width);
    return width;
}

/* Analyze array to find min, max, range, and optimal width */
void varintFORAnalyze(const uint64_t *values, size_t count,
                      varintFORMetadata *meta) {
    assert(count > 0);
    assert(values != NULL);
    assert(meta != NULL);

    uint64_t minVal = values[0];
    uint64_t maxVal = values[0];

    /* Find min and max in one pass */
    for (size_t i = 1; i < count; i++) {
        if (values[i] < minVal) {
            minVal = values[i];
        }
        if (values[i] > maxVal) {
            maxVal = values[i];
        }
    }

    /* Compute range and optimal offset width */
    uint64_t range = maxVal - minVal;
    varintWidth offsetWidth = varintFORComputeWidth(range);

    /* Fill metadata */
    meta->minValue = minVal;
    meta->maxValue = maxVal;
    meta->range = range;
    meta->offsetWidth = offsetWidth;
    meta->count = count;
    meta->encodedSize = varintFORSize(meta);
}

/* Calculate encoded size: min_value + offset_width + count + (count * offset_width) */
size_t varintFORSize(const varintFORMetadata *meta) {
    /* Use tagged varints for self-describing header */
    varintWidth minWidth = varintTaggedLen(meta->minValue);
    varintWidth countWidth = varintTaggedLen(meta->count);

    /* Header: min_value + offset_width (1 byte) + count + offsets */
    return minWidth + 1 + countWidth + (meta->count * meta->offsetWidth);
}

/* Encode array using Frame-of-Reference */
size_t varintFOREncode(uint8_t *dst, const uint64_t *values, size_t count,
                       varintFORMetadata *meta) {
    assert(dst != NULL);
    assert(values != NULL);
    assert(count > 0);

    /* Local metadata storage - must be at function scope */
    varintFORMetadata localMeta;

    /* Analyze if not already done */
    if (meta == NULL || meta->count != count) {
        varintFORAnalyze(values, count, &localMeta);
        if (meta != NULL) {
            *meta = localMeta;
        }
        meta = &localMeta;
    }

    uint8_t *ptr = dst;

    /* Encode min value using tagged varint (self-describing) */
    varintWidth minWidth = varintTaggedPut64(ptr, meta->minValue);
    ptr += minWidth;

    /* Encode offset width (1 byte) */
    *ptr = (uint8_t)meta->offsetWidth;
    ptr++;

    /* Encode count using tagged varint (self-describing) */
    varintWidth countWidth = varintTaggedPut64(ptr, meta->count);
    ptr += countWidth;

    /* Encode all offsets at fixed width */
    for (size_t i = 0; i < count; i++) {
        uint64_t offset = values[i] - meta->minValue;
        varintExternalPutFixedWidthQuick_(ptr, offset, meta->offsetWidth);
        ptr += meta->offsetWidth;
    }

    return ptr - dst;
}

/* Read metadata from encoded FOR data */
void varintFORReadMetadata(const uint8_t *src, varintFORMetadata *meta) {
    assert(src != NULL);
    assert(meta != NULL);

    const uint8_t *ptr = src;

    /* Decode min value using tagged varint (self-describing) */
    uint64_t minValue;
    varintWidth minWidth = varintTaggedGet64(ptr, &minValue);
    ptr += minWidth;

    /* Decode offset width (1 byte) */
    varintWidth offsetWidth = (varintWidth)(*ptr);
    ptr++;

    /* Decode count using tagged varint (self-describing) */
    uint64_t count;
    varintWidth countWidth = varintTaggedGet64(ptr, &count);

    /* Fill metadata */
    meta->minValue = minValue;
    meta->count = (size_t)count;
    meta->offsetWidth = offsetWidth;

    /* Range and max will be computed if needed */
    meta->range = 0;
    meta->maxValue = minValue;
    meta->encodedSize = minWidth + 1 + countWidth + ((size_t)count * offsetWidth);
}

/* Decode entire FOR-encoded array */
size_t varintFORDecode(const uint8_t *src, uint64_t *values, size_t maxCount) {
    assert(src != NULL);
    assert(values != NULL);

    varintFORMetadata meta;
    varintFORReadMetadata(src, &meta);

    if (meta.count > maxCount) {
        /* Not enough space in output buffer */
        return 0;
    }

    /* Calculate offset to data section using tagged varint lengths */
    varintWidth minWidth = varintTaggedLen(meta.minValue);
    varintWidth countWidth = varintTaggedLen(meta.count);

    const uint8_t *dataPtr = src + minWidth + 1 + countWidth;

    /* Decode all offsets and add back min value */
    for (size_t i = 0; i < meta.count; i++) {
        uint64_t offset;
        varintExternalGetQuick_(dataPtr, meta.offsetWidth, offset);
        values[i] = meta.minValue + offset;
        dataPtr += meta.offsetWidth;
    }

    return meta.count;
}

/* Random access to specific index */
uint64_t varintFORGetAt(const uint8_t *src, size_t index) {
    assert(src != NULL);

    varintFORMetadata meta;
    varintFORReadMetadata(src, &meta);

    assert(index < meta.count);

    /* Calculate offset to requested element using tagged varint lengths */
    varintWidth minWidth = varintTaggedLen(meta.minValue);
    varintWidth countWidth = varintTaggedLen(meta.count);

    const uint8_t *dataPtr = src + minWidth + 1 + countWidth +
                             (index * meta.offsetWidth);

    /* Decode offset and add min value */
    uint64_t offset;
    varintExternalGetQuick_(dataPtr, meta.offsetWidth, offset);
    return meta.minValue + offset;
}

/* Get minimum value from encoded data */
uint64_t varintFORGetMinValue(const uint8_t *src) {
    assert(src != NULL);

    uint64_t minValue;
    varintTaggedGet64(src, &minValue);
    return minValue;
}

/* Get count from encoded data */
size_t varintFORGetCount(const uint8_t *src) {
    assert(src != NULL);

    /* Skip min value (tagged varint) */
    uint64_t minValue;
    varintWidth minWidth = varintTaggedGet64(src, &minValue);

    /* Skip offset width byte */
    const uint8_t *countPtr = src + minWidth + 1;

    /* Decode count (tagged varint) */
    uint64_t count;
    varintTaggedGet64(countPtr, &count);
    return (size_t)count;
}

/* Get offset width from encoded data */
varintWidth varintFORGetOffsetWidth(const uint8_t *src) {
    assert(src != NULL);

    /* Skip min value (tagged varint) to get to offset width byte */
    uint64_t minValue;
    varintWidth minWidth = varintTaggedGet64(src, &minValue);

    return (varintWidth)src[minWidth];
}
