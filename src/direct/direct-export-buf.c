#define _GNU_SOURCE 1

#include "../vabackend.h"
#include <stdio.h>
#include <stdlib.h>
#include <ffnvcodec/dynlink_loader.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <string.h>
#include "../backend-common.h"

#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>

#ifndef CUDA_ARRAY3D_SURFACE_LDST
#define CUDA_ARRAY3D_SURFACE_LDST 0x02
#endif

static void destroyBackingImage(NVDriver *drv, BackingImage *img);

static bool isRgbSurfaceFourcc(uint32_t fourcc) {
    return fourcc == VA_FOURCC_ARGB ||
           fourcc == VA_FOURCC_XRGB ||
           fourcc == VA_FOURCC_ABGR ||
           fourcc == VA_FOURCC_XBGR ||
           fourcc == VA_FOURCC_RGBA ||
           fourcc == VA_FOURCC_RGBX ||
           fourcc == VA_FOURCC_BGRA ||
           fourcc == VA_FOURCC_BGRX;
}

static NVFormat nvFormatForSurface(const NVSurface *surface) {
    if (isRgbSurfaceFourcc((uint32_t) surface->fourcc)) {
        return NV_FORMAT_ARGB;
    }

    switch (surface->format) {
    case cudaVideoSurfaceFormat_P016:
        switch (surface->bitDepth) {
        case 10:
            return NV_FORMAT_P010;
        case 12:
            return NV_FORMAT_P012;
        default:
            return NV_FORMAT_P016;
        }
    case cudaVideoSurfaceFormat_YUV444_16Bit:
        return NV_FORMAT_Q416;
    case cudaVideoSurfaceFormat_YUV444:
        return NV_FORMAT_444P;
    default:
        return NV_FORMAT_NV12;
    }
}

static void findGPUIndexFromFd(NVDriver *drv) {
    //find the CUDA device id
    uint8_t drmUuid[16];
    get_device_uuid(&drv->driverContext, drmUuid);

    int gpuCount = 0;
    if (CHECK_CUDA_RESULT(drv->cu->cuDeviceGetCount(&gpuCount))) {
        return;
    }

    for (int i = 0; i < gpuCount; i++) {
        CUuuid uuid;
        if (!CHECK_CUDA_RESULT(drv->cu->cuDeviceGetUuid(&uuid, i))) {
            if (memcmp(drmUuid, uuid.bytes, 16) == 0) {
                drv->cudaGpuId = i;
                return;
            }
        }
    }

    //default to index 0
    drv->cudaGpuId = 0;
}

static bool import_to_cuda(NVDriver *drv, NVDriverImage *image, int bpc, int channels, NVCudaImage *cudaImage, CUarray *array) {
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = image->nvFd,
        .flags     = 0,
        .size      = image->memorySize
    };

    //LOG("importing memory size: %dx%d = %x", image->width, image->height, image->memorySize);

    CHECK_CUDA_RESULT_RETURN(drv->cu->cuImportExternalMemory(&cudaImage->extMem, &extMemDesc), false);

    //For some reason, this close *must* be *here*, otherwise we will get random visual glitches.
    close(image->nvFd2);
    image->nvFd = -1;
    image->nvFd2 = -1;

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
        .arrayDesc = {
            .Width = image->width,
            .Height = image->height,
            .Depth = 0,
            .Format = bpc == 8 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
            .NumChannels = channels,
            .Flags = CUDA_ARRAY3D_SURFACE_LDST
        },
        .numLevels = 1,
        .offset = 0
    };
    //create a mimap array from the imported memory
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&cudaImage->mipmapArray, cudaImage->extMem, &mipmapArrayDesc), false);

    //create an array from the mipmap array
    CHECK_CUDA_RESULT_RETURN(drv->cu->cuMipmappedArrayGetLevel(array, cudaImage->mipmapArray, 0), false);

    return true;
}


static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

static bool direct_initExporter(NVDriver *drv) {
    //this is only needed to see errors in firefox
    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    const PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");
    // EGL_KHR_debug is diagnostic-only and must not be required for decoding.
    if (eglDebugMessageControlKHR != NULL) {
        eglDebugMessageControlKHR(debug, debugAttribs);
    } else {
        LOG("EGL_KHR_debug is unavailable; continuing without EGL debug logging");
    }

    //make sure we have a drm fd
    if (drv->drmFd == -1) {
        int nvdGpu = drv->cudaGpuId;
        if (nvdGpu == -1) {
            // The default GPU is the first one we find.
            nvdGpu = 0;
        }

        int fd = -1;
        int nvIdx = 0;
        char node[20] = {0, };
        for (uint16_t drmIdx = 128; drmIdx < 128 + 16; drmIdx++) {
            LOG("Searching for GPU: %d %d %d", nvIdx, nvdGpu, drmIdx)
            snprintf(node, sizeof(node), "/dev/dri/renderD%u", drmIdx);
            fd = open(node, O_RDWR|O_CLOEXEC);
            if (fd == -1) {
                continue;
            }

            if (!isNvidiaDrmFd(fd, true) || !checkModesetParameterFromFd(fd)) {
                close(fd);
                fd = -1;
                continue;
            }

            if (nvIdx != nvdGpu) {
                close(fd);
                fd = -1;
                nvIdx++;
                continue;
            }
            break;
        }
        if (fd < 0) {
            LOG("Unable to find NVIDIA GPU %d", nvdGpu);
            return false;
        }

        drv->drmFd = fd;
        LOG("Found NVIDIA GPU %d at %s", nvdGpu, node);
    } else {
        if (!isNvidiaDrmFd(drv->drmFd, true) || !checkModesetParameterFromFd(drv->drmFd)) {
            return false;
        }

        //dup it so we can close it later and not effect firefox
        const int duplicatedFd = dup(drv->drmFd);
        if (duplicatedFd < 0) {
            LOG("Unable to duplicate DRM fd: %s", strerror(errno));
            return false;
        }
        drv->drmFd = duplicatedFd;
    }

    const bool ret = init_nvdriver(&drv->driverContext, drv->drmFd);

    //TODO this isn't really correct as we don't know if the driver version actually supports importing them
    //but we don't have an easy way to find out.
    drv->supports16BitSurface = true;
    drv->supports444Surface = true;
    findGPUIndexFromFd(drv);

    return ret;
}

