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
