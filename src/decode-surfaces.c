#include "decode-surfaces.h"

static uint32_t maxU32(uint32_t left, uint32_t right) {
    return left > right ? left : right;
}

uint32_t nvdSelectDecodeSurfaceCount(uint32_t codecDefault,
                                     uint32_t referenceHint,
                                     uint32_t clientTargets,
                                     uint32_t overrideCount,
                                     uint32_t minimum,
                                     uint32_t maximum) {
    if (minimum == 0) {
        minimum = 1;
    }
    if (maximum < minimum) {
        maximum = minimum;
    }

    uint32_t selected = overrideCount;
    if (selected == 0) {
        selected = maxU32(codecDefault, referenceHint);
        selected = maxU32(selected, clientTargets);
    }
    if (selected < minimum) {
        selected = minimum;
    }
    if (selected > maximum) {
        selected = maximum;
    }
    return selected;
}