static void direct_releaseExporter(NVDriver *drv) {
    free_nvdriver(&drv->driverContext);
}

static void initBackingImageSync(BackingImage *img) {
    pthread_mutex_init(&img->mutex, NULL);
    pthread_cond_init(&img->cond, NULL);
    img->syncInitialized = true;
}

static void cacheBackingImageFdStat(BackingImage *img, int index) {
    if (img == NULL || index < 0 || index >= 4 || img->fds[index] < 0) {
        return;
    }

    struct stat s;
    if (fstat(img->fds[index], &s) == 0) {
        img->st_dev[index] = s.st_dev;
        img->st_ino[index] = s.st_ino;
    }
}

static void fillBackingImageClearRows(uint8_t *rows, size_t widthInBytes, uint32_t rowsCount, NVFormat format, uint32_t plane) {
    if (format == NV_FORMAT_ARGB) {
        for (uint32_t y = 0; y < rowsCount; y++) {
            uint8_t *row = rows + (size_t) y * widthInBytes;
            for (size_t x = 0; x + 3 < widthInBytes; x += 4) {
                row[x] = 0;
                row[x + 1] = 0;
                row[x + 2] = 0;
                row[x + 3] = 0xff;
            }
        }
        return;
    }

    if (formatsInfo[format].bppc == 1) {
        memset(rows, plane == 0 ? 16 : 128, widthInBytes * rowsCount);
        return;
    }

    const uint16_t value = plane == 0 ? 0x1000 : 0x8000;
    uint16_t *samples = (uint16_t*) rows;
    const size_t sampleCount = widthInBytes * rowsCount / sizeof(uint16_t);
    for (size_t i = 0; i < sampleCount; i++) {
        samples[i] = value;
    }
}

static bool clearBackingImagePlaneHost(NVDriver *drv, BackingImage *img,
                                       uint32_t plane) {
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    const NVFormatPlane *p = &fmtInfo->plane[plane];
    const uint32_t width = nvPlaneExtent(img->width, p->ss.x);
    const uint32_t height = nvPlaneExtent(img->height, p->ss.y);
    const size_t widthInBytes = (size_t) width * fmtInfo->bppc * p->channelCount;
    if (widthInBytes == 0 || height == 0) {
        return true;
    }

    const size_t maxChunkBytes = 8 * 1024 * 1024;
    uint32_t chunkRows = (uint32_t) (maxChunkBytes / widthInBytes);
    if (chunkRows == 0) {
        chunkRows = 1;
    }
    if (chunkRows > height) {
        chunkRows = height;
    }

    uint8_t *rows = NULL;
    while (chunkRows > 0) {
        rows = malloc(widthInBytes * chunkRows);
        if (rows != NULL) {
            break;
        }
        chunkRows /= 2;
    }
    if (rows == NULL) {
        LOG("Unable to allocate staging memory to clear BackingImage plane");
        return false;
    }

    fillBackingImageClearRows(rows, widthInBytes, chunkRows, img->format, plane);

    bool failed = false;
    for (uint32_t y = 0; y < height; y += chunkRows) {
        const uint32_t remainingRows = height - y;
        const uint32_t rowsToCopy = chunkRows < remainingRows ? chunkRows : remainingRows;
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_HOST,
            .srcHost = rows,
            .srcPitch = widthInBytes,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = img->arrays[plane],
            .dstY = y,
            .WidthInBytes = widthInBytes,
            .Height = rowsToCopy
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy))) {
            failed = true;
            break;
        }
        nvStatsAdd(drv, NV_STAT_SECURITY_CLEAR_BYTES, widthInBytes * rowsToCopy);
    }

    free(rows);
    return !failed;
}

static bool ensureSecurityClearResourcesLocked(NVDriver *drv,
                                               size_t requiredBytes) {
    if (drv->securityClearStream == NULL &&
        CHECK_CUDA_RESULT(drv->cu->cuStreamCreate(
            &drv->securityClearStream, CU_STREAM_NON_BLOCKING))) {
        return false;
    }
    if (drv->securityClearBufferSize >= requiredBytes &&
        drv->securityClearBuffer != 0) {
        return true;
    }

    if (drv->securityClearBuffer != 0) {
        if (CHECK_CUDA_RESULT(
                drv->cu->cuStreamSynchronize(drv->securityClearStream)) ||
            CHECK_CUDA_RESULT(drv->cu->cuMemFree(
                drv->securityClearBuffer))) {
            return false;
        }
        drv->securityClearBuffer = 0;
        drv->securityClearBufferSize = 0;
    }
    if (CHECK_CUDA_RESULT(drv->cu->cuMemAlloc(
            &drv->securityClearBuffer, requiredBytes))) {
        drv->securityClearBuffer = 0;
        return false;
    }
    drv->securityClearBufferSize = requiredBytes;
    return true;
}

