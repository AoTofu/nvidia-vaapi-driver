#include "appendable-buffer.h"

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool reserveBuffer(AppendableBuffer *buffer, size_t required) {
    if (required <= buffer->allocated) {
        return true;
    }

    size_t capacity = buffer->allocated != 0 ? buffer->allocated : 256;
    while (capacity < required) {
        size_t growth = capacity >> 1;
        if (growth == 0 || capacity > SIZE_MAX - growth) {
            capacity = required;
            break;
        }
        capacity += growth;
    }

    void *newBuffer = memalign(16, capacity);
    if (newBuffer == NULL) {
        buffer->failed = true;
        return false;
    }
    if (buffer->buf != NULL && buffer->size != 0) {
        memcpy(newBuffer, buffer->buf, buffer->size);
    }

    void *oldBuffer = buffer->buf;
    buffer->buf = newBuffer;
    buffer->allocated = capacity;
    free(oldBuffer);
    return true;
}

bool reserveAdditionalBuffer(AppendableBuffer *buffer, size_t additional) {
    if (buffer->size > SIZE_MAX - additional) {
        buffer->failed = true;
        return false;
    }
    return reserveBuffer(buffer, buffer->size + additional);
}

bool reserveBufferElements(AppendableBuffer *buffer, size_t elements, size_t elementSize) {
    if (elements != 0 && elementSize > SIZE_MAX / elements) {
        buffer->failed = true;
        return false;
    }
    return reserveAdditionalBuffer(buffer, elements * elementSize);
}

bool appendBuffer(AppendableBuffer *buffer, const void *data, size_t size) {
    if (size == 0) {
        return true;
    }
    if (data == NULL || buffer->size > SIZE_MAX - size) {
        buffer->failed = true;
        return false;
    }

    size_t required = buffer->size + size;
    if (!reserveBuffer(buffer, required)) {
        return false;
    }

    memcpy((unsigned char *) buffer->buf + buffer->size, data, size);
    buffer->size = required;
    return true;
}

void freeAppendableBuffer(AppendableBuffer *buffer) {
    free(buffer->buf);
    buffer->buf = NULL;
    buffer->size = 0;
    buffer->allocated = 0;
    buffer->failed = false;
}
