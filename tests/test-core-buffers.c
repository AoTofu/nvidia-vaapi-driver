#include "appendable-buffer.h"
#include "list.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void testAppendAndGrow(void) {
    AppendableBuffer buffer = {0};
    unsigned char expected[4096];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (unsigned char) (i & 0xff);
        assert(appendBuffer(&buffer, &expected[i], 1));
    }

    assert(buffer.size == sizeof(expected));
    assert(buffer.allocated >= buffer.size);
    assert(memcmp(buffer.buf, expected, sizeof(expected)) == 0);
    freeAppendableBuffer(&buffer);
}

static void testAppendOverflowIsNonDestructive(void) {
    AppendableBuffer buffer = {0};
    const char initial[] = "kept";
    assert(appendBuffer(&buffer, initial, sizeof(initial)));

    void *oldBuffer = buffer.buf;
    size_t oldCapacity = buffer.allocated;
    buffer.size = SIZE_MAX;
    assert(!appendBuffer(&buffer, "x", 1));
    assert(buffer.failed);
    assert(buffer.buf == oldBuffer);
    assert(buffer.allocated == oldCapacity);

    // Restore the real allocation size before releasing the test fixture.
    buffer.size = sizeof(initial);
    freeAppendableBuffer(&buffer);
}

static void testArrayGrowthAndExhaustion(void) {
    Array array = {0};
    int values[100];
    for (size_t i = 0; i < 100; i++) {
        values[i] = (int) i;
        assert(add_element(&array, &values[i]));
    }
    assert(array.size == 100);
    assert(*(int *) get_element_at(&array, 99) == 99);
    remove_element_at(&array, 50);
    assert(array.size == 99);
    assert(*(int *) get_element_at(&array, 50) == 51);
    free(array.buf);

    Array exhausted = {.size = UINT32_MAX, .capacity = UINT32_MAX};
    assert(!add_element(&exhausted, &values[0]));
    assert(exhausted.size == UINT32_MAX);
}

int main(void) {
    testAppendAndGrow();
    testAppendOverflowIsNonDestructive();
    testArrayGrowthAndExhaustion();
    return 0;
}