static bool clearBackingImagePlaneGpu(NVDriver *drv, BackingImage *img,
                                      uint32_t plane) {
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    // The minimum supported ffnvcodec loader only exposes cuMemsetD8Async.
    // Multi-byte neutral patterns (P010/P016 and ARGB) therefore continue to
    // use the host staging fallback instead of depending on newer loader
    // members that may be absent at build time.
    if (img->format == NV_FORMAT_ARGB || fmtInfo->bppc != 1) {
        return false;
    }
    const NVFormatPlane *p = &fmtInfo->plane[plane];
    const uint32_t width = nvPlaneExtent(img->width, p->ss.x);
    const uint32_t height = nvPlaneExtent(img->height, p->ss.y);
    const size_t widthInBytes =
        (size_t) width * fmtInfo->bppc * p->channelCount;
    if (widthInBytes == 0 || height == 0) {
        return true;
    }

    const size_t maxChunkBytes = 8 * 1024 * 1024;
    uint32_t chunkRows = (uint32_t) (maxChunkBytes / widthInBytes);
    if (chunkRows == 0) {
        chunkRows = 1;
    }
    if (chunkRows > height) {
        chunkRows = height;
    }
    const size_t chunkBytes = widthInBytes * chunkRows;

    pthread_mutex_lock(&drv->securityClearMutex);
    bool failed = !ensureSecurityClearResourcesLocked(drv, chunkBytes);
    if (!failed) {
        failed = CHECK_CUDA_RESULT(drv->cu->cuMemsetD8Async(
            drv->securityClearBuffer, plane == 0 ? 16 : 128,
            chunkBytes, drv->securityClearStream));
    }

    for (uint32_t y = 0; !failed && y < height; y += chunkRows) {
        const uint32_t rowsToCopy = chunkRows < height - y
            ? chunkRows : height - y;
        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = drv->securityClearBuffer,
            .srcPitch = widthInBytes,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = img->arrays[plane],
            .dstY = y,
            .WidthInBytes = widthInBytes,
            .Height = rowsToCopy,
        };
        failed = CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(
            &cpy, drv->securityClearStream));
        if (!failed) {
            const uint64_t bytes = (uint64_t) widthInBytes * rowsToCopy;
            nvStatsAdd(drv, NV_STAT_SECURITY_CLEAR_BYTES, bytes);
            nvStatsAdd(drv, NV_STAT_SECURITY_CLEAR_GPU_BYTES, bytes);
        }
    }
    if (!failed) {
        failed = CHECK_CUDA_RESULT(
            drv->cu->cuStreamSynchronize(drv->securityClearStream));
    }
    pthread_mutex_unlock(&drv->securityClearMutex);
    return !failed;
}

static bool clearBackingImage(NVDriver *drv, BackingImage *img) {
    const uint64_t start = nvStatsTimestamp(drv);
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!clearBackingImagePlaneGpu(drv, img, i)) {
            nvStatsIncrement(drv, NV_STAT_SECURITY_CLEAR_HOST_FALLBACKS);
            if (img->format != NV_FORMAT_ARGB && fmtInfo->bppc == 1) {
                LOG("GPU backing-image clear failed; falling back to host staging");
            }
            if (!clearBackingImagePlaneHost(drv, img, i)) {
                return false;
            }
        }
    }
    const uint64_t end = nvStatsTimestamp(drv);
    if (start != 0 && end >= start) {
        nvStatsAdd(drv, NV_STAT_SECURITY_CLEAR_NS, end - start);
    }
    return true;
}

static uint64_t backingImageMemorySize(const BackingImage *img) {
    if (img == NULL) {
        return 0;
    }
    if (img->totalSize != 0) {
        return img->totalSize;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    uint64_t size = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size += img->size[i];
    }
    return size;
}

static bool backingImageCanPrune(const BackingImage *img) {
    return img != NULL &&
           img->surface == NULL &&
           atomic_load(&img->borrowCount) == 0;
}

static void linkDetachedBackingImageLocked(NVDriver *drv, BackingImage *img) {
    img->detachedPrev = drv->detachedBackingImageTail;
    img->detachedNext = NULL;
    if (drv->detachedBackingImageTail != NULL) {
        drv->detachedBackingImageTail->detachedNext = img;
    } else {
        drv->detachedBackingImageHead = img;
    }
    drv->detachedBackingImageTail = img;
    drv->detachedBackingImageBytes += backingImageMemorySize(img);
    drv->detachedBackingImageCount++;
}

static void unlinkDetachedBackingImageLocked(NVDriver *drv, BackingImage *img) {
    if (img->detachedPrev != NULL) {
        img->detachedPrev->detachedNext = img->detachedNext;
    } else {
        drv->detachedBackingImageHead = img->detachedNext;
    }
    if (img->detachedNext != NULL) {
        img->detachedNext->detachedPrev = img->detachedPrev;
    } else {
        drv->detachedBackingImageTail = img->detachedPrev;
    }
    const uint64_t bytes = backingImageMemorySize(img);
    drv->detachedBackingImageBytes = drv->detachedBackingImageBytes >= bytes
        ? drv->detachedBackingImageBytes - bytes : 0;
    if (drv->detachedBackingImageCount > 0) {
        drv->detachedBackingImageCount--;
    }
    img->detachedPrev = NULL;
    img->detachedNext = NULL;
    img->detachedSerial = 0;
}

static bool detachedBackingImagesOverLimit(uint64_t bytes, uint32_t count, const NVDriver *drv) {
    if (count == 0) {
        return false;
    }
    if (drv->maxDetachedBackingImages == 0 || drv->maxDetachedBackingImageBytes == 0) {
        return true;
    }
    const uint64_t scratch = drv->videoProcYBufferSize + drv->videoProcUVBufferSize +
                             drv->videoProcArgbBufferSize;
    const bool budgetExceeded = drv->memoryBudgetBytes != 0 &&
        (bytes > drv->memoryBudgetBytes || scratch > drv->memoryBudgetBytes - bytes);
    return count > drv->maxDetachedBackingImages ||
           bytes > drv->maxDetachedBackingImageBytes || budgetExceeded;
}

