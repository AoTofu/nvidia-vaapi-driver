#ifndef AV1_INTERVAL_INDEX_H
#define AV1_INTERVAL_INDEX_H

#include <stdint.h>
#include "appendable-buffer.h"

#define NVD_INTERVAL_INDEX_CAPACITY 4096U

typedef struct {
    uint32_t start, end;
    uint16_t left, right;
    uint8_t height;
} NVDIntervalNode;

typedef struct {
    AppendableBuffer storage;
    uint32_t maxEnd;
    uint16_t root, count;
} NVDIntervalIndex;

// Rejects empty/overlapping intervals immediately. An AVL tree bounds lookup
// and insertion to O(log T) for every order. Node IDs are one-based; zero is nil.
// A storage allocation failure sets storage.failed; other rejections do not.
bool nvdIntervalIndexInsert(NVDIntervalIndex *index, uint32_t start, uint32_t end);
void nvdIntervalIndexReset(NVDIntervalIndex *index);
void nvdIntervalIndexDestroy(NVDIntervalIndex *index);

#endif
