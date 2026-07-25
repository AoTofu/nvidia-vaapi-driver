#include "vabackend.h"
#include "backend-common.h"
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/mman.h>
#include <unistd.h>

bool checkModesetParameterFromFd(int fd) {
    if (fd > 0) {
        //this ioctl should fail if modeset=0
        struct drm_get_cap caps = { .capability = DRM_CAP_DUMB_BUFFER };
        int ret = ioctl(fd, DRM_IOCTL_GET_CAP, &caps);
        if (ret != 0) {
            //the modeset parameter is set to 0
            LOG("ERROR: This driver requires the nvidia_drm.modeset kernel module parameter set to 1");
            return false;
        }
        return true;
    }
    return true;
}

bool isNvidiaDrmFd(int fd, bool log) {
    if (fd > 0) {
        char name[16] = {0};
        struct drm_version ver = {
            .name = name,
            .name_len = 15
        };
        int ret = ioctl(fd, DRM_IOCTL_VERSION, &ver);
        if (ret || strncmp(name, "nvidia-drm", 10)) {
            if (log) {
                LOG("Invalid driver for DRM device: %s", ver.name);
            }
            return false;
        }
        return true;
    }
    return false;
}

bool nvBackingImageHasExternalHostMemory(const BackingImage *img) {
    if (img == NULL || img->numObjects == 0) {
        return false;
    }
    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->externalMappings[i] == NULL) {
            return false;
        }
    }
    return true;
}

bool nvBackingImageHasExternalDeviceMemory(const BackingImage *img) {
    if (img == NULL || img->numObjects == 0) {
        return false;
    }
    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->externalDevicePtrs[i] == 0) {
            return false;
        }
    }
    return true;
}

uint8_t *nvBackingImageHostPlane(const BackingImage *img, uint32_t plane) {
    const uint32_t objectIndex = img->planeObjectIndex[plane];
    return (uint8_t *) img->externalMappings[objectIndex] + img->offsets[plane];
}

CUdeviceptr nvBackingImageDevicePlane(const BackingImage *img, uint32_t plane) {
    const uint32_t objectIndex = img->planeObjectIndex[plane];
    return img->externalDevicePtrs[objectIndex] + (CUdeviceptr) img->offsets[plane];
}

bool nvSyncBackingImageHostAccess(BackingImage *img, uint64_t flags) {
    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->externalMappings[i] == NULL) {
            continue;
        }
        struct dma_buf_sync sync = { .flags = flags };
        if (ioctl(img->fds[i], DMA_BUF_IOCTL_SYNC, &sync) != 0 && errno != ENOTTY && errno != EINVAL) {
            LOG("DMA_BUF_IOCTL_SYNC failed for imported object %u: %s", i, strerror(errno));
            return false;
        }
    }
    return true;
}

void nvDestroyImportedBackingImage(NVDriver *drv, BackingImage *img) {
    if (img == NULL) {
        return;
    }
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }
    if (img->borrowedBackingImage != NULL && atomic_load(&img->borrowedBackingImage->borrowCount) > 0) {
        atomic_fetch_sub(&img->borrowedBackingImage->borrowCount, 1);
        img->borrowedBackingImage = NULL;
    }

    nvDestroyBackingImageVideoProcObjects(drv, img);

    for (uint32_t i = 0; i < NVD_MAX_IMPORTED_OBJECTS; i++) {
        if (img->externalMappings[i] != NULL) {
            munmap(img->externalMappings[i], (size_t) img->externalMappingSize[i]);
        }
        if (img->externalDevicePtrs[i] != 0) {
            CHECK_CUDA_RESULT(drv->cu->cuMemFree(img->externalDevicePtrs[i]));
        }
        if (img->externalObjectMems[i] != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->externalObjectMems[i]));
        }
        if (img->fds[i] >= 0) {
            close(img->fds[i]);
        }
    }
    if (img->syncInitialized) {
        pthread_cond_destroy(&img->cond);
        pthread_mutex_destroy(&img->mutex);
    }
    nvStatsBackingImageDestroyed(drv, img);
    free(img);
}

void nvDestroyBackingImageVideoProcObjects(NVDriver *drv, BackingImage *img) {
    if (drv == NULL || img == NULL || img->borrowedCudaResources) {
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        if (img->cachedVideoProcSurfaces[i] != 0 && drv->cuSurfObjectDestroy != NULL) {
            CHECK_CUDA_RESULT(drv->cuSurfObjectDestroy(img->cachedVideoProcSurfaces[i]));
            img->cachedVideoProcSurfaces[i] = 0;
        }
        if (img->cachedVideoProcTextures[i] != 0) {
            CHECK_CUDA_RESULT(drv->cu->cuTexObjectDestroy(img->cachedVideoProcTextures[i]));
            img->cachedVideoProcTextures[i] = 0;
        }
    }
}