static bool pruneOldestDetachedBackingImageLocked(NVDriver *drv) {
    BackingImage *img = drv->detachedBackingImageHead;
    while (img != NULL && !backingImageCanPrune(img)) {
        img = img->detachedNext;
    }
    if (img == NULL) {
        return false;
    }

    uint32_t pruneIndex = UINT32_MAX;
    ARRAY_FOR_EACH(BackingImage*, candidate, &drv->images)
        if (candidate == img) {
            pruneIndex = candidate_idx;
            break;
        }
    END_FOR_EACH
    if (pruneIndex == UINT32_MAX) {
        return false;
    }
    unlinkDetachedBackingImageLocked(drv, img);
    destroyBackingImage(drv, img);
    remove_element_at(&drv->images, pruneIndex);
    nvStatsIncrement(drv, NV_STAT_BACKING_PRUNE_COUNT);
    return true;
}

static void pruneDetachedBackingImagesToLimits(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);
    while (detachedBackingImagesOverLimit(drv->detachedBackingImageBytes,
                                          drv->detachedBackingImageCount, drv)) {
        if (!pruneOldestDetachedBackingImageLocked(drv)) {
            break;
        }
    }

    pthread_mutex_unlock(&drv->imagesMutex);
}

// Allocate a multi-plane YUV backing image as one dma-buf object per plane, all sharing a
// single (max-across-planes) block-linear modifier. This satisfies Chromium's requirement
// that every plane report the same DRM modifier while keeping each plane at offset 0 of its
// own object, so per-plane importers (mpv/GStreamer/ffmpeg) detile the chroma plane
// correctly -- unlike the single-buffer layout, where chroma sits at a non-zero offset
// inside a shared tiled buffer and those importers mis-detile it.
static BackingImage *direct_allocateBackingImage_perPlane(NVDriver *drv,
                                                          NVSurface *surface,
                                                          bool sharedModifier) {
    NVDriverImage driverImages[3] = { 0 };
    for (uint32_t i = 0; i < ARRAY_SIZE(driverImages); i++) {
        driverImages[i].nvFd = -1;
        driverImages[i].nvFd2 = -1;
        driverImages[i].drmFd = -1;
    }
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    initBackingImageSync(backingImage);

    // Separate object per plane -> the multi-object export/destroy paths handle it.
    backingImage->isSingleBuffer = false;
    for (int i = 0; i < 4; i++) {
        backingImage->fds[i] = -1;
    }

    backingImage->format = nvFormatForSurface(surface);
    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];

    // The packed offsets returned here are ignored because each plane starts at
    // offset zero in its own object. Generic clients use natural per-plane block
    // heights; Chromium/ANGLE uses the largest block height for every plane so
    // every object advertises one shared modifier.
    calculate_unified_image_layout(&drv->driverContext, driverImages, surface->width, surface->height,
                                   fmtInfo->bppc, fmtInfo->numPlanes, fmtInfo->plane,
                                   sharedModifier);
    LOG_DEBUG("Allocating per-plane BackingImage: %p %ux%u shared_modifier=%d",
              backingImage, surface->width, surface->height, sharedModifier);

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        int memFd = -1, memFd2 = -1, drmFd = -1;
        if (!alloc_buffer(&drv->driverContext, driverImages[i].memorySize, &driverImages[i], &memFd, &memFd2, &drmFd)) {
            goto fail;
        }

        const CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
            .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
            .handle.fd = memFd,
            .flags     = 0,
            .size      = driverImages[i].memorySize
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&backingImage->cudaImages[i].extMem, &extMemDesc))) {
            close(memFd);
            close(memFd2);
            close(drmFd);
            goto fail;
        }
        // memFd is now owned by CUDA; memFd2 must be closed here (see import_to_cuda).
        close(memFd2);
        backingImage->fds[i] = drmFd;
        cacheBackingImageFdStat(backingImage, (int) i);

        // CUDA derives block height from the array height. For shared-modifier
        // objects use the aligned allocation height, just like the packed path;
        // for natural objects use the visible plane height.
        const uint32_t arrayHeight = sharedModifier && driverImages[i].pitch != 0
            ? driverImages[i].memorySize / driverImages[i].pitch
            : driverImages[i].height;
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
            .arrayDesc = {
                .Width = driverImages[i].width,
                .Height = arrayHeight,
                .Depth = 0,
                .Format = fmtInfo->bppc == 1 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                .NumChannels = fmtInfo->plane[i].channelCount,
                .Flags = CUDA_ARRAY3D_SURFACE_LDST
            },
            .numLevels = 1,
            .offset = 0
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&backingImage->cudaImages[i].mipmapArray, backingImage->cudaImages[i].extMem, &mipmapArrayDesc))) {
            goto fail;
        }
        if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(&backingImage->arrays[i], backingImage->cudaImages[i].mipmapArray, 0))) {
            goto fail;
        }

        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->offsets[i] = 0;
        backingImage->size[i] = driverImages[i].memorySize;
    }

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    backingImage->fourcc = fmtInfo->fourcc;

    if (!clearBackingImage(drv, backingImage)) {
        goto fail;
    }

    return backingImage;

fail:
    destroyBackingImage(drv, backingImage);
    return NULL;
}

