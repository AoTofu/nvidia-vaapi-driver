#ifndef BACKENDCOMMON_H
#define BACKENDCOMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <ffnvcodec/dynlink_loader.h>

struct _BackingImage;
struct _NVDriver;

bool checkModesetParameterFromFd(int fd);
bool isNvidiaDrmFd(int fd, bool log);
bool nvBackingImageHasExternalHostMemory(const struct _BackingImage *img);
bool nvBackingImageHasExternalDeviceMemory(const struct _BackingImage *img);
uint8_t *nvBackingImageHostPlane(const struct _BackingImage *img, uint32_t plane);
CUdeviceptr nvBackingImageDevicePlane(const struct _BackingImage *img, uint32_t plane);
bool nvSyncBackingImageHostAccess(struct _BackingImage *img, uint64_t flags);
void nvDestroyImportedBackingImage(struct _NVDriver *drv, struct _BackingImage *img);

#endif // BACKENDCOMMON_H
