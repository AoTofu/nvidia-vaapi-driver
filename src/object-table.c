#include "object-table.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NVD_OBJECT_INDEX_BITS 15U
#define NVD_OBJECT_INDEX_MASK ((1U << NVD_OBJECT_INDEX_BITS) - 1U)
#define NVD_OBJECT_GENERATION_SHIFT NVD_OBJECT_INDEX_BITS
#define NVD_OBJECT_GENERATION_MASK 0xffffU
#define NVD_OBJECT_MAX_CAPACITY (1U << NVD_OBJECT_INDEX_BITS)
#define NVD_OBJECT_INITIAL_CAPACITY 4096U

static uint32_t makeId(uint16_t generation, uint32_t index) {
    return ((uint32_t) generation << NVD_OBJECT_GENERATION_SHIFT) | index;
}

static bool decodeId(uint32_t id, uint16_t *generation, uint32_t *index) {
    *generation = (uint16_t) ((id >> NVD_OBJECT_GENERATION_SHIFT) &
                              NVD_OBJECT_GENERATION_MASK);
    *index = id & NVD_OBJECT_INDEX_MASK;
    return *generation != 0;
}

void nvdObjectTableInit(NVDObjectTable *table) {
    memset(table, 0, sizeof(*table));
    table->freeHead = NVD_OBJECT_SLOT_NONE;
    table->freeTail = NVD_OBJECT_SLOT_NONE;
}

static bool growTable(NVDObjectTable *table) {
    if (table->capacity >= NVD_OBJECT_MAX_CAPACITY) {
        return false;
    }
    uint32_t newCapacity = table->capacity == 0
        ? NVD_OBJECT_INITIAL_CAPACITY : table->capacity * 2U;
    if (newCapacity > NVD_OBJECT_MAX_CAPACITY) {
        newCapacity = NVD_OBJECT_MAX_CAPACITY;
    }
    NVDObjectSlot *newSlots = realloc(table->slots, (size_t) newCapacity * sizeof(*newSlots));
    if (newSlots == NULL) {
        return false;
    }
    table->slots = newSlots;
    memset(&table->slots[table->capacity], 0,
           (size_t) (newCapacity - table->capacity) * sizeof(*table->slots));
    for (uint32_t i = table->capacity; i < newCapacity; i++) {
        table->slots[i].generation = 1;
        table->slots[i].nextFree = i + 1U < newCapacity ? i + 1U : NVD_OBJECT_SLOT_NONE;
    }
    if (table->freeTail != NVD_OBJECT_SLOT_NONE) {
        table->slots[table->freeTail].nextFree = table->capacity;
    } else {
        table->freeHead = table->capacity;
    }
    table->freeTail = newCapacity - 1U;
    table->capacity = newCapacity;
    return true;
}

NVDObject *nvdObjectTableAllocate(NVDObjectTable *table, uint8_t type, size_t payloadSize) {
    if (type >= 7 || payloadSize > SIZE_MAX - sizeof(NVDObject)) {
        return NULL;
    }
    if (table->freeHead == NVD_OBJECT_SLOT_NONE && !growTable(table)) {
        return NULL;
    }

    const uint32_t index = table->freeHead;
    NVDObjectSlot *slot = &table->slots[index];
    table->freeHead = slot->nextFree;
    if (table->freeHead == NVD_OBJECT_SLOT_NONE) {
        table->freeTail = NVD_OBJECT_SLOT_NONE;
    }
    NVDObject *object = calloc(1, sizeof(*object) + payloadSize);
    if (object == NULL) {
        slot->nextFree = NVD_OBJECT_SLOT_NONE;
        if (table->freeTail != NVD_OBJECT_SLOT_NONE) {
            table->slots[table->freeTail].nextFree = index;
        } else {
            table->freeHead = index;
        }
        table->freeTail = index;
        return NULL;
    }
    object->type = type;
    object->id = makeId(slot->generation, index);
    object->obj = payloadSize != 0 ? object->storage : NULL;
    slot->object = object;
    slot->nextFree = NVD_OBJECT_SLOT_NONE;
    table->liveCount++;
    return object;
}

NVDObject *nvdObjectTableGet(const NVDObjectTable *table, uint8_t type, uint32_t id) {
    uint16_t generation;
    uint32_t index;
    if (!decodeId(id, &generation, &index) || index >= table->capacity) {
        return NULL;
    }
    const NVDObjectSlot *slot = &table->slots[index];
    return slot->generation == generation && slot->object != NULL &&
           slot->object->type == type ? slot->object : NULL;
}

NVDObject *nvdObjectTableRemove(NVDObjectTable *table, uint32_t id) {
    uint16_t generation;
    uint32_t index;
    if (!decodeId(id, &generation, &index) || index >= table->capacity) {
        return NULL;
    }
    NVDObjectSlot *slot = &table->slots[index];
    if (slot->generation != generation || slot->object == NULL) {
        return NULL;
    }
    NVDObject *object = slot->object;
    slot->object = NULL;
    slot->generation++;
    if (slot->generation == 0) {
        // Generation zero remains reserved for malformed/uninitialised IDs.
        slot->generation = 1;
    }
    // FIFO reuse provides a practical quarantine window before a slot is used
    // again, while avoiding permanent retirement and unbounded table growth.
    slot->nextFree = NVD_OBJECT_SLOT_NONE;
    if (table->freeTail != NVD_OBJECT_SLOT_NONE) {
        table->slots[table->freeTail].nextFree = index;
    } else {
        table->freeHead = index;
    }
    table->freeTail = index;
    table->liveCount--;
    return object;
}

NVDObject *nvdObjectTableAt(const NVDObjectTable *table, uint32_t slotIndex) {
    return slotIndex < table->capacity ? table->slots[slotIndex].object : NULL;
}

void nvdObjectTableDestroy(NVDObjectTable *table) {
    if (table == NULL) {
        return;
    }
    for (uint32_t i = 0; i < table->capacity; i++) {
        free(table->slots[i].object);
    }
    free(table->slots);
    nvdObjectTableInit(table);
}