static BackingImage *direct_allocateBackingImage_single(NVDriver *drv, NVSurface *surface) {
    NVDriverImage driverImages[3] = { 0 };
    for (uint32_t i = 0; i < ARRAY_SIZE(driverImages); i++) {
        driverImages[i].nvFd = -1;
        driverImages[i].nvFd2 = -1;
        driverImages[i].drmFd = -1;
    }
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    initBackingImageSync(backingImage);

    backingImage->isSingleBuffer = true;
    for (int i = 0; i < 4; i++) {
        backingImage->fds[i] = -1;
    }

    backingImage->format = nvFormatForSurface(surface);

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];

    // Pass unifyBlockHeight=true: all planes are packed into one shared buffer under a
    // single DRM modifier, so they must agree on one (largest) block height.
    backingImage->totalSize = calculate_unified_image_layout(&drv->driverContext, driverImages, surface->width, surface->height,
                                                             fmtInfo->bppc, fmtInfo->numPlanes, fmtInfo->plane, true);
    LOG_DEBUG("Allocating single BackingImage: %p %ux%u = %llu bytes", backingImage, surface->width, surface->height,
              (unsigned long long) backingImage->totalSize);

    int memFd = -1;
    int memFd2 = -1;
    int drmFd = -1;
    if (backingImage->totalSize > UINT32_MAX ||
        !alloc_buffer(&drv->driverContext, (uint32_t) backingImage->totalSize, driverImages, &memFd, &memFd2, &drmFd)) {
        goto fail;
    }
    LOG_DEBUG("Allocate single Buffer: %d %d %d", memFd, memFd2, drmFd);

    const CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
        .type      = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
        .handle.fd = memFd,
        .flags     = 0,
        .size      = backingImage->totalSize
    };

    LOG_DEBUG("Importing single memory to CUDA");
    if (CHECK_CUDA_RESULT(drv->cu->cuImportExternalMemory(&backingImage->extMem, &extMemDesc))) {
        goto fail;
    }

    close(memFd2);
    memFd = -1;
    memFd2 = -1;

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        // The single buffer is exported under one DRM modifier that carries a
        // single block height (log2GobsPerBlockY, the max across all planes).
        // CUDA, however, derives a plane's block-linear layout from the array
        // height it is handed, so a shorter plane (e.g. NV12 chroma when the
        // coded height is ~86-170px, as at 144p) would be tiled with a smaller
        // block than the modifier advertises. The importer then detiles that
        // plane with the wrong block height and the chroma turns green. Create
        // the array at the block-aligned height (memorySize / pitch) so CUDA
        // lays every plane out with the same block height the modifier reports.
        const uint32_t alignedHeight = driverImages[i].pitch != 0 ?
            driverImages[i].memorySize / driverImages[i].pitch : driverImages[i].height;
        CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapArrayDesc = {
            .arrayDesc = {
                .Width = driverImages[i].width,
                .Height = alignedHeight,
                .Depth = 0,
                .Format = fmtInfo->bppc == 1 ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                .NumChannels = fmtInfo->plane[i].channelCount,
                .Flags = CUDA_ARRAY3D_SURFACE_LDST
            },
            .numLevels = 1,
            .offset = driverImages[i].offset
        };

        if (CHECK_CUDA_RESULT(drv->cu->cuExternalMemoryGetMappedMipmappedArray(&backingImage->cudaImages[i].mipmapArray, backingImage->extMem, &mipmapArrayDesc))) {
            goto fail;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayGetLevel(&backingImage->arrays[i], backingImage->cudaImages[i].mipmapArray, 0))) {
            goto fail;
        }
    }

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    backingImage->fourcc = fmtInfo->fourcc;
    backingImage->fds[0] = drmFd;
    drmFd = -1;
    cacheBackingImageFdStat(backingImage, 0);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->offsets[i] = driverImages[i].offset;
        backingImage->size[i] = driverImages[i].memorySize;
    }
    if (!clearBackingImage(drv, backingImage)) {
        goto fail;
    }

    return backingImage;

fail:
    if (memFd >= 0) {
        close(memFd);
    }
    if (memFd2 >= 0) {
        close(memFd2);
    }
    if (drmFd >= 0) {
        close(drmFd);
    }

    destroyBackingImage(drv, backingImage);
    return NULL;
}

static BackingImage *direct_allocateBackingImageImpl(NVDriver *drv, NVSurface *surface) {
    if (!isRgbSurfaceFourcc((uint32_t) surface->fourcc)) {
        const NVDExportLayout layout = nvdGetExportLayout();
        if (layout == NVD_EXPORT_LAYOUT_PACKED) {
            return direct_allocateBackingImage_single(drv, surface);
        }
        return direct_allocateBackingImage_perPlane(
            drv, surface,
            layout == NVD_EXPORT_LAYOUT_PER_PLANE_SHARED_MODIFIER);
    }

    NVDriverImage driverImages[3] = { 0 };
    for (uint32_t i = 0; i < ARRAY_SIZE(driverImages); i++) {
        driverImages[i].nvFd = -1;
        driverImages[i].nvFd2 = -1;
        driverImages[i].drmFd = -1;
    }
    BackingImage *backingImage = calloc(1, sizeof(BackingImage));
    if (backingImage == NULL) {
        return NULL;
    }
    initBackingImageSync(backingImage);
    for (int i = 0; i < 4; i++) {
        backingImage->fds[i] = -1;
    }

    backingImage->format = nvFormatForSurface(surface);

    const NVFormatInfo *fmtInfo = &formatsInfo[backingImage->format];
    const NVFormatPlane *p = fmtInfo->plane;

    LOG_DEBUG("Allocating BackingImages: %p %dx%d", backingImage, surface->width, surface->height);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!alloc_image(&drv->driverContext,
                         nvPlaneExtent(surface->width, p[i].ss.x),
                         nvPlaneExtent(surface->height, p[i].ss.y),
                         p[i].channelCount, 8 * fmtInfo->bppc, p[i].fourcc, &driverImages[i])) {
            goto bail;
        }
    }

    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (!import_to_cuda(drv, &driverImages[i], 8 * fmtInfo->bppc, p[i].channelCount, &backingImage->cudaImages[i], &backingImage->arrays[i]))
            goto bail;
    }

    backingImage->width = surface->width;
    backingImage->height = surface->height;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        backingImage->fds[i] = driverImages[i].drmFd;
        cacheBackingImageFdStat(backingImage, (int) i);
        backingImage->strides[i] = driverImages[i].pitch;
        backingImage->mods[i] = driverImages[i].mods;
        backingImage->size[i] = driverImages[i].memorySize;
    }

    return backingImage;

