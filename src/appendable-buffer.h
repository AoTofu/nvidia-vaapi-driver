#ifndef APPENDABLE_BUFFER_H
#define APPENDABLE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void *buf;
    size_t size;
    size_t allocated;
    bool failed;
} AppendableBuffer;

bool reserveBuffer(AppendableBuffer *buffer, size_t required);
bool reserveAdditionalBuffer(AppendableBuffer *buffer, size_t additional);
bool reserveBufferElements(AppendableBuffer *buffer, size_t elements, size_t elementSize);
bool appendBuffer(AppendableBuffer *buffer, const void *data, size_t size);
bool trimBuffer(AppendableBuffer *buffer, size_t targetCapacity);
void freeAppendableBuffer(AppendableBuffer *buffer);

#endif
