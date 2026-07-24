#ifndef DECODE_SURFACES_H
#define DECODE_SURFACES_H

#include <stdint.h>

uint32_t nvdSelectDecodeSurfaceCount(uint32_t codecDefault,
                                     uint32_t referenceHint,
                                     uint32_t clientTargets,
                                     uint32_t overrideCount,
                                     uint32_t minimum,
                                     uint32_t maximum);

#endif