bail:
    // Close the not-yet-transferred driver fds, then let destroyBackingImage
    // release any CUDA arrays/external-memory already imported by import_to_cuda
    // and the sync mutex/cond -- a plain free() here leaked all of those.
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        if (driverImages[i].nvFd >= 0) {
            close(driverImages[i].nvFd);
        }
        if (driverImages[i].nvFd2 >= 0) {
            close(driverImages[i].nvFd2);
        }
        if (driverImages[i].drmFd >= 0) {
            close(driverImages[i].drmFd);
        }
    }

    if (backingImage != NULL) {
        destroyBackingImage(drv, backingImage);
    }

    return NULL;
}

static BackingImage *direct_allocateBackingImage(NVDriver *drv, NVSurface *surface) {
    const uint64_t start = nvStatsTimestamp(drv);
    BackingImage *img = direct_allocateBackingImageImpl(drv, surface);
    const uint64_t end = nvStatsTimestamp(drv);
    nvStatsIncrement(drv, NV_STAT_BACKING_ALLOC_COUNT);
    if (start != 0 && end >= start) {
        nvStatsAdd(drv, NV_STAT_BACKING_ALLOC_NS, end - start);
    }
    return img;
}

static void destroyBackingImage(NVDriver *drv, BackingImage *img) {
    if (img->isExternalBuffer) {
        nvDestroyImportedBackingImage(drv, img);
        return;
    }
    nvStatsBackingImageDestroyed(drv, img);
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }
    if (img->borrowedBackingImage != NULL && atomic_load(&img->borrowedBackingImage->borrowCount) > 0) {
        atomic_fetch_sub(&img->borrowedBackingImage->borrowCount, 1);
        img->borrowedBackingImage = NULL;
    }

    for (uint32_t i = 0; i < NVD_MAX_IMPORTED_OBJECTS; i++) {
        if (img->externalMappings[i] != NULL) {
            munmap(img->externalMappings[i], (size_t) img->externalMappingSize[i]);
            img->externalMappings[i] = NULL;
            img->externalMappingSize[i] = 0;
        }
        if (img->externalDevicePtrs[i] != 0) {
            CHECK_CUDA_RESULT(drv->cu->cuMemFree(img->externalDevicePtrs[i]));
            img->externalDevicePtrs[i] = 0;
            img->externalDeviceSize[i] = 0;
        }
        if (img->externalObjectMems[i] != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->externalObjectMems[i]));
            img->externalObjectMems[i] = NULL;
        }
    }

    for (int i = 0; i < 4; i++) {
        if (img->fds[i] >= 0) {
            close(img->fds[i]);
        }
    }

    if (!img->borrowedCudaResources) {
        nvDestroyBackingImageVideoProcObjects(drv, img);
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            if (img->arrays[i] != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[i]));
            }

            if (img->cudaImages[i].mipmapArray != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuMipmappedArrayDestroy(img->cudaImages[i].mipmapArray));
            }
        }

        if (img->isSingleBuffer) {
            if (img->extMem != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->extMem));
            }
        } else {
            for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
                if (img->cudaImages[i].extMem != NULL) {
                    CHECK_CUDA_RESULT(drv->cu->cuDestroyExternalMemory(img->cudaImages[i].extMem));
                }
            }
        }
    }
    if (img->syncInitialized) {
        pthread_cond_destroy(&img->cond);
        pthread_mutex_destroy(&img->mutex);
    }

    memset(img, 0, sizeof(BackingImage));
    free(img);
}

// Reclaim the single oldest detached, unborrowed backing image (lowest
// detachedSerial). Returns true if one was destroyed. Used to relieve
// allocation pressure oldest-first, so the most-recently-detached images --
// whose exported dma-bufs are the most likely to still be in the client's
// display pipeline, or about to be re-imported across a codec/format switch --
// are freed last rather than all at once.
static bool pruneOldestReclaimableDetachedBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);
    const bool pruned = pruneOldestDetachedBackingImageLocked(drv);
    pthread_mutex_unlock(&drv->imagesMutex);

    return pruned;
}

static void direct_attachBackingImageToSurface(NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
    img->detachedSerial = 0;
    nvBackingImageStoreSurfaceColorMetadata(img, surface);
}

