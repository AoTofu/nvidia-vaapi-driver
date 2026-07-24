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

    // Exhaust the eight-bit generation for this slot. It must be retired
    // instead of ever making the original stale ID valid again.
    const uint32_t reusedSlot = second->id & ((1U << 20U) - 1U);
    for (unsigned int i = 0; i < 254; i++) {
        const uint32_t id = second->id;
        assert(nvdObjectTableRemove(&table, id) == second);
        free(second);
        assert(nvdObjectTableGet(&table, 2, id) == NULL);
        assert(nvdObjectTableGet(&table, 2, staleId) == NULL);
        second = nvdObjectTableAllocate(&table, 2, sizeof(uint64_t));
        assert(second != NULL);
    }
    assert((second->id & ((1U << 20U) - 1U)) != reusedSlot);

    for (unsigned int i = 0; i < 200; i++) {
        assert(nvdObjectTableAllocate(&table, (uint8_t) (i % 5), 17) != NULL);
    }
    assert(table.liveCount == 201);
    nvdObjectTableDestroy(&table);
    assert(table.liveCount == 0 && table.capacity == 0);
    return 0;
}
