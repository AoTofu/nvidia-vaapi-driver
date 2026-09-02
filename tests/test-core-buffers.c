#include "appendable-buffer.h"
#include "common.h"
#include "list.h"

#include <assert.h>
#include <fcntl.h>
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

    AppendableBuffer reserve = {.size = 2};
    assert(!reserveAdditionalBuffer(&reserve, SIZE_MAX));
    assert(reserve.failed);
    reserve.failed = false;
    reserve.size = 0;
    assert(!reserveBufferElements(&reserve, SIZE_MAX, 2));
    assert(reserve.failed);
}

static void testTrimPreservesData(void) {
    AppendableBuffer buffer = {0};
    unsigned char expected[4096];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (unsigned char) (i * 17U);
    }
    assert(appendBuffer(&buffer, expected, sizeof(expected)));
    assert(reserveBuffer(&buffer, 16U * 1024U * 1024U));
    assert(buffer.allocated >= 16U * 1024U * 1024U);

    assert(trimBuffer(&buffer, 8192));
    assert(buffer.allocated == 8192);
    assert(buffer.size == sizeof(expected));
    assert(memcmp(buffer.buf, expected, sizeof(expected)) == 0);

    // A target smaller than live data is raised to the live size.
    assert(trimBuffer(&buffer, 1));
    assert(buffer.allocated == sizeof(expected));
    assert(memcmp(buffer.buf, expected, sizeof(expected)) == 0);

    // Empty buffers can release all retained capacity.
    buffer.size = 0;
    assert(trimBuffer(&buffer, 0));
    assert(buffer.buf == NULL);
    assert(buffer.allocated == 0);
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

static void testDuplicatedFdIsCloseOnExec(void) {
    int pipeFds[2];
    assert(pipe(pipeFds) == 0);

    const int duplicatedFd = nvDupFdCloexec(pipeFds[0]);
    assert(duplicatedFd >= 0);
    assert((fcntl(duplicatedFd, F_GETFD) & FD_CLOEXEC) != 0);

    close(duplicatedFd);
    close(pipeFds[0]);
    close(pipeFds[1]);
}

static void testPlaneGeometryHelpers(void) {
    assert(nvPlaneExtent(7, 1) == 4);
    assert(nvPlaneExtent(8, 1) == 4);
    assert(nvPlaneExtent(UINT32_MAX, 32) == 1);
    assert(nvPlaneCoordinate(7, 1) == 3);
    assert(nvPlaneCoordinate(8, 1) == 4);
    assert(nvPlaneCoordinate(UINT32_MAX, 32) == 0);
}

int main(void) {
    testAppendAndGrow();
    testAppendOverflowIsNonDestructive();
    testTrimPreservesData();
    testArrayGrowthAndExhaustion();
    testDuplicatedFdIsCloseOnExec();
    testPlaneGeometryHelpers();
    return 0;
}