static void direct_detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        return;
    }

    if (surface->backingImage->isExternalBuffer || surface->backingImage->borrowedCudaResources) {
        destroyBackingImage(drv, surface->backingImage);
        surface->backingImage = NULL;
        return;
    }

    // Publish the detach (surface -> NULL) and assign the detached serial while
    // holding imagesMutex. The prune path reads both under the same lock to pick
    // the oldest reclaimable image; doing it unlocked exposes a window where a
    // just-detached image (serial not yet written, still 0) looks like the
    // oldest and gets reclaimed first -- exactly the most-recently-detached
    // image whose exported dma-buf is most likely still in the client's
    // pipeline, which is the corruption the oldest-first prune exists to avoid.
    pthread_mutex_lock(&drv->imagesMutex);
    nvStatsBackingImageSetActive(drv, surface->backingImage, false);
    surface->backingImage->surface = NULL;
    surface->backingImage->detachedSerial = ++drv->detachedBackingImageSerial;
    linkDetachedBackingImageLocked(drv, surface->backingImage);
    surface->backingImage = NULL;
    pthread_mutex_unlock(&drv->imagesMutex);

    pruneDetachedBackingImagesToLimits(drv);
}

static void direct_destroyAllBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        if (it->detachedSerial != 0) {
            unlinkDetachedBackingImageLocked(drv, it);
        }
        destroyBackingImage(drv, it);
        remove_element_at(&drv->images, it_idx);
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);
}

static bool direct_pruneToMemoryBudget(NVDriver *drv, uint64_t extraGpuBytes) {
    if (drv->memoryBudgetBytes == 0) {
        return true;
    }
    pthread_mutex_lock(&drv->imagesMutex);
    uint64_t scratch = drv->videoProcYBufferSize + drv->videoProcUVBufferSize +
                       drv->videoProcArgbBufferSize;
    while (extraGpuBytes > drv->memoryBudgetBytes ||
           drv->detachedBackingImageBytes + scratch > drv->memoryBudgetBytes - extraGpuBytes) {
        if (!pruneOldestDetachedBackingImageLocked(drv)) {
            break;
        }
    }
    const bool fits = extraGpuBytes <= drv->memoryBudgetBytes &&
        drv->detachedBackingImageBytes + scratch <= drv->memoryBudgetBytes - extraGpuBytes;
    pthread_mutex_unlock(&drv->imagesMutex);
    return fits;
}

static bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface,
                               uint32_t pitch, CUstream stream, CUevent completeEvent) {
    BackingImage *img = surface->backingImage;
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    uint32_t y = 0;
    const bool hostDestination = nvBackingImageHasExternalHostMemory(img);
    const bool deviceDestination = nvBackingImageHasExternalDeviceMemory(img);
    if (hostDestination && !nvSyncBackingImageHostAccess(img, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
        nvSyncBackingImageHostAccess(img, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        return false;
    }

    // mmap-backed destinations are not guaranteed to be page-locked, so keep
    // their copies synchronous. If NVDEC queued output processing on the
    // context stream, finish that work before reading the mapped frame.
    if (hostDestination && stream != NULL &&
        CHECK_CUDA_RESULT(drv->cu->cuStreamSynchronize(stream))) {
        nvSyncBackingImageHostAccess(img, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        return false;
    }

    bool failed = false;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        const uint32_t widthInBytes = nvPlaneExtent(surface->width, p->ss.x) * fmtInfo->bppc * p->channelCount;
        const uint32_t height = nvPlaneExtent(surface->height, p->ss.y);
        if (hostDestination) {
            CUDA_MEMCPY2D cpy = {
                .srcMemoryType = CU_MEMORYTYPE_DEVICE,
                .srcDevice = ptr,
                .srcY = y,
                .srcPitch = pitch,
                .dstMemoryType = CU_MEMORYTYPE_HOST,
                .dstHost = nvBackingImageHostPlane(img, i),
                .dstPitch = (uint32_t) img->strides[i],
                .Height = height,
                .WidthInBytes = widthInBytes
            };
            if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy))) {
                failed = true;
                break;
            }
            nvStatsAdd(drv, NV_STAT_HOST_COPY_BYTES, (uint64_t) widthInBytes * height);
            y += height;
            continue;
        }

        CUDA_MEMCPY2D cpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = ptr,
            .srcY = y,
            .srcPitch = pitch,
            .Height = height,
            .WidthInBytes = widthInBytes
        };
        if (deviceDestination) {
            cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            cpy.dstDevice = nvBackingImageDevicePlane(img, i);
            cpy.dstPitch = (uint32_t) img->strides[i];
        } else {
            cpy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            cpy.dstArray = img->arrays[i];
        }
        failed = CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, stream));
        if (failed) {
            break;
        }
        nvStatsAdd(drv, NV_STAT_DEVICE_COPY_BYTES, (uint64_t) widthInBytes * height);
        y += height;
    }

    if (hostDestination &&
        !nvSyncBackingImageHostAccess(img, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) {
        failed = true;
    }
    if (!hostDestination && !failed) {
        if (completeEvent != NULL) {
            failed = CHECK_CUDA_RESULT(drv->cu->cuEventRecord(completeEvent, stream));
        }
        if (!failed) {
            // The exported external-memory array is consumed outside CUDA.
            // A completed event alone was not consistently sufficient on the
            // release-build hardware matrix; synchronize the owning stream
            // before NVDEC unmap and publication.
            failed = CHECK_CUDA_RESULT(drv->cu->cuStreamSynchronize(stream));
        }
    }
    if (failed) {
        return false;
    }

    return true;
}

