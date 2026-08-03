#include "vabackend.h"

bool nvValidateSliceRange(NVContext *ctx, const NVBuffer *buffer,
                          uint32_t offset, size_t size, const void **data) {
    if (ctx == NULL || buffer == NULL || buffer->ptr == NULL ||
        (size_t) offset > buffer->size || size > buffer->size - (size_t) offset) {
        if (ctx != NULL) {
            ctx->inputValidationFailed = true;
        }
        if (data != NULL) {
            *data = NULL;
        }
        return false;
    }

    if (data != NULL) {
        *data = PTROFF(buffer->ptr, offset);
    }
    return true;
}

bool nvValidateBufferSchema(const NVBuffer *buffer,
                            const BufferSchema *schema) {
    if (buffer == NULL || schema == NULL || schema->minElementSize == 0 ||
        buffer->elements < schema->minElements ||
        (schema->maxElements != 0 &&
         buffer->elements > schema->maxElements) ||
        buffer->elementSize < schema->minElementSize ||
        (buffer->elements != 0 &&
         buffer->elementSize > SIZE_MAX / buffer->elements) ||
        buffer->size != buffer->elementSize * buffer->elements ||
        (buffer->size != 0 && buffer->ptr == NULL)) {
        return false;
    }
    return true;
}

int nvdSelectPictureIndex(const uint32_t allocatedCount,
                          const uint32_t surfaceLimit,
                          const uint32_t activeMask,
                          const uint32_t busyMask,
                          const uint32_t recycleCursor) {
    if (surfaceLimit == 0 || surfaceLimit > NVD_MAX_DECODE_SURFACES) {
        return -1;
    }
    if (allocatedCount < surfaceLimit) {
        return (int) allocatedCount;
    }

    const uint32_t limitMask = surfaceLimit == 32U
        ? UINT32_MAX
        : (1U << surfaceLimit) - 1U;
    const uint32_t available = limitMask & ~(activeMask | busyMask);
    if (available == 0) {
        return -1;
    }

    const uint32_t start = recycleCursor % surfaceLimit;
    for (uint32_t offset = 0; offset < surfaceLimit; offset++) {
        const uint32_t index = (start + offset) % surfaceLimit;
        if ((available & (1U << index)) != 0) {
            return (int) index;
        }
    }
    return -1;
}

size_t nvBuildVP8FrameHeader(uint8_t header[10], const bool keyFrame,
                             const uint8_t version, const bool showFrame,
                             const uint32_t firstPartitionSize,
                             const uint16_t width, const uint16_t height) {
    if (header == NULL || version > 7U || firstPartitionSize > 0x7ffffU ||
        (keyFrame && (width > 0x3fffU || height > 0x3fffU))) {
        return 0;
    }

    const uint32_t frameTag = (keyFrame ? 0U : 1U) |
                              ((uint32_t) version << 1U) |
                              ((uint32_t) showFrame << 4U) |
                              (firstPartitionSize << 5U);
    header[0] = (uint8_t) frameTag;
    header[1] = (uint8_t) (frameTag >> 8U);
    header[2] = (uint8_t) (frameTag >> 16U);
    if (!keyFrame) {
        return 3U;
    }

    header[3] = 0x9d;
    header[4] = 0x01;
    header[5] = 0x2a;
    header[6] = (uint8_t) width;
    header[7] = (uint8_t) (width >> 8U);
    header[8] = (uint8_t) height;
    header[9] = (uint8_t) (height >> 8U);
    return 10U;
}
