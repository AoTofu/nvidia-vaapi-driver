#include "av1-interval-index.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>

static void exhaustivePairs(void) {
    NVDIntervalIndex index = {0};
    for (uint32_t a = 0; a < 8; a++) for (uint32_t b = a + 1; b <= 8; b++)
        for (uint32_t c = 0; c < 8; c++) for (uint32_t d = c + 1; d <= 8; d++) {
            nvdIntervalIndexReset(&index);
            assert(nvdIntervalIndexInsert(&index,a,b));
            assert(nvdIntervalIndexInsert(&index,c,d) == !(a < d && c < b));
        }
    nvdIntervalIndexDestroy(&index);
}
static uint32_t randomState = 123456789;
static uint32_t randomWord(void) {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}
static unsigned validateTree(NVDIntervalIndex *index, uint16_t id, uint32_t *lastEnd, unsigned *seen) {
    if (!id) return 0;
    assert(id <= index->count);
    NVDIntervalNode *n = &((NVDIntervalNode *)index->storage.buf)[id-1];
    unsigned l = validateTree(index,n->left,lastEnd,seen);
    assert(n->start >= *lastEnd && n->start < n->end);
    *lastEnd = n->end;
    (*seen)++;
    unsigned r = validateTree(index,n->right,lastEnd,seen);
    assert(abs((int)l - (int)r) <= 1);
    assert(n->height == 1 + (l > r ? l : r));
    return n->height;
}
static void adversarialOrders(void) {
    NVDIntervalIndex index = {0};
    unsigned order[NVD_INTERVAL_INDEX_CAPACITY];
    for (unsigned kind = 0; kind < 3; kind++) {
        nvdIntervalIndexReset(&index);
        for (unsigned i = 0; i < NVD_INTERVAL_INDEX_CAPACITY; i++) order[i] = i;
        if (kind == 2) for (unsigned i = NVD_INTERVAL_INDEX_CAPACITY-1; i; i--) {
            unsigned j = randomWord() % (i+1), temp = order[i]; order[i] = order[j]; order[j] = temp;
        }
        for (unsigned i = 0; i < NVD_INTERVAL_INDEX_CAPACITY; i++) {
            unsigned start = 2 * (kind == 1 ? NVD_INTERVAL_INDEX_CAPACITY-1-i : order[i]);
            assert(nvdIntervalIndexInsert(&index,start,start+1));
        }
        uint32_t lastEnd = 0; unsigned seen = 0;
        assert(validateTree(&index,index.root,&lastEnd,&seen) <= 14);
        assert(seen == NVD_INTERVAL_INDEX_CAPACITY);
        assert(!nvdIntervalIndexInsert(&index,9000,9001));
    }
    nvdIntervalIndexDestroy(&index);
}
static void differentialFuzz(void) {
    NVDIntervalIndex index = {0};
    uint32_t starts[512], ends[512];
    for (unsigned run = 0; run < 1000; run++) {
        nvdIntervalIndexReset(&index);
        unsigned accepted = 0;
        for (unsigned trial = 0; trial < 512; trial++) {
            uint32_t start = randomWord() % 4096, end = start + randomWord() % 64;
            bool expected = start < end;
            for (unsigned i = 0; i < accepted; i++) if (start < ends[i] && starts[i] < end) expected = false;
            assert(nvdIntervalIndexInsert(&index,start,end) == expected);
            if (expected) { starts[accepted] = start; ends[accepted++] = end; }
        }
        uint32_t lastEnd = 0; unsigned seen = 0;
        validateTree(&index,index.root,&lastEnd,&seen);
        assert(seen == accepted);
    }
    nvdIntervalIndexReset(&index);
    assert(nvdIntervalIndexInsert(&index,UINT32_MAX-2,UINT32_MAX-1));
    assert(nvdIntervalIndexInsert(&index,UINT32_MAX-1,UINT32_MAX));
    assert(!nvdIntervalIndexInsert(&index,UINT32_MAX-2,UINT32_MAX));
    assert(!nvdIntervalIndexInsert(&index,UINT32_MAX,UINT32_MAX));
    nvdIntervalIndexDestroy(&index);
}
int main(void) { exhaustivePairs(); adversarialOrders(); differentialFuzz(); return 0; }
