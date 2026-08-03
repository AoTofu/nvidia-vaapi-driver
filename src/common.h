//
// Created by stephen on 12/11/23.
//

#ifndef COMMON_H
#define COMMON_H

static inline uint32_t nvPlaneExtent(const uint32_t value,
                                     const uint32_t subsamplingShift) {
    if (subsamplingShift == 0) {
        return value;
    }
    if (subsamplingShift >= 32) {
        return value != 0;
    }
    const uint32_t mask = (1U << subsamplingShift) - 1U;
    return (value >> subsamplingShift) + ((value & mask) != 0U);
}

typedef struct
{
    uint32_t x;
    uint32_t y;
} NVSubSampling;

typedef struct
{
    uint32_t channelCount;
    uint32_t fourcc;
    NVSubSampling ss; // subsampling
} NVFormatPlane;

#endif //COMMON_H
