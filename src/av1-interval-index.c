#include "av1-interval-index.h"

static NVDIntervalNode *node(NVDIntervalIndex *index, uint16_t id) {
    return &((NVDIntervalNode *)index->storage.buf)[id - 1];
}
static unsigned height(NVDIntervalIndex *index, uint16_t id) {
    return id ? node(index, id)->height : 0;
}
static void updateHeight(NVDIntervalIndex *index, uint16_t id) {
    NVDIntervalNode *n = node(index, id);
    unsigned l = height(index, n->left), r = height(index, n->right);
    n->height = 1 + (l > r ? l : r);
}
static uint16_t rotateLeft(NVDIntervalIndex *index, uint16_t root) {
    NVDIntervalNode *n = node(index, root);
    uint16_t next = n->right;
    n->right = node(index, next)->left;
    node(index, next)->left = root;
    updateHeight(index, root);
    updateHeight(index, next);
    return next;
}
static uint16_t rotateRight(NVDIntervalIndex *index, uint16_t root) {
    NVDIntervalNode *n = node(index, root);
    uint16_t next = n->left;
    n->left = node(index, next)->right;
    node(index, next)->right = root;
    updateHeight(index, root);
    updateHeight(index, next);
    return next;
}
static uint16_t insert(NVDIntervalIndex *index, uint16_t root, uint16_t added) {
    if (!root) return added;
    NVDIntervalNode *n = node(index, root);
    uint32_t start = node(index, added)->start;
    if (start < n->start) n->left = insert(index, n->left, added);
    else n->right = insert(index, n->right, added);
    updateHeight(index, root);
    int balance = (int)height(index, n->left) - (int)height(index, n->right);
    if (balance > 1) {
        if (start > node(index, n->left)->start) n->left = rotateLeft(index, n->left);
        return rotateRight(index, root);
    }
    if (balance < -1) {
        if (start < node(index, n->right)->start) n->right = rotateRight(index, n->right);
        return rotateLeft(index, root);
    }
    return root;
}

bool nvdIntervalIndexInsert(NVDIntervalIndex *index, uint32_t start, uint32_t end) {
    if (start >= end || index->count >= NVD_INTERVAL_INDEX_CAPACITY) return false;
    // The usual ascending input cannot overlap any registered range. Arbitrary
    // input orders use the same balanced index without deferring validation.
    if (start < index->maxEnd) {
        uint16_t current = index->root;
        while (current) {
            NVDIntervalNode *n = node(index, current);
            if (end <= n->start) current = n->left;
            else if (start >= n->end) current = n->right;
            else return false;
        }
    }
    const size_t required = ((size_t)index->count + 1) * sizeof(NVDIntervalNode);
    if (!reserveBuffer(&index->storage, required)) return false;
    const uint16_t added = ++index->count;
    *node(index, added) = (NVDIntervalNode){.start = start, .end = end, .height = 1};
    index->storage.size = required;
    index->root = insert(index, index->root, added);
    if (end > index->maxEnd) index->maxEnd = end;
    return true;
}

void nvdIntervalIndexReset(NVDIntervalIndex *index) {
    index->root = index->count = 0;
    index->maxEnd = 0;
    index->storage.size = 0;
    index->storage.failed = false;
}

void nvdIntervalIndexDestroy(NVDIntervalIndex *index) {
    freeAppendableBuffer(&index->storage);
    nvdIntervalIndexReset(index);
}
