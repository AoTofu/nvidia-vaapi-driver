//
// Created by stephen on 12/11/23.
//

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static inline int nvDupFdCloexec(const int fd) {
#ifdef F_DUPFD_CLOEXEC
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
    const int duplicatedFd = dup(fd);
    if (duplicatedFd < 0) {
        return -1;
    }
    if (fcntl(duplicatedFd, F_SETFD, FD_CLOEXEC) < 0) {
        close(duplicatedFd);
        return -1;
    }
    return duplicatedFd;
#endif
}

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

static inline uint32_t nvPlaneCoordinate(const uint32_t value,
                                         const uint32_t subsamplingShift) {
    return subsamplingShift < 32U ? value >> subsamplingShift : 0U;
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
