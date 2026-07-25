#include "object-table.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

int main(void) {
    NVDObjectTable table;
    nvdObjectTableInit(&table);

    NVDObject *first = nvdObjectTableAllocate(&table, 2, sizeof(uint64_t));
    assert(first != NULL && first->obj != NULL);
    *(uint64_t *) first->obj = UINT64_C(0x123456789abcdef0);
    const uint32_t staleId = first->id;
    assert(nvdObjectTableGet(&table, 2, staleId) == first);
    assert(nvdObjectTableGet(&table, 3, staleId) == NULL);
    assert(nvdObjectTableRemove(&table, staleId) == first);
    free(first);
    assert(nvdObjectTableGet(&table, 2, staleId) == NULL);

    NVDObject *second = nvdObjectTableAllocate(&table, 2, sizeof(uint64_t));
    assert(second != NULL);
    assert(second->id != staleId);
    assert(nvdObjectTableGet(&table, 2, second->id) == second);

    // Exercise sustained churn. FIFO slot reuse and generation wrap must not
    // grow the table while only one object is live.
    const uint32_t initialCapacity = table.capacity;
    for (unsigned int i = 0; i < 100000; i++) {
        const uint32_t id = second->id;
        assert(nvdObjectTableRemove(&table, id) == second);
        free(second);
        assert(nvdObjectTableGet(&table, 2, id) == NULL);
        second = nvdObjectTableAllocate(&table, 2, sizeof(uint64_t));
        assert(second != NULL);
        assert(table.capacity == initialCapacity);
    }

    // Force the next free slot through generation wrap and verify it remains
    // reusable rather than being permanently retired.
    assert(table.freeHead != NVD_OBJECT_SLOT_NONE);
    table.slots[table.freeHead].generation = UINT16_MAX;
    NVDObject *wrapping = nvdObjectTableAllocate(&table, 2, 1);
    assert(wrapping != NULL);
    const uint32_t wrappingSlot = wrapping->id & ((1U << 15U) - 1U);
    assert(nvdObjectTableRemove(&table, wrapping->id) == wrapping);
    free(wrapping);
    assert(table.slots[wrappingSlot].generation == 1);

    for (unsigned int i = 0; i < 200; i++) {
        assert(nvdObjectTableAllocate(&table, (uint8_t) (i % 5), 17) != NULL);
    }
    assert(table.liveCount == 201);
    nvdObjectTableDestroy(&table);
    assert(table.liveCount == 0 && table.capacity == 0);
    return 0;
}