static bool direct_realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = direct_allocateBackingImage(drv, surface);
        if (img == NULL) {
            // Allocation failed, typically under VRAM pressure. Reclaim detached
            // backing images oldest-first, retrying the allocation after each
            // one, instead of destroying the whole detached cache at once. The
            // most-recently-detached images are the most likely to still have
            // their exported dma-buf in flight in the client (or about to be
            // re-imported across a codec/format switch); freeing those out from
            // under the client corrupts the displayed frame. Oldest-first with a
            // retry between each prune frees only what this allocation needs and
            // keeps the recent frames alive.
            uint32_t reclaimed = 0;
            while (img == NULL && pruneOldestReclaimableDetachedBackingImage(drv)) {
                reclaimed++;
                img = direct_allocateBackingImage(drv, surface);
            }
            if (reclaimed > 0 && img != NULL) {
                LOG("Reclaimed %u detached BackingImage(s) oldest-first after allocation failure", reclaimed)
            }
            if (img == NULL) {
                LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
                pthread_mutex_unlock(&surface->mutex);
                return false;
            }
        }

        direct_attachBackingImageToSurface(surface, img);
        pthread_mutex_lock(&drv->imagesMutex);
        bool added = add_element(&drv->images, img);
        if (added) {
            nvStatsBackingImageCreated(drv, img, true);
        }
        pthread_mutex_unlock(&drv->imagesMutex);
        if (!added) {
            surface->backingImage = NULL;
            img->surface = NULL;
            destroyBackingImage(drv, img);
            pthread_mutex_unlock(&surface->mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

static BackingImage *resolveSyncImage(BackingImage *img) {
    if (img != NULL && img->borrowedBackingImage != NULL) {
        return img->borrowedBackingImage;
    }
    return img;
}

static bool direct_exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface,
                                 uint32_t pitch, CUstream stream, CUevent completeEvent) {
    if (!direct_realiseSurface(drv, surface)) {
        return false;
    }

    if (ptr != 0) {
        BackingImage *img = surface->backingImage;
        BackingImage *syncImg = resolveSyncImage(img);
        if (syncImg != NULL && syncImg->syncInitialized) {
            pthread_mutex_lock(&syncImg->mutex);
            syncImg->resolving = true;
            pthread_mutex_unlock(&syncImg->mutex);
        }
        nvStatsIncrement(drv, NV_STAT_EXPORT_COPIES);
        if (img != NULL && nvBackingImageHasExternalHostMemory(img)) {
            nvStatsIncrement(drv, NV_STAT_EXPORT_HOST_COPIES);
            nvStatsIncrement(drv, NV_STAT_HOST_FALLBACK_FRAMES);
        }
        nvBackingImageStoreSurfaceColorMetadata(img, surface);
        bool copied = copyFrameToSurface(drv, ptr, surface, pitch, stream, completeEvent);
        // The resolve worker clears an asynchronous image only after NVDEC
        // unmap. Publishing the backing image here permits an exporter to race
        // that final ownership transition even though the CUDA copy completed.
        if ((completeEvent == NULL || !copied) &&
            syncImg != NULL && syncImg->syncInitialized) {
            pthread_mutex_lock(&syncImg->mutex);
            syncImg->resolving = false;
            pthread_cond_broadcast(&syncImg->cond);
            pthread_mutex_unlock(&syncImg->mutex);
        }
        if (!copied) {
            return false;
        }
    } else {
        LOG("exporting with null ptr")
    }

    return true;
}

static bool direct_fillExportDescriptor(NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc) {
    const BackingImage *img = surface->backingImage;
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];

    nvBackingImageStoreSurfaceColorMetadata(surface->backingImage, surface);

    desc->fourcc = fmtInfo->fourcc;
    desc->width = surface->width;
    desc->height = surface->height;

    desc->num_layers = fmtInfo->numPlanes;
    nvStatsIncrement(drv, NV_STAT_EXPORT_DESCRIPTORS);
    if (img->isSingleBuffer) {
        nvStatsIncrement(drv, NV_STAT_EXPORT_DESCRIPTORS_SINGLE);
        desc->num_objects = 1;
        desc->objects[0].fd = dup(img->fds[0]);
        if (desc->objects[0].fd < 0) {
            desc->num_objects = 0;
            return false;
        }
        desc->objects[0].size = img->totalSize;
        desc->objects[0].drm_format_modifier = img->mods[0];

        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            desc->layers[i].drm_format = fmtInfo->plane[i].fourcc;
            desc->layers[i].num_planes = 1;
            desc->layers[i].object_index[0] = 0;
            desc->layers[i].offset[0] = img->offsets[i];
            desc->layers[i].pitch[0] = img->strides[i];
        }
    } else {
        nvStatsIncrement(drv, NV_STAT_EXPORT_DESCRIPTORS_MULTI);
        desc->num_objects = fmtInfo->numPlanes;

        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            desc->objects[i].fd = dup(img->fds[i]);
            if (desc->objects[i].fd < 0) {
                for (uint32_t j = 0; j < i; j++) {
                    close(desc->objects[j].fd);
                    desc->objects[j].fd = -1;
                }
                desc->num_objects = 0;
                return false;
            }
            desc->objects[i].size = img->size[i];
            desc->objects[i].drm_format_modifier = img->mods[i];

            desc->layers[i].drm_format = fmtInfo->plane[i].fourcc;
            desc->layers[i].num_planes = 1;
            desc->layers[i].object_index[0] = i;
            desc->layers[i].offset[0] = img->offsets[i];
            desc->layers[i].pitch[0] = img->strides[i];
        }
    }

    return true;
}

const NVBackend DIRECT_BACKEND = {
    .name = "direct",
    .initExporter = direct_initExporter,
    .releaseExporter = direct_releaseExporter,
    .exportCudaPtr = direct_exportCudaPtr,
    .detachBackingImageFromSurface = direct_detachBackingImageFromSurface,
    .realiseSurface = direct_realiseSurface,
    .fillExportDescriptor = direct_fillExportDescriptor,
    .destroyAllBackingImage = direct_destroyAllBackingImage,
    .pruneToMemoryBudget = direct_pruneToMemoryBudget
};
