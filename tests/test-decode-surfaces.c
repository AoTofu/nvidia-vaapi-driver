#include "decode-surfaces.h"

#include <assert.h>

int main(void) {
    assert(nvdSelectDecodeSurfaceCount(10, 0, 0, 0, 2, 32) == 10);
    assert(nvdSelectDecodeSurfaceCount(10, 18, 12, 0, 2, 32) == 18);
    assert(nvdSelectDecodeSurfaceCount(10, 8, 14, 0, 2, 32) == 14);
    assert(nvdSelectDecodeSurfaceCount(20, 20, 20, 6, 2, 32) == 6);
    assert(nvdSelectDecodeSurfaceCount(2, 0, 0, 0, 6, 16) == 6);
    assert(nvdSelectDecodeSurfaceCount(32, 32, 32, 0, 2, 18) == 18);
    assert(nvdSelectDecodeSurfaceCount(0, 0, 0, 0, 0, 0) == 1);
    return 0;
}
