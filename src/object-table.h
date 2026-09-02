#ifndef OBJECT_TABLE_H
#define OBJECT_TABLE_H

#include <stddef.h>
#include <stdint.h>

#define NVD_OBJECT_SLOT_NONE UINT32_MAX

typedef struct NVDObject {
    uint32_t id;
    uint8_t type;
    void *obj;
    max_align_t storage[];
} NVDObject;

typedef struct {
    NVDObject *object;
    uint32_t nextFree;
    uint16_t generation;
} NVDObjectSlot;

typedef struct {
    NVDObjectSlot *slots;
    uint32_t capacity;
    uint32_t freeHead;
    uint32_t freeTail;
    uint32_t liveCount;
} NVDObjectTable;

void nvdObjectTableInit(NVDObjectTable *table);
NVDObject *nvdObjectTableAllocate(NVDObjectTable *table, uint8_t type, size_t payloadSize);
NVDObject *nvdObjectTableGet(const NVDObjectTable *table, uint8_t type, uint32_t id);
NVDObject *nvdObjectTableRemove(NVDObjectTable *table, uint32_t id);
NVDObject *nvdObjectTableRemoveTyped(NVDObjectTable *table, uint8_t type, uint32_t id);
NVDObject *nvdObjectTableAt(const NVDObjectTable *table, uint32_t slotIndex);
void nvdObjectTableDestroy(NVDObjectTable *table);

#endif
