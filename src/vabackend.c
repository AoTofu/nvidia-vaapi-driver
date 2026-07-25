#define _GNU_SOURCE

#include "vabackend.h"
#include "backend-common.h"
#include "decode-surfaces.h"
#include "kernels.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <drm_fourcc.h>

#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>
#include <dlfcn.h>

#include <time.h>

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_include
#define __has_include(x) 0
#endif

#ifndef CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_FD
#define CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_FD ((CUexternalMemoryHandleType)7)
#endif

#if __has_include(<pthread_np.h>)
#include <pthread_np.h>
#define gettid pthread_getthreadid_np
#define HAVE_GETTID 1
#endif

#ifndef HAVE_GETTID
#include <sys/syscall.h>
/* Bionic and glibc >= 2.30 declare gettid() system call wrapper in unistd.h and
 * has a definition for it */
#ifdef __BIONIC__
#define HAVE_GETTID 1
#elif !defined(__GLIBC_PREREQ)
#define HAVE_GETTID 0
#elif !__GLIBC_PREREQ(2,30)
#define HAVE_GETTID 0
#else
#define HAVE_GETTID 1
#endif
#endif

pid_t nv_gettid(void)
{
#if HAVE_GETTID
    return gettid();
#else
    return syscall(__NR_gettid);
#endif
}

static pthread_mutex_t concurrency_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t instances;
static uint32_t max_instances;

static void releaseInstanceSlot(void) {
    pthread_mutex_lock(&concurrency_mutex);
    if (instances > 0) {
        instances--;
    }
    LOG("Now have %d (%d max) instances", instances, max_instances);
    pthread_mutex_unlock(&concurrency_mutex);
}

static CudaFunctions *cu;
static CuvidFunctions *cv;

extern const NVCodec __start_nvd_codecs[];
extern const NVCodec __stop_nvd_codecs[];

static FILE *LOG_OUTPUT;
static FILE *STATS_OUTPUT;
static bool LOG_DEBUG_ENABLED;
static bool USE_SINGLE_BUFFER_EXPORT;

// Destination for the statistics dump: the dedicated stats log if one was opened
// (NVD_STATS_LOG), otherwise the regular log stream. Used by the stats subsystem.
FILE *nvStatsOutput(void) {
    return STATS_OUTPUT != NULL ? STATS_OUTPUT : LOG_OUTPUT;
}

// Fallback byte ceiling for the detached backing-image cache when the GPU's
// total VRAM cannot be queried. When it can, the default scales with the
// device instead: totalVram/64 (~1.6%), clamped to [64 MiB, 512 MiB]. The
// image-count limit below is the primary bound (it matches the cache's
// purpose: keep the last N frames alive across a stream switch, whatever
// their resolution); the byte ceiling is a safety net so those N frames
// can't pin an outsized share of a small card's VRAM.
static const uint64_t DEFAULT_MAX_DETACHED_BACKING_IMAGE_BYTES = 128ULL * 1024ULL * 1024ULL;
static const uint64_t MIN_DYNAMIC_DETACHED_BACKING_IMAGE_BYTES = 64ULL * 1024ULL * 1024ULL;
static const uint64_t MAX_DYNAMIC_DETACHED_BACKING_IMAGE_BYTES = 512ULL * 1024ULL * 1024ULL;
static const uint32_t DEFAULT_MAX_DETACHED_BACKING_IMAGES = 16;
static const uint64_t DEFAULT_VIDEOPROC_SCRATCH_MAX_BYTES = 256ULL * 1024ULL * 1024ULL;
static const uint32_t VIDEOPROC_SCRATCH_IDLE_FRAMES = 120;

static int gpu = -1;
static enum {
    EGL, DIRECT
} backend = DIRECT;

const NVFormatInfo formatsInfo[] =
{
    [NV_FORMAT_NONE] = {0},
    [NV_FORMAT_NV12] = {1, 2, DRM_FORMAT_NV12,     false, false, {{1, DRM_FORMAT_R8,       {0,0}}, {2, DRM_FORMAT_RG88,   {1,1}}},                            {VA_FOURCC_NV12, VA_LSB_FIRST,   12, 0,0,0,0,0}},
    [NV_FORMAT_P010] = {2, 2, DRM_FORMAT_P010,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P010, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P012] = {2, 2, DRM_FORMAT_P012,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P012, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P016] = {2, 2, DRM_FORMAT_P016,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P016, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_444P] = {1, 3, DRM_FORMAT_YUV444,   false, true,  {{1, DRM_FORMAT_R8,       {0,0}}, {1, DRM_FORMAT_R8,     {0,0}}, {1, DRM_FORMAT_R8, {0,0}}}, {VA_FOURCC_444P, VA_LSB_FIRST,   24, 0,0,0,0,0}},
#if VA_CHECK_VERSION(1, 20, 0)
    [NV_FORMAT_Q416] = {2, 3, DRM_FORMAT_INVALID,  true,  true,  {{1, DRM_FORMAT_R16,      {0,0}}, {1, DRM_FORMAT_R16,    {0,0}}, {1, DRM_FORMAT_R16,{0,0}}}, {VA_FOURCC_Q416, VA_LSB_FIRST,   48, 0,0,0,0,0}},
#endif
    [NV_FORMAT_ARGB] = {1, 1, VA_FOURCC_ARGB,      false, false, {{4, DRM_FORMAT_ARGB8888, {0,0}}},                            {VA_FOURCC_ARGB, VA_LSB_FIRST,   32, 0,0,0,0,0}},
};

static NVFormat nvFormatFromVaFormat(uint32_t fourcc) {
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].vaFormat.fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

static bool isRgbFourcc(uint32_t fourcc) {
    return fourcc == VA_FOURCC_ARGB ||
           fourcc == VA_FOURCC_XRGB ||
           fourcc == VA_FOURCC_ABGR ||
           fourcc == VA_FOURCC_XBGR ||
           fourcc == VA_FOURCC_RGBA ||
           fourcc == VA_FOURCC_RGBX ||
           fourcc == VA_FOURCC_BGRA ||
           fourcc == VA_FOURCC_BGRX;
}

static NVFormat nvFormatFromSurfaceFourcc(uint32_t fourcc) {
    if (isRgbFourcc(fourcc)) {
        return NV_FORMAT_ARGB;
    }
    return nvFormatFromVaFormat(fourcc);
}

static const char *fourccString(uint32_t fourcc, char out[5]) {
    out[0] = (char) (fourcc & 0xff);
    out[1] = (char) ((fourcc >> 8) & 0xff);
    out[2] = (char) ((fourcc >> 16) & 0xff);
    out[3] = (char) ((fourcc >> 24) & 0xff);
    out[4] = '\0';
    return out;
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

static bool backingImageFdMatchesStat(const BackingImage *img, const struct stat *fdStat, int index) {
    return img->fds[index] >= 0 &&
           img->st_dev[index] == fdStat->st_dev &&
           img->st_ino[index] == fdStat->st_ino;
}

static bool backingImageMatchesImport(BackingImage *img, const struct stat *fdStat, NVFormat format, uint32_t width, uint32_t height) {
    if (img == NULL || img->isExternalBuffer || img->borrowedCudaResources ||
        img->format != format || img->width != width || img->height != height) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (backingImageFdMatchesStat(img, fdStat, i)) {
            return true;
        }
    }
    return false;
}

static BackingImage *retainBackingImageByFd(NVDriver *drv, int fd, NVFormat format, uint32_t width, uint32_t height) {
    struct stat fdStat;
    if (fd < 0 || fstat(fd, &fdStat) != 0) {
        return NULL;
    }

    BackingImage *ret = NULL;
    pthread_mutex_lock(&drv->imagesMutex);
    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        if (backingImageMatchesImport(img, &fdStat, format, width, height)) {
            ret = img;
            atomic_fetch_add(&ret->borrowCount, 1);
            break;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->imagesMutex);

    return ret;
}

static bool backingImageMatchesImportedLayout(const BackingImage *existing,
                                              const BackingImage *imported) {
    if (existing == NULL || imported == NULL || existing->format != imported->format) {
        return false;
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[existing->format];
    const uint32_t expectedObjects = existing->isSingleBuffer ? 1 : fmtInfo->numPlanes;
    if (imported->numObjects != expectedObjects || imported->numPlanes != fmtInfo->numPlanes) {
        return false;
    }

    for (uint32_t plane = 0; plane < fmtInfo->numPlanes; plane++) {
        const uint32_t importedObject = imported->planeObjectIndex[plane];
        const uint32_t existingObject = existing->isSingleBuffer ? 0 : plane;
        struct stat importedStat;
        if (importedObject >= imported->numObjects ||
            fstat(imported->fds[importedObject], &importedStat) != 0 ||
            !backingImageFdMatchesStat(existing, &importedStat, (int) existingObject) ||
            imported->offsets[plane] != existing->offsets[plane] ||
            imported->strides[plane] != existing->strides[plane] ||
            imported->mods[importedObject] != existing->mods[existingObject]) {
            return false;
        }
    }
    return true;
}

static bool processRequiresSingleBufferExport(void) {
#ifdef __linux__
    // Chromium stores only one modifier in gfx::NativePixmapHandle and aborts
    // if separate dma-buf objects advertise different per-plane modifiers.
    // Detect its GPU process by comm instead of changing the default export:
    // non-Chromium clients retain the per-plane block heights fixed by a2833b2.
    char comm[32] = { 0 };
    int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t bytesRead = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (bytesRead > 0) {
            comm[bytesRead] = '\0';
            comm[strcspn(comm, "\r\n")] = '\0';
            return strcmp(comm, "chrome") == 0 || strcmp(comm, "chromium") == 0;
        }
    }
#endif
    return false;
}

__attribute__ ((constructor))
static void init() {
    char *nvdLog = getenv("NVD_LOG");
    if (nvdLog != NULL) {
        if (strcmp(nvdLog, "1") == 0) {
            LOG_OUTPUT = stdout;
        } else {
            LOG_OUTPUT = fopen(nvdLog, "a");
            if (LOG_OUTPUT == NULL) {
                LOG_OUTPUT = stdout;
            }
        }
    }
    char *nvdLogVerbose = getenv("NVD_LOG_VERBOSE");
    LOG_DEBUG_ENABLED = nvdLogVerbose != NULL && strcmp(nvdLogVerbose, "0") != 0;
    // Keep the explicit override for wrappers with a non-standard process name.
    // Automatic Chromium compatibility is deliberately narrow so a2833b2's
    // per-plane modifier path remains the default for every other VA client.
    USE_SINGLE_BUFFER_EXPORT = getenv("NVD_SINGLE_BUFFER") != NULL ||
                               processRequiresSingleBufferExport();
    char *nvdStats = getenv("NVD_STATS");
    if (nvdStats != NULL && strcmp(nvdStats, "0") != 0) {
        char *nvdStatsLog = getenv("NVD_STATS_LOG");
        if (nvdStatsLog != NULL) {
            STATS_OUTPUT = fopen(nvdStatsLog, "a");
            if (STATS_OUTPUT == NULL) {
                STATS_OUTPUT = stdout;
            }
        } else if (LOG_OUTPUT != NULL) {
            STATS_OUTPUT = LOG_OUTPUT;
        } else {
            STATS_OUTPUT = stdout;
        }
    }

    char *nvdGpu = getenv("NVD_GPU");
    if (nvdGpu != NULL) {
        gpu = atoi(nvdGpu);
    }

    char *nvdMaxInstances = getenv("NVD_MAX_INSTANCES");
    if (nvdMaxInstances != NULL) {
        max_instances = atoi(nvdMaxInstances);
    }

    char *nvdBackend = getenv("NVD_BACKEND");
    if (nvdBackend != NULL) {
        if (strncmp(nvdBackend, "direct", 6) == 0) {
            backend = DIRECT;
        } else if (strncmp(nvdBackend, "egl", 6) == 0) {
            backend = EGL;
        }
    }

#ifdef __linux__
    //try to detect the Firefox sandbox and skip loading CUDA if detected
    int fd = open("/proc/version", O_RDONLY);
    if (fd < 0) {
        LOG("ERROR: Potential Firefox sandbox detected, failing to init!");
        LOG("If running in Firefox, set env var MOZ_DISABLE_RDD_SANDBOX=1 to disable sandbox.");
        //exit here so we don't init CUDA, unless an env var has been set to force us to init even though we've detected a sandbox
        if (getenv("NVD_FORCE_INIT") == NULL) {
            return;
        }
    } else {
        //we're not in a sandbox
        //continue as normal
        close(fd);
    }
#endif

    //initialise the CUDA and NVDEC functions
    int ret = cuda_load_functions(&cu, NULL);
    if (ret != 0) {
        cu = NULL;
        LOG("Failed to load CUDA functions");
        return;
    }
    ret = cuvid_load_functions(&cv, NULL);
    if (ret != 0) {
        cv = NULL;
        LOG("Failed to load NVDEC functions");
        return;
    }

    //Not really much we can do here to abort the loading of the library
    CHECK_CUDA_RESULT(cu->cuInit(0));
}

__attribute__ ((destructor))
static void cleanup() {
    if (cv != NULL) {
        cuvid_free_functions(&cv);
    }
    if (cu != NULL) {
        cuda_free_functions(&cu);
    }
}


#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(gnu_printf) || (defined(__GNUC__) && !defined(__clang__))
__attribute((format(gnu_printf, 4, 5)))
#endif
void logger(const char *filename, const char *function, int line, const char *msg, ...) {
    if (LOG_OUTPUT == 0) {
        return;
    }

    va_list argList;
    char formattedMessage[1024];

    va_start(argList, msg);
    vsnprintf(formattedMessage, 1024, msg, argList);
    va_end(argList);

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    fprintf(LOG_OUTPUT, "%10ld.%09ld [%d-%d] %s:%4d %24s %s\n", (long)tp.tv_sec, tp.tv_nsec, getpid(), nv_gettid(), filename, line, function, formattedMessage);
    fflush(LOG_OUTPUT);
}

bool nvdLogDebugEnabled(void) {
    return LOG_DEBUG_ENABLED;
}

bool nvdUseSingleBufferExport(void) {
    return USE_SINGLE_BUFFER_EXPORT;
}

static uint64_t parseEnvU64(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        LOG("Ignoring invalid %s=%s", name, value);
        return fallback;
    }

    return parsed;
}

static uint32_t parseDecodeSurfaceOverride(bool *automatic) {
    *automatic = false;
    const char *value = getenv("NVD_DECODE_SURFACES");
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    if (strcmp(value, "auto") == 0) {
        *automatic = true;
        return 0;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        LOG("Ignoring invalid NVD_DECODE_SURFACES=%s", value);
        return 0;
    }
    return (uint32_t) parsed;
}

// Default byte ceiling for the detached backing-image cache, derived from the
// GPU's total VRAM: totalVram/64 clamped to [64 MiB, 512 MiB]. Computed once at
// init (deterministic per machine) -- deliberately NOT from *free* VRAM, which
// is a device-wide value shared with every other process and would make the
// cache size race with whatever else the GPU is running. ffnvcodec's loader
// doesn't expose cuDeviceTotalMem, so resolve it from the already-loaded
// libcuda; if anything fails, fall back to the fixed default.
static uint64_t defaultMaxDetachedBackingImageBytes(int cudaGpuId) {
    typedef CUresult CUDAAPI tcuDeviceTotalMem_l(size_t *bytes, CUdevice dev);

    if (cu == NULL || cu->cuDeviceGet == NULL) {
        return DEFAULT_MAX_DETACHED_BACKING_IMAGE_BYTES;
    }

    // libcuda is already loaded by cuda_load_functions; RTLD_NOLOAD just takes
    // another reference to it without touching the filesystem.
    void *libcuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (libcuda == NULL) {
        libcuda = dlopen("libcuda.so.1", RTLD_NOW);
    }
    if (libcuda == NULL) {
        return DEFAULT_MAX_DETACHED_BACKING_IMAGE_BYTES;
    }

    uint64_t ret = DEFAULT_MAX_DETACHED_BACKING_IMAGE_BYTES;
    tcuDeviceTotalMem_l *deviceTotalMem = (tcuDeviceTotalMem_l *) dlsym(libcuda, "cuDeviceTotalMem_v2");
    CUdevice dev = 0;
    size_t totalVram = 0;
    if (deviceTotalMem != NULL &&
        cu->cuDeviceGet(&dev, cudaGpuId >= 0 ? cudaGpuId : 0) == CUDA_SUCCESS &&
        deviceTotalMem(&totalVram, dev) == CUDA_SUCCESS && totalVram > 0) {
        ret = totalVram / 64;
        if (ret < MIN_DYNAMIC_DETACHED_BACKING_IMAGE_BYTES) {
            ret = MIN_DYNAMIC_DETACHED_BACKING_IMAGE_BYTES;
        } else if (ret > MAX_DYNAMIC_DETACHED_BACKING_IMAGE_BYTES) {
            ret = MAX_DYNAMIC_DETACHED_BACKING_IMAGE_BYTES;
        }
        LOG("Detached backing-image cache ceiling: %llu bytes (total VRAM %zu bytes)",
            (unsigned long long) ret, totalVram);
    }

    dlclose(libcuda);
    return ret;
}

bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line) {
    if (CUDA_SUCCESS != err) {
        const char *errStr = NULL;
        cu->cuGetErrorString(err, &errStr);
        logger(file, function, line, "CUDA ERROR '%s' (%d)\n", errStr, err);
        return true;
    }
    return false;
}

static Object allocateObject(NVDriver *drv, ObjectType type, size_t allocatePtrSize) {
    pthread_mutex_lock(&drv->objectCreationMutex);
    if (drv->terminating) {
        pthread_mutex_unlock(&drv->objectCreationMutex);
        return NULL;
    }
    Object newObj = nvdObjectTableAllocate(&drv->objects, (uint8_t) type, allocatePtrSize);
    pthread_mutex_unlock(&drv->objectCreationMutex);
    return newObj;
}

static Object getObject(NVDriver *drv, ObjectType type, VAGenericID id) {
    Object ret = NULL;
    if (id != VA_INVALID_ID) {
        nvStatsIncrement(drv, NV_STAT_OBJECT_LOOKUP_COUNT);
        pthread_mutex_lock(&drv->objectCreationMutex);
        if (!drv->terminating) {
            ret = nvdObjectTableGet(&drv->objects, (uint8_t) type, id);
        }
        pthread_mutex_unlock(&drv->objectCreationMutex);
        nvStatsAdd(drv, NV_STAT_OBJECT_LOOKUP_STEPS, 1);
    }
    return ret;
}

static void* getObjectPtr(NVDriver *drv, ObjectType type, VAGenericID id) {
    if (id != VA_INVALID_ID) {
        Object o = getObject(drv, type, id);
        if (o != NULL) {
            return o->obj;
        }
    }
    return NULL;
}

static void deleteObject(NVDriver *drv, VAGenericID id) {
    if (id == VA_INVALID_ID) {
        return;
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    Object object = nvdObjectTableRemove(&drv->objects, id);
    pthread_mutex_unlock(&drv->objectCreationMutex);
    free(object);
}

static void setSurfaceResolving(NVSurface *surface, bool resolving);
static void waitSurfaceResolved(NVSurface *surface);
static void releaseBufferMemory(NVDriver *drv, NVBuffer *buffer);

static bool destroyContext(NVDriver *drv, NVContext *nvCtx) {
    // Join on whether the resolve thread was actually started, not on decoder !=
    // NULL: a decode context whose decoder was destroyed and failed to recreate
    // (recreateDecoderForSurface) leaves decoder == NULL with the resolve thread
    // still running. Guarding on decoder would skip the join and free nvCtx out
    // from under the live thread. VideoProc contexts never start the thread, so
    // the flag stays false for them.
    if (nvCtx->resolveThreadStarted) {
        LOG("Signaling resolve thread to exit");
        void *cancelled[RESOLVE_QUEUE_CAPACITY] = { 0 };
        const size_t cancelledCount = resolveQueueCancel(&nvCtx->resolveQueue,
                                                         cancelled,
                                                         ARRAY_SIZE(cancelled));
        for (size_t i = 0; i < cancelledCount; i++) {
            setSurfaceResolving((NVSurface *) cancelled[i], false);
        }
        LOG("Waiting for resolve thread to exit");
        int ret = pthread_join(nvCtx->resolveThread, NULL);
        LOG("Finished waiting for resolve thread with %d", ret);
        if (ret != 0) {
            // The worker may still reference nvCtx, its decoder, and queued
            // surfaces. Keep all of them alive if join did not prove exit.
            return false;
        }
        nvCtx->resolveThreadStarted = false;
    }
    if (nvCtx->decoder != NULL) {
        if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
            return false;
        }
        CUresult result = cv->cuvidDestroyDecoder(nvCtx->decoder);
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        if (result != CUDA_SUCCESS) {
            LOG("cuvidDestroyDecoder failed: %d", result);
            return false;
        }
        nvCtx->decoder = NULL;
    }

    if (nvCtx->codec != NULL && nvCtx->codec->destroy != NULL) {
        nvCtx->codec->destroy(nvCtx);
    }
    free(nvCtx->codecData);
    nvCtx->codecData = NULL;

    freeAppendableBuffer(&nvCtx->sliceOffsets);
    freeAppendableBuffer(&nvCtx->bitstreamBuffer);

    resolveQueueDestroy(&nvCtx->resolveQueue);
    if (nvCtx->surfaceCreationMutexInitialized) {
        pthread_mutex_destroy(&nvCtx->surfaceCreationMutex);
        nvCtx->surfaceCreationMutexInitialized = false;
    }

    return true;
}

static bool deleteAllContexts(NVDriver *drv) {
    for (;;) {
        Object contextObject = NULL;
        pthread_mutex_lock(&drv->objectCreationMutex);
        for (uint32_t i = 0; i < drv->objects.capacity; i++) {
            Object candidate = nvdObjectTableAt(&drv->objects, i);
            if (candidate == NULL) {
                continue;
            }
            if (candidate->type == OBJECT_TYPE_CONTEXT) {
                contextObject = candidate;
                break;
            }
        }
        pthread_mutex_unlock(&drv->objectCreationMutex);

        if (contextObject == NULL) {
            return true;
        }
        if (!destroyContext(drv, (NVContext *) contextObject->obj)) {
            return false;
        }
        deleteObject(drv, contextObject->id);
    }
}

static void deleteAllObjects(NVDriver *drv) {
    for (;;) {
        pthread_mutex_lock(&drv->objectCreationMutex);
        if (drv->objects.liveCount == 0) {
            pthread_mutex_unlock(&drv->objectCreationMutex);
            break;
        }
        Object object = NULL;
        for (uint32_t i = drv->objects.capacity; i-- > 0;) {
            object = nvdObjectTableAt(&drv->objects, i);
            if (object != NULL) {
                object = nvdObjectTableRemove(&drv->objects, object->id);
                break;
            }
        }
        pthread_mutex_unlock(&drv->objectCreationMutex);

        if (object == NULL) {
            break;
        }

        LOG("Found object %d of type %d", object->id, object->type);
        if (object->type == OBJECT_TYPE_BUFFER) {
            NVBuffer *buffer = object->obj;
            releaseBufferMemory(drv, buffer);
        } else if (object->type == OBJECT_TYPE_SURFACE) {
            NVSurface *surface = object->obj;
            if (surface->syncInitialized) {
                pthread_cond_destroy(&surface->cond);
                pthread_mutex_destroy(&surface->mutex);
                surface->syncInitialized = false;
            }
        }
        free(object);
    }
}

NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf) {
    Object obj = getObject(drv, OBJECT_TYPE_SURFACE, surf);
    if (obj != NULL) {
        NVSurface *suf = (NVSurface*) obj->obj;
        return suf;
    }
    return NULL;
}

int pictureIdxFromSurfaceId(NVDriver *drv, VASurfaceID surfId) {
    NVSurface *surf = nvSurfaceFromSurfaceId(drv, surfId);
    if (surf != NULL) {
        return surf->pictureIdx;
    }
    return -1;
}

static cudaVideoCodec vaToCuCodec(VAProfile profile) {
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        cudaVideoCodec cvc = c->computeCudaCodec(profile);
        if (cvc != cudaVideoCodec_NONE) {
            return cvc;
        }
    }

    return cudaVideoCodec_NONE;
}

static bool doesGPUSupportCodec(cudaVideoCodec codec, int bitDepth, cudaVideoChromaFormat chromaFormat, uint32_t *width, uint32_t *height)
{
    CUVIDDECODECAPS videoDecodeCaps = {
        .eCodecType      = codec,
        .eChromaFormat   = chromaFormat,
        .nBitDepthMinus8 = bitDepth - 8
    };

    CHECK_CUDA_RESULT_RETURN(cv->cuvidGetDecoderCaps(&videoDecodeCaps), false);

    if (width != NULL) {
        *width = videoDecodeCaps.nMaxWidth;
    }
    if (height != NULL) {
        *height = videoDecodeCaps.nMaxHeight;
    }
    return (videoDecodeCaps.bIsSupported == 1);
}

static void* resolveSurfaces(void *param) {
    NVContext *ctx = (NVContext*) param;
    NVDriver *drv = ctx->drv;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), NULL);

    if (CHECK_CUDA_RESULT(cu->cuStreamCreate(&ctx->resolveStream, CU_STREAM_NON_BLOCKING)) ||
        CHECK_CUDA_RESULT(cu->cuEventCreate(&ctx->resolveCompleteEvent,
                                            CU_EVENT_DISABLE_TIMING))) {
        if (ctx->resolveCompleteEvent != NULL) {
            CHECK_CUDA_RESULT(cu->cuEventDestroy(ctx->resolveCompleteEvent));
            ctx->resolveCompleteEvent = NULL;
        }
        if (ctx->resolveStream != NULL) {
            CHECK_CUDA_RESULT(cu->cuStreamDestroy(ctx->resolveStream));
            ctx->resolveStream = NULL;
        }
        LOG("Unable to create context resolve stream; using the legacy default stream");
    }

    LOG("[RT] Resolve thread for %p started", ctx);
    NVSurface *surface = NULL;
    while (resolveQueuePop(&ctx->resolveQueue, (void **) &surface)) {

        CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
        unsigned int pitch = 0;

        //map frame
        CUVIDPROCPARAMS procParams = {
            .progressive_frame = surface->progressiveFrame,
            .top_field_first = surface->topFieldFirst,
            .second_field = surface->secondField,
            .output_stream = ctx->resolveStream,
        };

        //LOG("Mapping surface %d", surface->pictureIdx);
        if (surface->decodeFailed || CHECK_CUDA_RESULT(cv->cuvidMapVideoFrame(ctx->decoder, surface->pictureIdx, &deviceMemory, &pitch, &procParams))) {
            setSurfaceResolving(surface, false);
            continue;
        }
        //LOG("Mapped surface %d to %p (%d)", surface->pictureIdx, (void*)deviceMemory, pitch);

        //update cuarray
        nvStatsIncrement(drv, NV_STAT_RESOLVE_FRAMES);
        if (!drv->backend->exportCudaPtr(drv, deviceMemory, surface, pitch,
                                         ctx->resolveStream,
                                         ctx->resolveCompleteEvent)) {
            // Backends normally complete the surface themselves on success.
            // Their failure paths are not uniform, so guarantee wakeup here.
            setSurfaceResolving(surface, false);
        }
        //LOG("Surface %d exported", surface->pictureIdx);
        //unmap frame

        CHECK_CUDA_RESULT(cv->cuvidUnmapVideoFrame(ctx->decoder, deviceMemory));
    }
    //release the decoder here to prevent multiple threads attempting it
    if (ctx->decoder != NULL) {
        CUresult result = cv->cuvidDestroyDecoder(ctx->decoder);
        ctx->decoder = NULL;
        if (result != CUDA_SUCCESS) {
            LOG("cuvidDestroyDecoder failed: %d", result);
        }
    }
    if (ctx->resolveStream != NULL) {
        CHECK_CUDA_RESULT(cu->cuStreamSynchronize(ctx->resolveStream));
    }
    if (ctx->resolveCompleteEvent != NULL) {
        CHECK_CUDA_RESULT(cu->cuEventDestroy(ctx->resolveCompleteEvent));
        ctx->resolveCompleteEvent = NULL;
    }
    if (ctx->resolveStream != NULL) {
        CHECK_CUDA_RESULT(cu->cuStreamDestroy(ctx->resolveStream));
        ctx->resolveStream = NULL;
    }
    CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
    LOG("[RT] Resolve thread for %p exiting", ctx);
    return NULL;
}


static VAStatus nvQueryConfigProfiles(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //now filter out the codecs we don't support
    for (int i = 0; i < drv->profileCount; i++) {
        profile_list[i] = drv->profiles[i];
    }

    *num_profiles = drv->profileCount;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigProfiles2(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    int profiles = 0;
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG2, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG2Simple;
        profile_list[profiles++] = VAProfileMPEG2Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG4, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG4Simple;
        profile_list[profiles++] = VAProfileMPEG4AdvancedSimple;
        profile_list[profiles++] = VAProfileMPEG4Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VC1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVC1Simple;
        profile_list[profiles++] = VAProfileVC1Main;
        profile_list[profiles++] = VAProfileVC1Advanced;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264Main;
        profile_list[profiles++] = VAProfileH264High;
        profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_JPEG, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileJPEGBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_SVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264StereoHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_MVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264MultiviewHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP8, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP8Version0_3;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile0; //color depth: 8 bit, 4:2:0
    }
    if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileAV1Profile0;
    }

    if (drv->supports16BitSurface) {
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain10;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain12;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile2; //color depth: 10–12 bit, 4:2:0
        }
    }

    if (drv->supports444Surface) {
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain444;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile1; //color depth: 8 bit, 4:2:2, 4:4:0, 4:4:4
        }
        if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileAV1Profile1;
        }

#if VA_CHECK_VERSION(1, 20, 0)
        if (drv->supports16BitSurface) {
            if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_10;
            }
            if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_12;
            }
            if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileVP9Profile3; //color depth: 10–12 bit, 4:2:2, 4:4:0, 4:4:4
            }
        }
#endif
    }

    // Nvidia decoder doesn't support 422 chroma layout
#if 0
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_10;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_12;
    }
#endif

    //now filter out the codecs we don't support
    for (int i = 0; i < profiles; i++) {
        if (vaToCuCodec(profile_list[i]) == cudaVideoCodec_NONE) {
            for (int x = i; x < profiles-1; x++) {
                profile_list[x] = profile_list[x+1];
            }
            profiles--;
            i--;
        }
    }

    profile_list[profiles++] = VAProfileNone;

    *num_profiles = profiles;

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,	/* out */
        int *num_entrypoints			/* out */
    )
{
    if (profile == VAProfileNone) {
        entrypoint_list[0] = VAEntrypointVideoProc;
        *num_entrypoints = 1;
        return VA_STATUS_SUCCESS;
    }

    entrypoint_list[0] = VAEntrypointVLD;
    *num_entrypoints = 1;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,	/* in/out */
        int num_attribs
    )
{
    if (entrypoint == VAEntrypointVideoProc && profile == VAProfileNone) {
        NVDriver *drv = (NVDriver*) ctx->pDriverData;
        for (int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == VAConfigAttribRTFormat) {
                attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
                if (drv->supports16BitSurface) {
                    attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
                }
            } else {
                LOG("unhandled vpp config attribute: %d", attrib_list[i].type);
            }
        }
        return VA_STATUS_SUCCESS;
    }

    if (entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (vaToCuCodec(profile) == cudaVideoCodec_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    //LOG("Got here with profile: %d == %d", profile, vaToCuCodec(profile));

    for (int i = 0; i < num_attribs; i++)
    {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
        {
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            switch (profile) {
            case VAProfileHEVCMain12:
            case VAProfileVP9Profile2:
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_12;
                // Fall-through
            case VAProfileHEVCMain10:
            case VAProfileAV1Profile0:
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
                break;

            case VAProfileHEVCMain444_12:
            case VAProfileVP9Profile3:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444_12 | VA_RT_FORMAT_YUV420_12;
                // Fall-through
            case VAProfileHEVCMain444_10:
            case VAProfileAV1Profile1:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV420_10;
                // Fall-through
            case VAProfileHEVCMain444:
            case VAProfileVP9Profile1:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444;
                break;
            default:
                //do nothing
                break;
            }

            if (!drv->supports16BitSurface) {
                attrib_list[i].value &= ~(VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
            }
            if (!drv->supports444Surface) {
                attrib_list[i].value &= ~(VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
            }
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureWidth)
        {
            doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, &attrib_list[i].value, NULL);
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureHeight)
        {
            doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, NULL, &attrib_list[i].value);
        }
        else
        {
            LOG("unhandled config attribute: %d", attrib_list[i].type);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateConfig(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (config_id == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    *config_id = VA_INVALID_ID;
    //LOG("got profile: %d with %d attributes", profile, num_attribs);
    cudaVideoCodec cudaCodec = vaToCuCodec(profile);

    if (entrypoint == VAEntrypointVideoProc && profile == VAProfileNone) {
        Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
        if (obj == NULL) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        NVConfig *cfg = (NVConfig*) obj->obj;
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->cudaCodec = cudaVideoCodec_NONE;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->bitDepth = 8;
        *config_id = obj->id;
        return VA_STATUS_SUCCESS;
    }

    if (cudaCodec == cudaVideoCodec_NONE) {
        //we don't support this yet
        LOG("Profile not supported: %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (entrypoint != VAEntrypointVLD) {
        LOG("Entrypoint not supported: %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    if (obj == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    NVConfig *cfg = (NVConfig*) obj->obj;
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;

    //this will contain all the attributes the client cares about
    //for (int i = 0; i < num_attribs; i++) {
    //  LOG("got config attrib: %d %d %d", i, attrib_list[i].type, attrib_list[i].value);
    //}

    cfg->cudaCodec = cudaCodec;
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->bitDepth = 8;
    uint32_t rtFormat = 0;
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat) {
            rtFormat = attrib_list[i].value;
            break;
        }
    }

    if (drv->supports16BitSurface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->bitDepth = 10;
            break;
        case VAProfileHEVCMain12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->bitDepth = 12;
            break;
        case VAProfileVP9Profile2:
        case VAProfileAV1Profile0:
            // If the user provides an RTFormat, we can use that to identify which decoder
            // configuration is appropriate. If a format is not required here, the caller
            // must pass render targets to createContext so we can use those to establish
            // the surface format and bit depth.
            if (rtFormat != 0) {
                if ((rtFormat & VA_RT_FORMAT_YUV420_12) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 12;
                } else if ((rtFormat & VA_RT_FORMAT_YUV420_10) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
                }
            } else {
                if (cfg->profile == VAProfileVP9Profile2) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
                } else {
                    LOG("Unable to determine surface type for VP9/AV1 codec due to no RTFormat specified.");
                }
            }
        default:
            break;
        }
    }

    if (drv->supports444Surface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain444:
        case VAProfileVP9Profile1:
        case VAProfileAV1Profile1:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
            break;
        default:
            break;
        }
    }

    if (drv->supports444Surface && drv->supports16BitSurface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain444_10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 10;
            break;
        case VAProfileHEVCMain444_12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 12;
            break;
        case VAProfileVP9Profile3:
        case VAProfileAV1Profile1:
            // If the user provides an RTFormat, we can use that to identify which decoder
            // configuration is appropriate. If a format is not required here, the caller
            // must pass render targets to createContext so we can use those to establish
            // the surface format and bit depth.
            if (rtFormat != 0) {
                if ((rtFormat & VA_RT_FORMAT_YUV444_12) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 12;
                } else if ((rtFormat & VA_RT_FORMAT_YUV444_10) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                } else if ((rtFormat & VA_RT_FORMAT_YUV444) != 0) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 8;
                }
            } else {
                if (cfg->profile == VAProfileVP9Profile3) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                }
            }
        default:
            break;
        }
    }

    *config_id = obj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyConfig(
        VADriverContextP ctx,
        VAConfigID config_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    deleteObject(drv, config_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id,
        VAProfile *profile,		/* out */
        VAEntrypoint *entrypoint, 	/* out */
        VAConfigAttrib *attrib_list,	/* out */
        int *num_attribs		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config_id);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->entrypoint == VAEntrypointVideoProc) {
        *profile = cfg->profile;
        *entrypoint = cfg->entrypoint;
        int i = 0;
        attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
        attrib_list[i].type = VAConfigAttribRTFormat;
        if (drv->supports16BitSurface) {
            attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
        }
        i++;
        *num_attribs = i;
        return VA_STATUS_SUCCESS;
    }

    *profile = cfg->profile;
    *entrypoint = cfg->entrypoint;
    int i = 0;
    attrib_list[i].value = VA_RT_FORMAT_YUV420;
    attrib_list[i].type = VAConfigAttribRTFormat;
    switch (cfg->profile) {
    case VAProfileHEVCMain12:
    case VAProfileVP9Profile2:
        attrib_list[i].value |= VA_RT_FORMAT_YUV420_12;
        // Fall-through
    case VAProfileHEVCMain10:
    case VAProfileAV1Profile0:
        attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
        break;

    case VAProfileHEVCMain444_12:
    case VAProfileVP9Profile3:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444_12 | VA_RT_FORMAT_YUV420_12;
        // Fall-through
    case VAProfileHEVCMain444_10:
    case VAProfileAV1Profile1:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV420_10;
        // Fall-through
    case VAProfileHEVCMain444:
    case VAProfileVP9Profile1:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444;
        break;
    default:
        //do nothing
        break;
    }

    if (!drv->supports16BitSurface) {
        attrib_list[i].value &= ~(VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
    }
    if (!drv->supports444Surface) {
        attrib_list[i].value &= ~(VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
    }

    i++;
    *num_attribs = i;
    return VA_STATUS_SUCCESS;
}

static void releaseExternalCudaBuffers(NVDriver *drv, BackingImage *img) {
    for (uint32_t i = 0; i < img->numObjects; i++) {
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
}

static bool importedObjectsAreLinear(const BackingImage *img) {
    if (img->numObjects == 0) {
        return false;
    }
    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->mods[i] != DRM_FORMAT_MOD_LINEAR) {
            return false;
        }
    }
    return true;
}

static bool importExternalBuffersToCuda(NVDriver *drv, BackingImage *img) {
    if (!importedObjectsAreLinear(img) || drv->cu->cuExternalMemoryGetMappedBuffer == NULL) {
        return false;
    }

    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->fds[i] < 0 || img->objectSize[i] == 0) {
            goto fail;
        }
        int importFd = dup(img->fds[i]);
        if (importFd < 0) {
            goto fail;
        }
        // On desktop CUDA, generic dma-buf import is not available through the
        // external-memory API. OPAQUE_FD still permits zero-copy for handles
        // produced by a CUDA-compatible external API; ordinary dma-bufs fall
        // through to the mmap-backed direct D-to-H path below.
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
            .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
            .handle.fd = importFd,
            .flags = 0,
            .size = img->objectSize[i]
        };
        CUresult importResult = drv->cu->cuImportExternalMemory(&img->externalObjectMems[i], &extMemDesc);
        if (importResult != CUDA_SUCCESS) {
            LOG_DEBUG("CUDA mapped-buffer import unavailable for external object %u (error %d)",
                      i, importResult);
            close(importFd);
            goto fail;
        }
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC bufferDesc = {
            .offset = 0,
            .size = img->objectSize[i],
            .flags = 0
        };
        CUresult mapResult = drv->cu->cuExternalMemoryGetMappedBuffer(&img->externalDevicePtrs[i],
                                                                      img->externalObjectMems[i],
                                                                      &bufferDesc);
        if (mapResult != CUDA_SUCCESS) {
            LOG_DEBUG("CUDA mapped-buffer mapping unavailable for external object %u (error %d)",
                      i, mapResult);
            goto fail;
        }
        img->externalDeviceSize[i] = img->objectSize[i];
    }

    char fourcc[5];
    LOG_DEBUG("Imported linear external %s surface as %u CUDA mapped buffer object(s)",
              fourccString((uint32_t) img->fourcc, fourcc), img->numObjects);
    return true;

fail:
    releaseExternalCudaBuffers(drv, img);
    return false;
}

static bool mapExternalBacking(BackingImage *img) {
    if (!importedObjectsAreLinear(img)) {
        return false;
    }
    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->fds[i] < 0 || img->objectSize[i] == 0 || img->objectSize[i] > SIZE_MAX) {
            goto fail;
        }
        img->externalMappings[i] = mmap(NULL, (size_t) img->objectSize[i], PROT_READ | PROT_WRITE,
                                        MAP_SHARED, img->fds[i], 0);
        if (img->externalMappings[i] == MAP_FAILED) {
            img->externalMappings[i] = NULL;
            goto fail;
        }
        img->externalMappingSize[i] = img->objectSize[i];
    }
    return true;

fail:
    for (uint32_t i = 0; i < img->numObjects; i++) {
        if (img->externalMappings[i] != NULL) {
            munmap(img->externalMappings[i], (size_t) img->externalMappingSize[i]);
            img->externalMappings[i] = NULL;
            img->externalMappingSize[i] = 0;
        }
    }
    return false;
}

static uint64_t importedFdSize(int fd) {
    struct stat statBuffer;
    if (fstat(fd, &statBuffer) == 0 && statBuffer.st_size > 0) {
        return (uint64_t) statBuffer.st_size;
    }
    const off_t original = lseek(fd, 0, SEEK_CUR);
    const off_t end = lseek(fd, 0, SEEK_END);
    if (original >= 0) {
        lseek(fd, original, SEEK_SET);
    }
    if (end > 0) {
        return (uint64_t) end;
    }
    return 0;
}

static bool validateImportedLayout(const BackingImage *img) {
    if (img->format <= NV_FORMAT_NONE || img->format > NV_FORMAT_ARGB ||
        img->numObjects == 0 || img->numObjects > NVD_MAX_IMPORTED_OBJECTS) {
        return false;
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[img->format];
    if (img->numPlanes != fmtInfo->numPlanes) {
        return false;
    }

    for (uint32_t i = 0; i < img->numPlanes; i++) {
        const NVFormatPlane *plane = &fmtInfo->plane[i];
        const uint32_t objectIndex = img->planeObjectIndex[i];
        const uint64_t widthBytes = (uint64_t) (img->width >> plane->ss.x) *
                                    fmtInfo->bppc * plane->channelCount;
        const uint64_t height = img->height >> plane->ss.y;
        if (objectIndex >= img->numObjects || img->objectSize[objectIndex] == 0 ||
            img->strides[i] <= 0 || (uint64_t) img->strides[i] < widthBytes || height == 0) {
            return false;
        }
        const uint64_t lastRow = height - 1;
        if (lastRow > (UINT64_MAX - (uint64_t) img->offsets[i] - widthBytes) /
                          (uint64_t) img->strides[i]) {
            return false;
        }
        const uint64_t end = (uint64_t) img->offsets[i] +
                             lastRow * (uint64_t) img->strides[i] + widthBytes;
        if (end > img->objectSize[objectIndex]) {
            return false;
        }
    }
    return true;
}

static BackingImage *createImportedBackingImageImpl(NVDriver *drv, const ImportedSurface *imported, uint32_t width, uint32_t height) {
    NVFormat format = nvFormatFromSurfaceFourcc(imported->pixelFormat);
    if (format == NV_FORMAT_NONE || imported->numPlanes == 0 || imported->numObjects == 0 ||
        imported->objects[0].fd < 0) {
        return NULL;
    }

    BackingImage *img = calloc(1, sizeof(BackingImage));
    if (img == NULL) {
        return NULL;
    }
    for (int i = 0; i < 4; i++) {
        img->fds[i] = -1;
    }

    img->isExternalBuffer = true;
    img->format = format;
    img->width = width;
    img->height = height;
    img->fourcc = (int) imported->pixelFormat;
    img->numObjects = imported->numObjects;
    img->numPlanes = imported->numPlanes;
    img->isSingleBuffer = imported->numObjects == 1;
    for (uint32_t i = 0; i < imported->numObjects; i++) {
        img->fds[i] = dup(imported->objects[i].fd);
        if (img->fds[i] < 0) {
            goto fail;
        }
        cacheBackingImageFdStat(img, (int) i);
        const uint64_t actualSize = importedFdSize(img->fds[i]);
        img->objectSize[i] = imported->objects[i].size != 0 ? imported->objects[i].size : actualSize;
        if (img->objectSize[i] == 0 || (actualSize != 0 && img->objectSize[i] > actualSize) ||
            UINT64_MAX - img->totalSize < img->objectSize[i]) {
            goto fail;
        }
        img->totalSize += img->objectSize[i];
        img->mods[i] = imported->objects[i].modifier;
    }
    for (uint32_t i = 0; i < imported->numPlanes; i++) {
        if (imported->planes[i].objectIndex >= img->numObjects ||
            imported->planes[i].pitch > INT_MAX || imported->planes[i].offset > INT_MAX) {
            goto fail;
        }
        img->planeObjectIndex[i] = imported->planes[i].objectIndex;
        img->strides[i] = (int) imported->planes[i].pitch;
        img->offsets[i] = (int) imported->planes[i].offset;
        const uint32_t objectIndex = imported->planes[i].objectIndex;
        img->size[i] = img->objectSize[objectIndex] > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t) img->objectSize[objectIndex];
    }
    if (!validateImportedLayout(img)) {
        LOG("Invalid imported surface object/plane layout");
        goto fail;
    }

    BackingImage *existing = retainBackingImageByFd(drv, imported->objects[0].fd, format, width, height);
    if (existing != NULL && !backingImageMatchesImportedLayout(existing, img)) {
        atomic_fetch_sub(&existing->borrowCount, 1);
        existing = NULL;
    }
    if (existing != NULL) {
        nvStatsIncrement(drv, NV_STAT_BACKING_CACHE_HITS);
        nvBackingImageCopyColorMetadata(img, existing);
        const NVFormatInfo *fmtInfo = &formatsInfo[format];
        for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
            img->arrays[i] = existing->arrays[i];
            img->strides[i] = existing->strides[i];
            img->offsets[i] = existing->offsets[i];
            img->size[i] = existing->size[i];
            img->mods[i] = existing->mods[i];
        }
        img->totalSize = existing->totalSize != 0 ? existing->totalSize : existing->size[0];
        img->borrowedCudaResources = true;
        img->borrowedBackingImage = existing;
        LOG_DEBUG("Imported surface reused backing image color metadata: imported=%p backing=%p color_standard=%s(%d) full_range=%d",
            img, existing, nvColorStandardName(img->colorStandard), img->colorStandard, img->colorRangeFull);
        return img;
    }

    if (importExternalBuffersToCuda(drv, img)) {
        return img;
    }

    if (mapExternalBacking(img)) {
        return img;
    }

    char fourcc[5];
    LOG("Unable to import external surface %s: only validated linear layouts are supported",
        fourccString(imported->pixelFormat, fourcc));

fail:
    releaseExternalCudaBuffers(drv, img);
    for (int i = 0; i < 4; i++) {
        if (img->externalMappings[i] != NULL) {
            munmap(img->externalMappings[i], (size_t) img->externalMappingSize[i]);
        }
        if (img->fds[i] >= 0) {
            close(img->fds[i]);
        }
    }
    free(img);
    return NULL;
}

static BackingImage *createImportedBackingImage(NVDriver *drv, const ImportedSurface *imported, uint32_t width, uint32_t height) {
    const uint64_t start = nvStatsTimestamp(drv);
    BackingImage *img = createImportedBackingImageImpl(drv, imported, width, height);
    const uint64_t end = nvStatsTimestamp(drv);
    nvStatsIncrement(drv, NV_STAT_BACKING_ALLOC_COUNT);
    if (start != 0 && end >= start) {
        nvStatsAdd(drv, NV_STAT_BACKING_ALLOC_NS, end - start);
    }
    return img;
}

static BackingImage *surfaceSyncBackingImage(NVSurface *surface) {
    if (surface == NULL || surface->backingImage == NULL) {
        return NULL;
    }
    if (surface->backingImage->borrowedBackingImage != NULL) {
        return surface->backingImage->borrowedBackingImage;
    }
    return surface->backingImage;
}

static void setSurfaceBackingImageResolving(NVSurface *surface, bool resolving) {
    BackingImage *img = surfaceSyncBackingImage(surface);
    if (img == NULL || !img->syncInitialized) {
        return;
    }

    pthread_mutex_lock(&img->mutex);
    img->resolving = resolving;
    if (!resolving) {
        pthread_cond_broadcast(&img->cond);
    }
    pthread_mutex_unlock(&img->mutex);
}

static void setSurfaceResolving(NVSurface *surface, bool resolving) {
    if (surface == NULL) {
        return;
    }

    setSurfaceBackingImageResolving(surface, resolving);

    pthread_mutex_lock(&surface->mutex);
    surface->resolving = resolving ? 1 : 0;
    if (!resolving) {
        pthread_cond_broadcast(&surface->cond);
    }
    pthread_mutex_unlock(&surface->mutex);
}

static void waitSurfaceResolved(NVSurface *surface) {
    if (surface == NULL) {
        return;
    }

    pthread_mutex_lock(&surface->mutex);
    while (surface->resolving) {
        pthread_cond_wait(&surface->cond, &surface->mutex);
    }
    pthread_mutex_unlock(&surface->mutex);

    BackingImage *img = surfaceSyncBackingImage(surface);
    if (img == NULL || !img->syncInitialized) {
        return;
    }

    pthread_mutex_lock(&img->mutex);
    while (img->resolving) {
        pthread_cond_wait(&img->cond, &img->mutex);
    }
    pthread_mutex_unlock(&img->mutex);
}

static void destroySurfaceSynchronization(NVSurface *surface) {
    if (!surface->syncInitialized) {
        return;
    }
    pthread_cond_destroy(&surface->cond);
    pthread_mutex_destroy(&surface->mutex);
    surface->syncInitialized = false;
}

static bool initializeContextSynchronization(NVContext *nvCtx) {
    pthread_mutexattr_t attrib;
    if (pthread_mutexattr_init(&attrib) != 0) {
        return false;
    }
    if (pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE) != 0) {
        pthread_mutexattr_destroy(&attrib);
        return false;
    }
    int result = pthread_mutex_init(&nvCtx->surfaceCreationMutex, &attrib);
    pthread_mutexattr_destroy(&attrib);
    if (result != 0) {
        return false;
    }
    nvCtx->surfaceCreationMutexInitialized = true;

    if (!resolveQueueInit(&nvCtx->resolveQueue)) {
        pthread_mutex_destroy(&nvCtx->surfaceCreationMutex);
        nvCtx->surfaceCreationMutexInitialized = false;
        return false;
    }
    if (nvCtx->drv->statsEnabled) {
        ResolveQueueTelemetry telemetry = {
            .depth = &nvCtx->drv->stats[NV_STAT_RESOLVE_QUEUE_DEPTH],
            .highWater = &nvCtx->drv->stats[NV_STAT_RESOLVE_QUEUE_HIGH_WATER],
            .fullWaits = &nvCtx->drv->stats[NV_STAT_RESOLVE_QUEUE_FULL_WAITS],
            .waitNs = &nvCtx->drv->stats[NV_STAT_RESOLVE_QUEUE_WAIT_NS],
        };
        resolveQueueSetTelemetry(&nvCtx->resolveQueue, telemetry);
    }
    return true;
}

static VAStatus nvCreateSurfaces2(
            VADriverContextP    ctx,
            unsigned int        format,
            unsigned int        width,
            unsigned int        height,
            VASurfaceID        *surfaces,
            unsigned int        num_surfaces,
            VASurfaceAttrib    *attrib_list,
            unsigned int        num_attribs
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (surfaces == NULL && num_surfaces != 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < num_surfaces; i++) {
        surfaces[i] = VA_INVALID_ID;
    }
    ImportedSurface imported;
    parseSurfaceImportAttributes(attrib_list, num_attribs, &imported);
    if (imported.requested && !imported.valid) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    const bool importSurface = imported.valid;
    if (importSurface && imported.legacyPrime && imported.numLegacyBuffers != num_surfaces) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (importSurface && !imported.legacyPrime && num_surfaces != 1) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    uint32_t surfaceFourcc = importSurface ? imported.pixelFormat : 0;

    cudaVideoSurfaceFormat nvFormat;
    cudaVideoChromaFormat chromaFormat;
    int bitdepth;

    switch (format)
    {
    case VA_RT_FORMAT_YUV420:
        nvFormat = cudaVideoSurfaceFormat_NV12;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 8;
        break;
    case VA_RT_FORMAT_YUV420_10:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 10;
        break;
    case VA_RT_FORMAT_YUV420_12:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 12;
        break;
    case VA_RT_FORMAT_YUV444:
        nvFormat = cudaVideoSurfaceFormat_YUV444;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 8;
        break;
    case VA_RT_FORMAT_YUV444_10:
        nvFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 10;
        break;
    case VA_RT_FORMAT_YUV444_12:
        nvFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 12;
        break;
    case VA_RT_FORMAT_RGB32:
        nvFormat = cudaVideoSurfaceFormat_NV12;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 8;
        break;
    
    default:
        LOG("Unknown format: %X", format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    // If there is subsampled chroma make the size a multple of it
    switch(chromaFormat) {
        case cudaVideoChromaFormat_422:
            width = ROUND_UP(width, 2);
            break;
        case cudaVideoChromaFormat_420:
            width = ROUND_UP(width, 2);
            height = ROUND_UP(height, 2);
            break;
        default:
            // no change needed
            break;
    }
    if (importSurface && (imported.width != width || imported.height != height)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    for (uint32_t i = 0; i < num_surfaces; i++) {
        Object surfaceObject = allocateObject(drv, OBJECT_TYPE_SURFACE, sizeof(NVSurface));
        if (surfaceObject == NULL) {
            for (uint32_t j = 0; j < i; j++) {
                NVSurface *rollbackSurface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, surfaces[j]);
                if (rollbackSurface != NULL && rollbackSurface->backingImage != NULL) {
                    drv->backend->detachBackingImageFromSurface(drv, rollbackSurface);
                }
                if (rollbackSurface != NULL) {
                    destroySurfaceSynchronization(rollbackSurface);
                }
                deleteObject(drv, surfaces[j]);
                surfaces[j] = VA_INVALID_ID;
            }
            CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surfaces[i] = surfaceObject->id;
        NVSurface *suf = (NVSurface*) surfaceObject->obj;
        suf->width = width;
        suf->height = height;
        suf->format = nvFormat;
        suf->fourcc = surfaceFourcc != 0 ? (int) surfaceFourcc : (format == VA_RT_FORMAT_RGB32 ? VA_FOURCC_ARGB : 0);
        suf->pictureIdx = -1;
        suf->bitDepth = bitdepth;
        suf->context = NULL;
        suf->chromaFormat = chromaFormat;
        if (pthread_mutex_init(&suf->mutex, NULL) != 0) {
            deleteObject(drv, surfaceObject->id);
            surfaces[i] = VA_INVALID_ID;
            for (uint32_t j = 0; j < i; j++) {
                NVSurface *rollbackSurface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, surfaces[j]);
                if (rollbackSurface != NULL && rollbackSurface->backingImage != NULL) {
                    drv->backend->detachBackingImageFromSurface(drv, rollbackSurface);
                }
                if (rollbackSurface != NULL) {
                    destroySurfaceSynchronization(rollbackSurface);
                }
                deleteObject(drv, surfaces[j]);
                surfaces[j] = VA_INVALID_ID;
            }
            CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        if (pthread_cond_init(&suf->cond, NULL) != 0) {
            pthread_mutex_destroy(&suf->mutex);
            deleteObject(drv, surfaceObject->id);
            surfaces[i] = VA_INVALID_ID;
            for (uint32_t j = 0; j < i; j++) {
                NVSurface *rollbackSurface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, surfaces[j]);
                if (rollbackSurface != NULL && rollbackSurface->backingImage != NULL) {
                    drv->backend->detachBackingImageFromSurface(drv, rollbackSurface);
                }
                if (rollbackSurface != NULL) {
                    destroySurfaceSynchronization(rollbackSurface);
                }
                deleteObject(drv, surfaces[j]);
                surfaces[j] = VA_INVALID_ID;
            }
            CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        suf->syncInitialized = true;

        if (importSurface) {
            ImportedSurface selectedImport;
            if (!importedSurfaceSelectIndex(&imported, i, num_surfaces, &selectedImport)) {
                selectedImport.valid = false;
            }
            BackingImage *img = selectedImport.valid
                ? createImportedBackingImage(drv, &selectedImport, width, height)
                : NULL;
            if (img == NULL) {
                // Roll back every surface object allocated in this call, including
                // the current one, so a failed import doesn't leak them. The
                // earlier surfaces (j < i) already have a BackingImage attached;
                // detach it first so its fds/mmap/CUDA external memory are freed
                // and any borrowCount taken on a cached image is released --
                // deleteObject only frees the NVSurface struct itself.
                for (uint32_t j = 0; j <= i; j++) {
                    NVSurface *rollbackSurface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surfaces[j]);
                    if (rollbackSurface != NULL && rollbackSurface->backingImage != NULL) {
                        drv->backend->detachBackingImageFromSurface(drv, rollbackSurface);
                    }
                    if (rollbackSurface != NULL) {
                        destroySurfaceSynchronization(rollbackSurface);
                    }
                    deleteObject(drv, surfaces[j]);
                    surfaces[j] = VA_INVALID_ID;
                }
                CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
            suf->backingImage = img;
            img->surface = suf;
            nvStatsBackingImageCreated(drv, img, true);
            nvSurfaceCopyColorMetadataFromBackingImage(suf, img);
            char fourcc[5];
            LOG_DEBUG("Importing surface %ux%u, format %X/%s (%p) color_standard=%s(%d) full_range=%d",
                width, height, format, fourccString(surfaceFourcc, fourcc), suf,
                nvColorStandardName(suf->colorStandard), suf->colorStandard, suf->colorRangeFull);
        } else {
            LOG_DEBUG("Creating surface %ux%u, format %X (%p)", width, height, format, suf);
        }
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surfaces		/* out */
    )
{
    return nvCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}


static VAStatus nvDestroySurfaces(
        VADriverContextP ctx,
        VASurfaceID *surface_list,
        int num_surfaces
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    for (int i = 0; i < num_surfaces; i++) {
        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_list[i]);
        if (!surface) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        LOG_DEBUG("Destroying surface %d (%p)", surface->pictureIdx, surface);

        waitSurfaceResolved(surface);
        drv->backend->detachBackingImageFromSurface(drv, surface);
        destroySurfaceSynchronization(surface);
        deleteObject(drv, surface_list[i]);
    }

    return VA_STATUS_SUCCESS;
}

static uint32_t defaultDecodeSurfaceCount(cudaVideoCodec codec) {
    switch (codec) {
    case cudaVideoCodec_H264:
    case cudaVideoCodec_HEVC:
        return 20;
    case cudaVideoCodec_VP9:
    case cudaVideoCodec_AV1:
        return 10;
    case cudaVideoCodec_JPEG:
        return 2;
    case cudaVideoCodec_MPEG1:
    case cudaVideoCodec_MPEG2:
    case cudaVideoCodec_MPEG4:
    case cudaVideoCodec_VC1:
    case cudaVideoCodec_VP8:
    default:
        return 8;
    }
}

static uint32_t selectContextDecodeSurfaceCount(NVContext *nvCtx) {
    NVDriver *drv = nvCtx->drv;
    const uint32_t automatic = nvdSelectDecodeSurfaceCount(
        defaultDecodeSurfaceCount(nvCtx->cudaCodec), nvCtx->decodeSurfaceReferenceHint,
        nvCtx->clientRenderTargetCount, 0, drv->decodeSurfacesMinimum,
        drv->decodeSurfacesMaximum);
    nvStatsSet(drv, NV_STAT_DECODE_SURFACES_AUTO_CANDIDATE, automatic);
    if (drv->decodeSurfacesAuto) {
        return automatic;
    }
    const uint32_t overrideCount = drv->decodeSurfacesOverride != 0
        ? drv->decodeSurfacesOverride : 32;
    return nvdSelectDecodeSurfaceCount(automatic, 0, 0, overrideCount,
                                       drv->decodeSurfacesMinimum,
                                       drv->decodeSurfacesMaximum);
}

static VAStatus createDecoderForContext(NVContext *nvCtx) {
    if (nvCtx->decoder != NULL) {
        return VA_STATUS_SUCCESS;
    }

    nvCtx->surfaceCount = (int) selectContextDecodeSurfaceCount(nvCtx);
    if (nvCtx->currentPictureId > nvCtx->surfaceCount) {
        LOG("Selected %d decode surfaces after %d picture indices were assigned",
            nvCtx->surfaceCount, nvCtx->currentPictureId);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    int displayAreaWidth = nvCtx->width;
    int displayAreaHeight = nvCtx->height;
    switch (nvCtx->decoderChromaFormat) {
    case cudaVideoChromaFormat_422:
        displayAreaWidth = ROUND_UP(displayAreaWidth, 2);
        break;
    case cudaVideoChromaFormat_420:
        displayAreaWidth = ROUND_UP(displayAreaWidth, 2);
        displayAreaHeight = ROUND_UP(displayAreaHeight, 2);
        break;
    default:
        break;
    }

    CUVIDDECODECREATEINFO info = {
        .CodecType = nvCtx->cudaCodec,
        .ulCreationFlags = cudaVideoCreate_PreferCUVID,
        .display_area.right = displayAreaWidth,
        .display_area.bottom = displayAreaHeight,
        .ChromaFormat = nvCtx->decoderChromaFormat,
        .OutputFormat = nvCtx->decoderSurfaceFormat,
        .bitDepthMinus8 = nvCtx->decoderBitDepth - 8,
        .DeinterlaceMode = cudaVideoDeinterlaceMode_Weave,
        .ulNumOutputSurfaces = 1,
        .ulNumDecodeSurfaces = (unsigned long) nvCtx->surfaceCount,
    };
    info.ulWidth = info.ulMaxWidth = info.ulTargetWidth = nvCtx->width;
    info.ulHeight = info.ulMaxHeight = info.ulTargetHeight = nvCtx->height;

    NVDriver *drv = nvCtx->drv;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    CUvideodecoder decoder = NULL;
    CUresult result = cv->cuvidCreateDecoder(&decoder, &info);
    CUresult popResult = cu->cuCtxPopCurrent(NULL);
    if (result != CUDA_SUCCESS || popResult != CUDA_SUCCESS) {
        CHECK_CUDA_RESULT(result);
        CHECK_CUDA_RESULT(popResult);
        if (decoder != NULL) {
            CHECK_CUDA_RESULT(cv->cuvidDestroyDecoder(decoder));
        }
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    nvCtx->decoder = decoder;
    nvStatsIncrement(drv, NV_STAT_DECODER_CREATES);
    nvStatsSet(drv, NV_STAT_DECODE_SURFACES_SELECTED, (uint64_t) nvCtx->surfaceCount);
    nvStatsSet(drv, NV_STAT_DECODE_SURFACES_LEGACY, 32);
    LOG("Created decoder lazily with %d surfaces (legacy fallback: 32, refs hint: %u, client targets: %u)",
        nvCtx->surfaceCount, nvCtx->decodeSurfaceReferenceHint, nvCtx->clientRenderTargetCount);
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (context == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    *context = VA_INVALID_ID;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config_id);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->entrypoint == VAEntrypointVideoProc) {
        Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
        if (contextObj == NULL) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        LOG("Creating VideoProc context id: %d", contextObj->id);

        NVContext *nvCtx = (NVContext*) contextObj->obj;
        nvCtx->drv = drv;
        nvCtx->decoder = NULL;
        nvCtx->profile = cfg->profile;
        nvCtx->entrypoint = cfg->entrypoint;
        nvCtx->width = picture_width;
        nvCtx->height = picture_height;
        nvCtx->codec = NULL;

        if (!initializeContextSynchronization(nvCtx)) {
            deleteObject(drv, contextObj->id);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        *context = contextObj->id;
        return VA_STATUS_SUCCESS;
    }

    LOG("Creating context with %d render targets, at %dx%d", num_render_targets, picture_width, picture_height);

    //find the codec they've selected
    const NVCodec *selectedCodec = NULL;
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        for (int i = 0; i < c->supportedProfileCount; i++) {
            if (c->supportedProfiles[i] == cfg->profile) {
                selectedCodec = c;
                break;
            }
        }
    }
    if (selectedCodec == NULL) {
        LOG("Unable to find codec for profile: %d", cfg->profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (num_render_targets) {
        // Update the decoder configuration to match the passed in surfaces.
        NVSurface *surface = (NVSurface *) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_targets[0]);
        if (!surface) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        cfg->surfaceFormat = surface->format;
        cfg->chromaFormat = surface->chromaFormat;
        cfg->bitDepth = surface->bitDepth;
    }

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    if (contextObj == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("Creating lazy decoder context id: %d", contextObj->id);

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->decoder = NULL;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
    nvCtx->codec = selectedCodec;
    nvCtx->cudaCodec = cfg->cudaCodec;
    nvCtx->decoderSurfaceFormat = cfg->surfaceFormat;
    nvCtx->decoderChromaFormat = cfg->chromaFormat;
    nvCtx->decoderBitDepth = cfg->bitDepth;
    nvCtx->surfaceCount = 0;
    nvCtx->clientRenderTargetCount = num_render_targets > 0 ? (uint32_t) num_render_targets : 0;
    nvCtx->firstKeyframeValid = false;
    
    if (!initializeContextSynchronization(nvCtx)) {
        destroyContext(drv, nvCtx);
        deleteObject(drv, contextObj->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    int err = pthread_create(&nvCtx->resolveThread, NULL, &resolveSurfaces, nvCtx);
    if (err != 0) {
        LOG("Unable to create resolve thread: %d", err);
        destroyContext(drv, nvCtx);
        deleteObject(drv, contextObj->id);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    nvCtx->resolveThreadStarted = true;

    *context = contextObj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyContext(
        VADriverContextP ctx,
        VAContextID context)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Destroying context: %d", context);

    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VAStatus ret = VA_STATUS_SUCCESS;

    if (!destroyContext(drv, nvCtx)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    deleteObject(drv, context);

    return ret;
}

static VAStatus recreateDecoderForSurface(NVContext *nvCtx, NVSurface *surface) {
    const bool matches = nvCtx->decoderSurfaceFormat == surface->format &&
                         nvCtx->decoderChromaFormat == surface->chromaFormat &&
                         nvCtx->decoderBitDepth == surface->bitDepth;
    if (nvCtx->decoder != NULL) {
        if (matches) {
            return VA_STATUS_SUCCESS;
        }
        LOG("Decoder/surface format mismatch after lazy decoder creation: decoder format=%d chroma=%d bitDepth=%d, surface format=%d chroma=%d bitDepth=%d",
            nvCtx->decoderSurfaceFormat, nvCtx->decoderChromaFormat, nvCtx->decoderBitDepth,
            surface->format, surface->chromaFormat, surface->bitDepth);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    nvCtx->decoderSurfaceFormat = surface->format;
    nvCtx->decoderChromaFormat = surface->chromaFormat;
    nvCtx->decoderBitDepth = surface->bitDepth;
    return VA_STATUS_SUCCESS;
}

static bool isValidBufferType(VABufferType type) {
    // Codec handlers use this enum as an array index, so reject values outside
    // the libva-defined range before they can reach a dispatch table.
    return (int) type >= 0 && type < VABufferTypeMax;
}

static bool checkedMultiplySize(size_t left, size_t right, size_t *result) {
    // Keep the overflow check in size_t, which is also the type consumed by
    // the allocator and memcpy below.
    if (left != 0 && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool checkedAddSize(size_t left, size_t right, size_t *result) {
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static const size_t bufferPoolClassSizes[NVD_BUFFER_POOL_CLASS_COUNT] = {
    4U * 1024U,
    8U * 1024U,
    16U * 1024U,
    32U * 1024U,
    64U * 1024U,
    128U * 1024U,
    256U * 1024U,
    512U * 1024U,
    1024U * 1024U,
    2U * 1024U * 1024U,
    4U * 1024U * 1024U,
};

static int bufferPoolClassForSize(size_t size) {
    for (int i = 0; i < NVD_BUFFER_POOL_CLASS_COUNT; i++) {
        if (size <= bufferPoolClassSizes[i]) {
            return i;
        }
    }
    return -1;
}

static bool allocateBufferMemory(NVDriver *drv, NVBuffer *buffer, size_t size) {
    buffer->ptr = NULL;
    buffer->capacity = 0;
    buffer->poolClass = -1;
    if (size == 0) {
        return true;
    }

    const int poolClass = bufferPoolClassForSize(size);
    const size_t capacity = poolClass >= 0 ? bufferPoolClassSizes[poolClass] : size;
    if (poolClass >= 0) {
        pthread_mutex_lock(&drv->bufferPoolMutex);
        NVBufferPoolBlock *block = drv->bufferPool[poolClass];
        if (block != NULL) {
            drv->bufferPool[poolClass] = block->next;
            drv->bufferPoolCounts[poolClass]--;
            drv->bufferPoolBytes -= capacity;
            buffer->ptr = block;
        }
        nvStatsSet(drv, NV_STAT_BUFFER_POOL_RETAINED_BYTES, drv->bufferPoolBytes);
        pthread_mutex_unlock(&drv->bufferPoolMutex);
    }

    if (buffer->ptr != NULL) {
        nvStatsIncrement(drv, NV_STAT_BUFFER_POOL_HITS);
    } else {
        nvStatsIncrement(drv, NV_STAT_BUFFER_POOL_MISSES);
        if (posix_memalign(&buffer->ptr, 16, capacity) != 0) {
            buffer->ptr = NULL;
            return false;
        }
    }
    buffer->capacity = capacity;
    buffer->poolClass = (int8_t) poolClass;
    nvStatsAdd(drv, NV_STAT_BUFFER_REQUESTED_BYTES, size);
    nvStatsAdd(drv, NV_STAT_BUFFER_CAPACITY_BYTES, capacity);
    if (capacity > size) {
        nvStatsAdd(drv, NV_STAT_BUFFER_INTERNAL_FRAGMENTATION_BYTES, capacity - size);
    }
    return true;
}

static void releaseBufferMemory(NVDriver *drv, NVBuffer *buffer) {
    if (buffer == NULL || buffer->ptr == NULL) {
        return;
    }
    bool pooled = false;
    if (buffer->poolClass >= 0 && buffer->poolClass < NVD_BUFFER_POOL_CLASS_COUNT) {
        const int poolClass = buffer->poolClass;
        pthread_mutex_lock(&drv->bufferPoolMutex);
        if (drv->bufferPoolCounts[poolClass] < 16 &&
            buffer->capacity <= drv->bufferPoolMaxBytes &&
            drv->bufferPoolBytes <= drv->bufferPoolMaxBytes - buffer->capacity) {
            NVBufferPoolBlock *block = buffer->ptr;
            block->next = drv->bufferPool[poolClass];
            drv->bufferPool[poolClass] = block;
            drv->bufferPoolCounts[poolClass]++;
            drv->bufferPoolBytes += buffer->capacity;
            pooled = true;
        }
        nvStatsSet(drv, NV_STAT_BUFFER_POOL_RETAINED_BYTES, drv->bufferPoolBytes);
        pthread_mutex_unlock(&drv->bufferPoolMutex);
    }
    if (!pooled) {
        free(buffer->ptr);
    }
    buffer->ptr = NULL;
    buffer->capacity = 0;
    buffer->poolClass = -1;
}

static void destroyBufferPool(NVDriver *drv) {
    if (!drv->bufferPoolMutexInitialized) {
        return;
    }
    pthread_mutex_lock(&drv->bufferPoolMutex);
    for (int i = 0; i < NVD_BUFFER_POOL_CLASS_COUNT; i++) {
        NVBufferPoolBlock *block = drv->bufferPool[i];
        while (block != NULL) {
            NVBufferPoolBlock *next = block->next;
            free(block);
            block = next;
        }
        drv->bufferPool[i] = NULL;
        drv->bufferPoolCounts[i] = 0;
    }
    drv->bufferPoolBytes = 0;
    nvStatsSet(drv, NV_STAT_BUFFER_POOL_RETAINED_BYTES, 0);
    pthread_mutex_unlock(&drv->bufferPoolMutex);
}

static VAStatus nvCreateBuffer(
        VADriverContextP ctx,
        VAContextID context,		/* in */
        VABufferType type,		/* in */
        unsigned int size,		/* in */
        unsigned int num_elements,	/* in */
        void *data,			/* in */
        VABufferID *buf_id
    )
{
    //LOG("got buffer %p, type %x, size %u, elements %u", data, type, size, num_elements);
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    if (buf_id == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    // Every failure path must leave the caller without a usable object ID.
    *buf_id = VA_INVALID_ID;

    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!isValidBufferType(type)) {
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    //HACK: This is an awful hack to support VP8 videos when running within FFMPEG.
    //VA-API doesn't pass enough information for NVDEC to work with, but the information is there
    //just before the start of the buffer that was passed to us.
    size_t elementSize = size;
    size_t offset = 0;
    if (nvCtx->profile == VAProfileVP8Version0_3 && type == VASliceDataBufferType) {
        offset = ((uintptr_t) data) & 0xf;
        data = ((char *) data) - offset;
        // The VP8 prefix becomes part of each copied element, so account for
        // it before multiplying by the element count.
        if (elementSize > SIZE_MAX - offset) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        elementSize += offset;
    }

    size_t bufferSize = 0;
    if (!checkedMultiplySize(elementSize, num_elements, &bufferSize)) {
        LOG("Buffer size overflow: %zu bytes x %u elements", elementSize, num_elements);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    Object bufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    if (bufferObject == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = bufferSize;
    buf->offset = offset;

    if (!allocateBufferMemory(drv, buf, buf->size)) {
        LOG("Unable to allocate buffer of %zu bytes", buf->size);
        // allocateObject has already published the ID; remove it before
        // returning so failed allocations cannot leave zombie VA objects.
        deleteObject(drv, bufferObject->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (data != NULL && buf->size != 0)
    {
        memcpy(buf->ptr, data, buf->size);
    } else if (buf->size != 0) {
        memset(buf->ptr, 0, buf->size);
    }

    *buf_id = bufferObject->id;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvBufferSetNumElements(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        unsigned int num_elements	/* in */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        void **pbuf         /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    *pbuf = buf->ptr;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvUnmapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id	/* in */
    )
{
    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyBuffer(
        VADriverContextP ctx,
        VABufferID buffer_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffer_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    releaseBufferMemory(drv, buf);

    deleteObject(drv, buffer_id);

    return VA_STATUS_SUCCESS;
}

static uint8_t clampU8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t) value;
}

typedef struct {
    int vToR;
    int uToG;
    int vToG;
    int uToB;
} ColorMatrix;

typedef struct {
    int sampleShift;
    int yScale;
    int yOffset;
    int uvOffset;
    int rounding;
    int valueShift;
} VideoProcSampleInfo;

static const ColorMatrix kLimitedRangeColorMatrices[] = {
    { 409, 100, 208, 516 },
    { 459,  55, 136, 541 },
    { 430,  48, 167, 548 },
};

static const ColorMatrix kFullRangeColorMatrices[] = {
    { 359,  88, 183, 454 },
    { 403,  48, 120, 475 },
    { 377,  42, 146, 482 },
};

static const char *colorMatrixName(const ColorMatrix *matrix) {
    if (matrix == &kLimitedRangeColorMatrices[0] || matrix == &kFullRangeColorMatrices[0]) {
        return "BT.601";
    }
    if (matrix == &kLimitedRangeColorMatrices[1] || matrix == &kFullRangeColorMatrices[1]) {
        return "BT.709";
    }
    if (matrix == &kLimitedRangeColorMatrices[2] || matrix == &kFullRangeColorMatrices[2]) {
        return "BT.2020";
    }
    return "unknown";
}

static const char *sourceRangeName(uint8_t range) {
    switch (range) {
    case VA_SOURCE_RANGE_FULL:
        return "full";
    case VA_SOURCE_RANGE_REDUCED:
        return "limited";
    case VA_SOURCE_RANGE_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *effectiveRangeName(bool fullRange) {
    return fullRange ? "full" : "limited";
}

VAProcColorStandardType nvColorStandardFromMatrixCoefficients(uint8_t matrixCoefficients) {
    switch (matrixCoefficients) {
    case 1:
        return VAProcColorStandardBT709;
    case 4:
        return VAProcColorStandardBT470M;
    case 5:
        return VAProcColorStandardBT470BG;
    case 6:
        return VAProcColorStandardSMPTE170M;
    case 7:
        return VAProcColorStandardSMPTE240M;
    case 9:
        return VAProcColorStandardBT2020;
    default:
        return VAProcColorStandardNone;
    }
}

void nvSurfaceResetColorMetadata(NVSurface *surface) {
    if (surface == NULL) {
        return;
    }

    surface->colorStandard = VAProcColorStandardNone;
    surface->colorRangeFull = false;
}

void nvSurfaceSetColorMetadata(NVSurface *surface, VAProcColorStandardType colorStandard, bool colorRangeFull) {
    if (surface == NULL) {
        return;
    }

    surface->colorStandard = colorStandard;
    surface->colorRangeFull = colorRangeFull;
}

void nvSurfaceCopyColorMetadata(NVSurface *dst, const NVSurface *src) {
    if (dst == NULL || src == NULL) {
        return;
    }

    dst->colorStandard = src->colorStandard;
    dst->colorRangeFull = src->colorRangeFull;
}

static BackingImage *metadataBackingImage(BackingImage *img) {
    if (img != NULL && img->borrowedBackingImage != NULL) {
        return img->borrowedBackingImage;
    }
    return img;
}

static const BackingImage *constMetadataBackingImage(const BackingImage *img) {
    if (img != NULL && img->borrowedBackingImage != NULL) {
        return img->borrowedBackingImage;
    }
    return img;
}

void nvSurfaceCopyColorMetadataFromBackingImage(NVSurface *surface, const BackingImage *img) {
    const BackingImage *metadataImg = constMetadataBackingImage(img);
    if (surface == NULL || metadataImg == NULL) {
        return;
    }

    surface->colorStandard = metadataImg->colorStandard;
    surface->colorRangeFull = metadataImg->colorRangeFull;
}

void nvBackingImageStoreSurfaceColorMetadata(BackingImage *img, const NVSurface *surface) {
    BackingImage *metadataImg = metadataBackingImage(img);
    if (metadataImg == NULL || surface == NULL) {
        return;
    }

    metadataImg->colorStandard = surface->colorStandard;
    metadataImg->colorRangeFull = surface->colorRangeFull;
}

void nvBackingImageCopyColorMetadata(BackingImage *dst, const BackingImage *src) {
    const BackingImage *metadataSrc = constMetadataBackingImage(src);
    if (dst == NULL || metadataSrc == NULL) {
        return;
    }

    dst->colorStandard = metadataSrc->colorStandard;
    dst->colorRangeFull = metadataSrc->colorRangeFull;
}

const char *nvColorStandardName(VAProcColorStandardType colorStandard) {
    switch (colorStandard) {
    case VAProcColorStandardNone:
        return "None";
    case VAProcColorStandardBT601:
        return "BT.601";
    case VAProcColorStandardBT709:
        return "BT.709";
    case VAProcColorStandardBT470M:
        return "BT.470M";
    case VAProcColorStandardBT470BG:
        return "BT.470BG";
    case VAProcColorStandardSMPTE170M:
        return "SMPTE170M";
    case VAProcColorStandardSMPTE240M:
        return "SMPTE240M";
    case VAProcColorStandardGenericFilm:
        return "GenericFilm";
    case VAProcColorStandardSRGB:
        return "sRGB";
    case VAProcColorStandardSTRGB:
        return "stRGB";
    case VAProcColorStandardXVYCC601:
        return "xvYCC601";
    case VAProcColorStandardXVYCC709:
        return "xvYCC709";
    case VAProcColorStandardBT2020:
        return "BT.2020";
    case VAProcColorStandardExplicit:
        return "Explicit";
    default:
        return "unknown";
    }
}

static const ColorMatrix *colorMatrixForStandard(VAProcColorStandardType colorStandard, uint32_t width, bool fullRange) {
    const ColorMatrix *matrices = fullRange ? kFullRangeColorMatrices : kLimitedRangeColorMatrices;

    switch (colorStandard) {
    case VAProcColorStandardBT601:
    case VAProcColorStandardSMPTE170M:
    case VAProcColorStandardBT470M:
    case VAProcColorStandardBT470BG:
        return &matrices[0];
    case VAProcColorStandardBT709:
    case VAProcColorStandardSMPTE240M:
        return &matrices[1];
    case VAProcColorStandardBT2020:
        return &matrices[2];
    case VAProcColorStandardNone:
    default:
        return width >= 1280 ? &matrices[1] : &matrices[0];
    }
}

static VAProcColorStandardType effectiveSurfaceColorStandard(const NVSurface *src, const VAProcPipelineParameterBuffer *pipeline) {
    if (pipeline->surface_color_standard == VAProcColorStandardExplicit) {
        return nvColorStandardFromMatrixCoefficients(pipeline->input_color_properties.matrix_coefficients);
    }
    if (pipeline->surface_color_standard != VAProcColorStandardNone) {
        return pipeline->surface_color_standard;
    }
    if (src != NULL && src->colorStandard != VAProcColorStandardNone) {
        return src->colorStandard;
    }
    return VAProcColorStandardNone;
}

static bool effectiveSurfaceColorRangeFull(const NVSurface *src, const VAProcPipelineParameterBuffer *pipeline) {
    if (pipeline->input_color_properties.color_range == VA_SOURCE_RANGE_FULL) {
        return true;
    }
    if (pipeline->input_color_properties.color_range == VA_SOURCE_RANGE_REDUCED) {
        return false;
    }
    return src != NULL && src->colorRangeFull;
}

static VideoProcSampleInfo videoProcSampleInfoForFormat(NVFormat format, bool fullRange) {
    const int yScale = fullRange ? 256 : 298;
    switch (format) {
    case NV_FORMAT_P010:
        return (VideoProcSampleInfo) { 6, yScale, fullRange ? 0 : 64, 512, 512, 10 };
    case NV_FORMAT_P012:
        return (VideoProcSampleInfo) { 4, yScale, fullRange ? 0 : 256, 2048, 2048, 12 };
    case NV_FORMAT_NV12:
    default:
        return (VideoProcSampleInfo) { 0, yScale, fullRange ? 0 : 16, 128, 128, 8 };
    }
}

static bool loadVideoProcKernel(NVDriver *drv, bool is16Bit) {
    if (is16Bit) {
        static bool loggedP010KernelFailure = false;

        if (drv->p010ToArgbKernel != NULL) {
            return true;
        }
        if (drv->videoProcKernelP010Failed) {
            return false;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuModuleLoadData(&drv->videoProcModuleP010, p010ToArgbPtx)) ||
            CHECK_CUDA_RESULT(drv->cu->cuModuleGetFunction(&drv->p010ToArgbKernel, drv->videoProcModuleP010, "p010_to_argb"))) {
            if (drv->videoProcModuleP010 != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuModuleUnload(drv->videoProcModuleP010));
                drv->videoProcModuleP010 = NULL;
            }
            drv->p010ToArgbKernel = NULL;
            drv->videoProcKernelP010Failed = true;
            if (!loggedP010KernelFailure) {
                LOG("CUDA P010 VideoProc kernel unavailable, using CPU fallback");
                loggedP010KernelFailure = true;
            }
            return false;
        }

        return true;
    } else {
        static bool loggedNV12KernelFailure = false;

        if (drv->nv12ToArgbKernel != NULL) {
            return true;
        }
        if (drv->videoProcKernelFailed) {
            return false;
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuModuleLoadData(&drv->videoProcModule, nv12ToArgbPtx)) ||
            CHECK_CUDA_RESULT(drv->cu->cuModuleGetFunction(&drv->nv12ToArgbKernel, drv->videoProcModule, "nv12_to_argb"))) {
            if (drv->videoProcModule != NULL) {
                CHECK_CUDA_RESULT(drv->cu->cuModuleUnload(drv->videoProcModule));
                drv->videoProcModule = NULL;
            }
            drv->nv12ToArgbKernel = NULL;
            drv->videoProcKernelFailed = true;
            if (!loggedNV12KernelFailure) {
                LOG("CUDA NV12 VideoProc kernel unavailable, using CPU fallback");
                loggedNV12KernelFailure = true;
            }
            return false;
        }

        return true;
    }
}

static bool loadVideoProcArrayKernel(NVDriver *drv) {
    if (drv->arrayToArgbKernel != NULL) {
        return true;
    }
    if (drv->videoProcArrayKernelFailed) {
        return false;
    }
    if (CHECK_CUDA_RESULT(drv->cu->cuModuleLoadData(&drv->videoProcArrayModule, arrayToArgbPtx)) ||
        CHECK_CUDA_RESULT(drv->cu->cuModuleGetFunction(&drv->arrayToArgbKernel,
                                                       drv->videoProcArrayModule,
                                                       "array_to_argb"))) {
        if (drv->videoProcArrayModule != NULL) {
            CHECK_CUDA_RESULT(drv->cu->cuModuleUnload(drv->videoProcArrayModule));
            drv->videoProcArrayModule = NULL;
        }
        drv->arrayToArgbKernel = NULL;
        drv->videoProcArrayKernelFailed = true;
        LOG("Direct CUDA-array VideoProc kernel unavailable; retaining staged fallback");
        return false;
    }
    return true;
}

static bool ensureVideoProcStream(NVDriver *drv) {
    if (drv->videoProcStream == NULL &&
        CHECK_CUDA_RESULT(drv->cu->cuStreamCreate(&drv->videoProcStream, CU_STREAM_NON_BLOCKING))) {
        return false;
    }
    if (drv->videoProcEvent == NULL &&
        CHECK_CUDA_RESULT(drv->cu->cuEventCreate(&drv->videoProcEvent, CU_EVENT_DISABLE_TIMING))) {
        CHECK_CUDA_RESULT(drv->cu->cuStreamDestroy(drv->videoProcStream));
        drv->videoProcStream = NULL;
        return false;
    }
    return true;
}

static bool finishVideoProcStream(NVDriver *drv) {
    return !CHECK_CUDA_RESULT(drv->cu->cuEventRecord(drv->videoProcEvent, drv->videoProcStream)) &&
           !CHECK_CUDA_RESULT(drv->cu->cuEventSynchronize(drv->videoProcEvent));
}

static bool loadSurfaceObjectFunctions(NVDriver *drv) {
    if (drv->surfaceFunctionsLoaded) {
        return drv->cuSurfObjectCreate != NULL && drv->cuSurfObjectDestroy != NULL;
    }
    drv->surfaceFunctionsLoaded = true;
    drv->cuSurfObjectCreate = (NVCuSurfObjectCreate *) dlsym(drv->cu->lib, "cuSurfObjectCreate");
    drv->cuSurfObjectDestroy = (NVCuSurfObjectDestroy *) dlsym(drv->cu->lib, "cuSurfObjectDestroy");
    return drv->cuSurfObjectCreate != NULL && drv->cuSurfObjectDestroy != NULL;
}

static bool createVideoProcTexture(NVDriver *drv, CUarray array, CUtexObject *texture) {
    CUDA_RESOURCE_DESC resource = {
        .resType = CU_RESOURCE_TYPE_ARRAY,
        .res.array.hArray = array,
    };
    CUDA_TEXTURE_DESC descriptor = {
        .addressMode = { CU_TR_ADDRESS_MODE_CLAMP, CU_TR_ADDRESS_MODE_CLAMP, CU_TR_ADDRESS_MODE_CLAMP },
        .filterMode = CU_TR_FILTER_MODE_POINT,
        .flags = CU_TRSF_READ_AS_INTEGER,
    };
    return drv->cu->cuTexObjectCreate(texture, &resource, &descriptor, NULL) == CUDA_SUCCESS;
}

static bool createVideoProcSurface(NVDriver *drv, CUarray array, NVCUsurfObject *surface) {
    if (!loadSurfaceObjectFunctions(drv)) {
        return false;
    }
    CUDA_RESOURCE_DESC resource = {
        .resType = CU_RESOURCE_TYPE_ARRAY,
        .res.array.hArray = array,
    };
    return drv->cuSurfObjectCreate(surface, &resource) == CUDA_SUCCESS;
}

static BackingImage *videoProcResourceOwner(BackingImage *img) {
    return img != NULL && img->borrowedBackingImage != NULL
        ? img->borrowedBackingImage : img;
}

static bool getCachedVideoProcTexture(NVDriver *drv, BackingImage *img,
                                      uint32_t plane, CUtexObject *texture) {
    BackingImage *owner = videoProcResourceOwner(img);
    if (owner == NULL || plane >= 3 || owner->arrays[plane] == NULL) {
        return false;
    }
    if (owner->cachedVideoProcTextures[plane] == 0) {
        if (!createVideoProcTexture(drv, owner->arrays[plane],
                                    &owner->cachedVideoProcTextures[plane])) {
            return false;
        }
        nvStatsIncrement(drv, NV_STAT_VIDEOPROC_TEXTURE_CREATES);
    }
    *texture = owner->cachedVideoProcTextures[plane];
    return true;
}

static bool getCachedVideoProcSurface(NVDriver *drv, BackingImage *img,
                                      uint32_t plane, NVCUsurfObject *surface) {
    BackingImage *owner = videoProcResourceOwner(img);
    if (owner == NULL || plane >= 3 || owner->arrays[plane] == NULL) {
        return false;
    }
    if (owner->cachedVideoProcSurfaces[plane] == 0) {
        if (!createVideoProcSurface(drv, owner->arrays[plane],
                                    &owner->cachedVideoProcSurfaces[plane])) {
            return false;
        }
        nvStatsIncrement(drv, NV_STAT_VIDEOPROC_SURFACE_CREATES);
    }
    *surface = owner->cachedVideoProcSurfaces[plane];
    return true;
}

static uint32_t rgbOrderForFourcc(uint32_t fourcc) {
    switch (fourcc) {
    case VA_FOURCC_RGBA:
    case VA_FOURCC_RGBX:
        return 1;
    case VA_FOURCC_ARGB:
    case VA_FOURCC_XRGB:
        return 2;
    case VA_FOURCC_ABGR:
    case VA_FOURCC_XBGR:
        return 3;
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
    default:
        return 0;
    }
}

static size_t roundVideoProcBufferSize(size_t requiredSize) {
    const size_t blockSize = 1024 * 1024;
    if (requiredSize > SIZE_MAX - (blockSize - 1)) {
        return requiredSize;
    }
    return (requiredSize + blockSize - 1) & ~(blockSize - 1);
}

static void updateVideoProcScratchStats(NVDriver *drv) {
    const uint64_t gpuBytes = drv->videoProcYBufferSize + drv->videoProcUVBufferSize +
                              drv->videoProcArgbBufferSize;
    const uint64_t cpuBytes = drv->cpuVideoProcYBufferSize + drv->cpuVideoProcUVBufferSize +
                              drv->cpuVideoProcArgbBufferSize;
    nvStatsSet(drv, NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES, gpuBytes);
    nvStatsSetMax(drv, NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES_PEAK, gpuBytes);
    nvStatsSet(drv, NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES, cpuBytes);
    nvStatsSetMax(drv, NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES_PEAK, cpuBytes);
    nvStatsUpdateMemoryEstimates(drv);
}

static bool ensureVideoProcBuffer(NVDriver *drv, CUdeviceptr *buffer, size_t *bufferSize, size_t requiredSize) {
    if (*bufferSize >= requiredSize && *buffer != 0) {
        return true;
    }

    const size_t allocSize = roundVideoProcBufferSize(requiredSize);
    const uint64_t additionalBytes = allocSize > *bufferSize ? allocSize - *bufferSize : 0;
    if (drv->backend->pruneToMemoryBudget != NULL &&
        !drv->backend->pruneToMemoryBudget(drv, additionalBytes)) {
        return false;
    }
    const uint64_t currentBytes = drv->videoProcYBufferSize + drv->videoProcUVBufferSize +
                                  drv->videoProcArgbBufferSize;
    if (allocSize > drv->videoProcScratchMaxBytes ||
        currentBytes - *bufferSize > drv->videoProcScratchMaxBytes - allocSize) {
        return false;
    }

    CUdeviceptr newBuffer = 0;
    if (CHECK_CUDA_RESULT(drv->cu->cuMemAlloc(&newBuffer, allocSize))) {
        return false;
    }
    if (*buffer != 0 && CHECK_CUDA_RESULT(drv->cu->cuMemFree(*buffer))) {
        CHECK_CUDA_RESULT(drv->cu->cuMemFree(newBuffer));
        return false;
    }
    *buffer = newBuffer;
    *bufferSize = allocSize;
    updateVideoProcScratchStats(drv);
    return true;
}

static bool ensureCpuVideoProcBuffer(NVDriver *drv, void **buffer, size_t *bufferSize, size_t requiredSize) {
    if (*bufferSize >= requiredSize && *buffer != NULL) {
        return true;
    }

    const size_t allocSize = roundVideoProcBufferSize(requiredSize);
    const uint64_t currentBytes = drv->cpuVideoProcYBufferSize + drv->cpuVideoProcUVBufferSize +
                                  drv->cpuVideoProcArgbBufferSize;
    if (allocSize > drv->videoProcScratchMaxBytes ||
        currentBytes - *bufferSize > drv->videoProcScratchMaxBytes - allocSize) {
        return false;
    }
    void *newBuffer = realloc(*buffer, allocSize);
    if (newBuffer == NULL) {
        return false;
    }

    *buffer = newBuffer;
    *bufferSize = allocSize;
    updateVideoProcScratchStats(drv);
    return true;
}

static void trimVideoProcScratchLocked(NVDriver *drv, bool gpu, bool cpu) {
    if (gpu) {
        CUdeviceptr *buffers[] = {
            &drv->videoProcYBuffer,
            &drv->videoProcUVBuffer,
            &drv->videoProcArgbBuffer,
        };
        size_t *sizes[] = {
            &drv->videoProcYBufferSize,
            &drv->videoProcUVBufferSize,
            &drv->videoProcArgbBufferSize,
        };
        for (size_t i = 0; i < ARRAY_SIZE(buffers); i++) {
            if (*buffers[i] != 0 && !CHECK_CUDA_RESULT(drv->cu->cuMemFree(*buffers[i]))) {
                *buffers[i] = 0;
                *sizes[i] = 0;
            }
        }
    }
    if (cpu) {
        free(drv->cpuVideoProcYBuffer);
        free(drv->cpuVideoProcUVBuffer);
        free(drv->cpuVideoProcArgbBuffer);
        drv->cpuVideoProcYBuffer = NULL;
        drv->cpuVideoProcUVBuffer = NULL;
        drv->cpuVideoProcArgbBuffer = NULL;
        drv->cpuVideoProcYBufferSize = 0;
        drv->cpuVideoProcUVBufferSize = 0;
        drv->cpuVideoProcArgbBufferSize = 0;
    }
    updateVideoProcScratchStats(drv);
}

static void noteVideoProcCudaSuccessLocked(NVDriver *drv, bool directArrayPath) {
    if (drv->videoProcCudaFramesSinceCpuFallback < VIDEOPROC_SCRATCH_IDLE_FRAMES) {
        drv->videoProcCudaFramesSinceCpuFallback++;
    }
    if (drv->videoProcCudaFramesSinceCpuFallback >= VIDEOPROC_SCRATCH_IDLE_FRAMES) {
        const bool gpuAllocated = drv->videoProcYBuffer != 0 || drv->videoProcUVBuffer != 0 ||
                                  drv->videoProcArgbBuffer != 0;
        const bool cpuAllocated = drv->cpuVideoProcYBuffer != NULL ||
                                  drv->cpuVideoProcUVBuffer != NULL ||
                                  drv->cpuVideoProcArgbBuffer != NULL;
        if ((directArrayPath && gpuAllocated) || cpuAllocated) {
            trimVideoProcScratchLocked(drv, directArrayPath && gpuAllocated, cpuAllocated);
        }
    }
}

static void writeRgbPixel(uint8_t *dst, uint32_t fourcc, uint8_t r, uint8_t g, uint8_t b) {
    switch (fourcc) {
    case VA_FOURCC_RGBA:
    case VA_FOURCC_RGBX:
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = 255;
        break;
    case VA_FOURCC_BGRA:
    case VA_FOURCC_BGRX:
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = 255;
        break;
    case VA_FOURCC_ARGB:
    case VA_FOURCC_XRGB:
        dst[0] = 255;
        dst[1] = r;
        dst[2] = g;
        dst[3] = b;
        break;
    case VA_FOURCC_ABGR:
    case VA_FOURCC_XBGR:
        dst[0] = 255;
        dst[1] = b;
        dst[2] = g;
        dst[3] = r;
        break;
    default:
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = 255;
        break;
    }
}

// Fast path for the normal array-to-array VideoProc case. Source arrays are
// read directly through texture objects and the destination is written through
// a surface object, eliminating all three full-frame staging buffers/copies.
// A CUDA-mapped external RGB buffer uses the same kernel's linear destination.
static bool convertNV12ArrayToARGBCudaLocked(NVDriver *drv,
                                             BackingImage *srcImg,
                                             BackingImage *dstImg,
                                             uint32_t width,
                                             uint32_t height,
                                             const ColorMatrix *matrix,
                                             VideoProcSampleInfo sampleInfo) {
    if (srcImg->arrays[0] == NULL || srcImg->arrays[1] == NULL ||
        !ensureVideoProcStream(drv) || !loadVideoProcArrayKernel(drv)) {
        return false;
    }

    CUtexObject yTexture = 0;
    CUtexObject uvTexture = 0;
    NVCUsurfObject dstSurface = 0;
    uint64_t dstHandle = 0;
    uint32_t dstIsSurface = 0;
    uint32_t dstPitch = 0;
    bool submitted = false;
    BackingImage *srcOwner = videoProcResourceOwner(srcImg);
    BackingImage *dstOwner = videoProcResourceOwner(dstImg);
    const bool objectCreationNeeded =
        srcOwner == NULL || srcOwner->cachedVideoProcTextures[0] == 0 ||
        srcOwner->cachedVideoProcTextures[1] == 0 ||
        (!nvBackingImageHasExternalDeviceMemory(dstImg) &&
         (dstOwner == NULL || dstOwner->cachedVideoProcSurfaces[0] == 0));
    const uint64_t objectCreateStart = nvStatsTimestamp(drv);

    if (!getCachedVideoProcTexture(drv, srcImg, 0, &yTexture) ||
        !getCachedVideoProcTexture(drv, srcImg, 1, &uvTexture)) {
        goto done;
    }

    if (nvBackingImageHasExternalDeviceMemory(dstImg)) {
        dstHandle = (uint64_t) nvBackingImageDevicePlane(dstImg, 0);
        dstPitch = (uint32_t) dstImg->strides[0];
    } else if (dstImg->arrays[0] != NULL &&
               getCachedVideoProcSurface(drv, dstImg, 0, &dstSurface)) {
        dstHandle = dstSurface;
        dstIsSurface = 1;
    } else {
        goto done;
    }
    const uint64_t objectCreateEnd = nvStatsTimestamp(drv);
    if (objectCreationNeeded && objectCreateStart != 0 &&
        objectCreateEnd >= objectCreateStart) {
        nvStatsAdd(drv, NV_STAT_VIDEOPROC_OBJECT_CREATE_NS,
                   objectCreateEnd - objectCreateStart);
    }

    uint32_t order = rgbOrderForFourcc((uint32_t) dstImg->fourcc);
    uint32_t vToR = (uint32_t) matrix->vToR;
    uint32_t uToG = (uint32_t) matrix->uToG;
    uint32_t vToG = (uint32_t) matrix->vToG;
    uint32_t uToB = (uint32_t) matrix->uToB;
    uint32_t yScale = (uint32_t) sampleInfo.yScale;
    uint32_t sampleShift = (uint32_t) sampleInfo.sampleShift;
    uint32_t yOffset = (uint32_t) sampleInfo.yOffset;
    uint32_t uvOffset = (uint32_t) sampleInfo.uvOffset;
    uint32_t rounding = (uint32_t) sampleInfo.rounding;
    uint32_t valueShift = (uint32_t) sampleInfo.valueShift;
    void *args[] = {
        &yTexture,
        &uvTexture,
        &dstHandle,
        &dstIsSurface,
        &width,
        &height,
        &dstPitch,
        &order,
        &vToR,
        &uToG,
        &vToG,
        &uToB,
        &yScale,
        &sampleShift,
        &yOffset,
        &uvOffset,
        &rounding,
        &valueShift,
    };
    if (CHECK_CUDA_RESULT(drv->cu->cuLaunchKernel(drv->arrayToArgbKernel,
            (width + 15) / 16, (height + 15) / 16, 1,
            16, 16, 1, 0, drv->videoProcStream, args, NULL))) {
        goto done;
    }
    submitted = true;
    if (!finishVideoProcStream(drv)) {
        submitted = false;
        goto done;
    }
    nvStatsIncrement(drv, NV_STAT_VIDEOPROC_DIRECT_ARRAY_FRAMES);

done:
    if (submitted == false && drv->videoProcStream != NULL) {
        CHECK_CUDA_RESULT(drv->cu->cuStreamSynchronize(drv->videoProcStream));
    }
    return submitted;
}

static bool convertNV12ToARGBCuda(NVDriver *drv, BackingImage *srcImg, BackingImage *dstImg, uint32_t width, uint32_t height, bool is16Bit, const ColorMatrix *matrix, VideoProcSampleInfo sampleInfo) {
    if (srcImg->arrays[0] == NULL || srcImg->arrays[1] == NULL) {
        return false;
    }

    const size_t bpp = is16Bit ? 2 : 1;
    const size_t ySize = (size_t) width * height * bpp;
    const size_t uvHeight = (height + 1) / 2;
    const size_t uvSize = (size_t) width * uvHeight * bpp;
    const size_t argbSize = (size_t) width * height * 4;
    const bool externalDeviceDestination = nvBackingImageHasExternalDeviceMemory(dstImg);
    CUdeviceptr dstDevice = externalDeviceDestination ? nvBackingImageDevicePlane(dstImg, 0) : drv->videoProcArgbBuffer;
    uint32_t dstPitch = externalDeviceDestination ? (uint32_t) dstImg->strides[0] : width * 4;

    const uint64_t mutexWaitStart = nvStatsTimestamp(drv);
    pthread_mutex_lock(&drv->exportMutex);
    const uint64_t mutexWaitEnd = nvStatsTimestamp(drv);
    if (mutexWaitStart != 0 && mutexWaitEnd >= mutexWaitStart) {
        nvStatsAdd(drv, NV_STAT_VIDEOPROC_MUTEX_WAIT_NS,
                   mutexWaitEnd - mutexWaitStart);
    }
    if (convertNV12ArrayToARGBCudaLocked(drv, srcImg, dstImg, width, height, matrix, sampleInfo)) {
        noteVideoProcCudaSuccessLocked(drv, true);
        pthread_mutex_unlock(&drv->exportMutex);
        return true;
    }
    if (!loadVideoProcKernel(drv, is16Bit) ||
        !ensureVideoProcStream(drv) ||
        !ensureVideoProcBuffer(drv, &drv->videoProcYBuffer, &drv->videoProcYBufferSize, ySize) ||
        !ensureVideoProcBuffer(drv, &drv->videoProcUVBuffer, &drv->videoProcUVBufferSize, uvSize)) {
        goto fail;
    }
    if (!externalDeviceDestination &&
        !ensureVideoProcBuffer(drv, &drv->videoProcArgbBuffer, &drv->videoProcArgbBufferSize, argbSize)) {
        goto fail;
    }
    if (!externalDeviceDestination) {
        dstDevice = drv->videoProcArgbBuffer;
    }

    CUDA_MEMCPY2D yCpy = {
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = srcImg->arrays[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = drv->videoProcYBuffer,
        .dstPitch = width * bpp,
        .WidthInBytes = width * bpp,
        .Height = height
    };
    CUDA_MEMCPY2D uvCpy = {
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = srcImg->arrays[1],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = drv->videoProcUVBuffer,
        .dstPitch = width * bpp,
        .WidthInBytes = width * bpp,
        .Height = uvHeight
    };
    // Queue the fallback copies and kernel on the dedicated non-blocking stream.
    // The reusable completion event synchronizes once per finished frame and
    // avoids the legacy default stream's process-wide ordering rules.
    if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&yCpy, drv->videoProcStream))) {
        goto fail;
    }
    nvStatsAdd(drv, NV_STAT_DEVICE_COPY_BYTES, ySize);
    if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&uvCpy, drv->videoProcStream))) {
        goto fail;
    }
    nvStatsAdd(drv, NV_STAT_DEVICE_COPY_BYTES, uvSize);

    uint32_t yPitch = width * bpp;
    uint32_t uvPitch = width * bpp;
    uint32_t order = rgbOrderForFourcc((uint32_t) dstImg->fourcc);
    uint32_t vToR = (uint32_t) matrix->vToR;
    uint32_t uToG = (uint32_t) matrix->uToG;
    uint32_t vToG = (uint32_t) matrix->vToG;
    uint32_t uToB = (uint32_t) matrix->uToB;
    uint32_t yScale = (uint32_t) sampleInfo.yScale;
    uint32_t sampleShift = (uint32_t) sampleInfo.sampleShift;
    uint32_t yOffset = (uint32_t) sampleInfo.yOffset;
    uint32_t uvOffset = (uint32_t) sampleInfo.uvOffset;
    uint32_t rounding = (uint32_t) sampleInfo.rounding;
    uint32_t valueShift = (uint32_t) sampleInfo.valueShift;
    void *nv12Args[] = {
        &drv->videoProcYBuffer,
        &drv->videoProcUVBuffer,
        &dstDevice,
        &width,
        &height,
        &yPitch,
        &uvPitch,
        &dstPitch,
        &order,
        &vToR,
        &uToG,
        &vToG,
        &uToB,
        &yScale,
        &yOffset,
        &uvOffset,
        &rounding,
        &valueShift
    };
    void *p010Args[] = {
        &drv->videoProcYBuffer,
        &drv->videoProcUVBuffer,
        &dstDevice,
        &width,
        &height,
        &yPitch,
        &uvPitch,
        &dstPitch,
        &order,
        &vToR,
        &uToG,
        &vToG,
        &uToB,
        &yScale,
        &sampleShift,
        &yOffset,
        &uvOffset,
        &rounding,
        &valueShift
    };
    void **args = is16Bit ? p010Args : nv12Args;
    CUfunction kernel = is16Bit ? drv->p010ToArgbKernel : drv->nv12ToArgbKernel;
    if (CHECK_CUDA_RESULT(drv->cu->cuLaunchKernel(kernel,
            (width + 15) / 16, (height + 15) / 16, 1,
            16, 16, 1, 0, drv->videoProcStream, args, NULL))) {
        goto fail;
    }

    if (!externalDeviceDestination) {
        CUDA_MEMCPY2D argbCpy = {
            .srcMemoryType = CU_MEMORYTYPE_DEVICE,
            .srcDevice = drv->videoProcArgbBuffer,
            .srcPitch = width * 4,
            .dstMemoryType = CU_MEMORYTYPE_ARRAY,
            .dstArray = dstImg->arrays[0],
            .WidthInBytes = width * 4,
            .Height = height
        };
        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&argbCpy, drv->videoProcStream))) {
            goto fail;
        }
        nvStatsAdd(drv, NV_STAT_DEVICE_COPY_BYTES, argbSize);
    }
    if (!finishVideoProcStream(drv)) {
        goto fail;
    }

    noteVideoProcCudaSuccessLocked(drv, false);
    pthread_mutex_unlock(&drv->exportMutex);
    return true;

fail:
    if (drv->videoProcStream != NULL) {
        CHECK_CUDA_RESULT(drv->cu->cuStreamSynchronize(drv->videoProcStream));
    }
    pthread_mutex_unlock(&drv->exportMutex);
    return false;
}

static bool convertNV12ToARGB(NVDriver *drv, BackingImage *srcImg, BackingImage *dstImg, uint32_t width, uint32_t height, const ColorMatrix *matrix, VideoProcSampleInfo sampleInfo) {
    const bool is16Bit = srcImg->format == NV_FORMAT_P010 || srcImg->format == NV_FORMAT_P012;
    const char *formatName = is16Bit ? "P010/P012" : "NV12";

    const bool externalHostDestination = nvBackingImageHasExternalHostMemory(dstImg);
    const bool externalDeviceDestination = nvBackingImageHasExternalDeviceMemory(dstImg);
    if (!externalHostDestination || externalDeviceDestination) {
        if (convertNV12ToARGBCuda(drv, srcImg, dstImg, width, height, is16Bit, matrix, sampleInfo)) {
            nvStatsIncrement(drv, NV_STAT_VIDEOPROC_CUDA);
            static bool loggedCudaVideoProc[2] = { false, false };
            const int logIndex = is16Bit ? 1 : 0;
            if (!loggedCudaVideoProc[logIndex]) {
                LOG("Using CUDA %s to RGB VideoProc conversion", formatName);
                loggedCudaVideoProc[logIndex] = true;
            }
            return true;
        }
        nvStatsIncrement(drv, NV_STAT_VIDEOPROC_CUDA_FAILURES);
        static bool loggedCudaFallback[2] = { false, false };
        const int logIndex = is16Bit ? 1 : 0;
        if (!loggedCudaFallback[logIndex]) {
            LOG("CUDA %s to RGB conversion failed, falling back to CPU", formatName);
            loggedCudaFallback[logIndex] = true;
        }
    }
    nvStatsIncrement(drv, NV_STAT_VIDEOPROC_CPU_FALLBACK);

    const size_t bpp = is16Bit ? 2 : 1;
    const size_t ySize = (size_t) width * height * bpp;
    const size_t uvSize = (size_t) width * ((height + 1) / 2) * bpp;
    const size_t argbSize = (size_t) width * height * 4;
    const bool externalHostSource = nvBackingImageHasExternalHostMemory(srcImg);
    bool sourceAccessStarted = false;
    bool destinationAccessStarted = false;

    pthread_mutex_lock(&drv->exportMutex);
    drv->videoProcCudaFramesSinceCpuFallback = 0;

    if (externalHostSource) {
        sourceAccessStarted = nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        if (!sourceAccessStarted) {
            nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            goto fail;
        }
    }
    if (externalHostDestination) {
        destinationAccessStarted = nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        if (!destinationAccessStarted) {
            nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            goto fail;
        }
    }

    if (!ensureCpuVideoProcBuffer(drv, &drv->cpuVideoProcYBuffer, &drv->cpuVideoProcYBufferSize, ySize) ||
        !ensureCpuVideoProcBuffer(drv, &drv->cpuVideoProcUVBuffer, &drv->cpuVideoProcUVBufferSize, uvSize)) {
        goto fail;
    }
    if (!externalHostDestination &&
        !ensureCpuVideoProcBuffer(drv, &drv->cpuVideoProcArgbBuffer, &drv->cpuVideoProcArgbBufferSize, argbSize)) {
        goto fail;
    }

    uint8_t *yPlane = drv->cpuVideoProcYBuffer;
    uint8_t *uvPlane = drv->cpuVideoProcUVBuffer;
    uint8_t *argb = !externalHostDestination ? drv->cpuVideoProcArgbBuffer : NULL;

    if (externalHostSource) {
        const uint8_t *srcY = nvBackingImageHostPlane(srcImg, 0);
        const uint8_t *srcUV = nvBackingImageHostPlane(srcImg, 1);
        for (uint32_t y = 0; y < height; y++) {
            memcpy(yPlane + (size_t) y * width * bpp, srcY + (size_t) y * srcImg->strides[0], width * bpp);
        }
        for (uint32_t y = 0; y < (height + 1) / 2; y++) {
            memcpy(uvPlane + (size_t) y * width * bpp, srcUV + (size_t) y * srcImg->strides[1], width * bpp);
        }
        nvStatsAdd(drv, NV_STAT_HOST_COPY_BYTES, ySize + uvSize);
    } else {
        const bool externalDeviceSource = nvBackingImageHasExternalDeviceMemory(srcImg);
        CUDA_MEMCPY2D yCpy = {
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = yPlane,
            .dstPitch = width * bpp,
            .WidthInBytes = width * bpp,
            .Height = height
        };
        CUDA_MEMCPY2D uvCpy = {
            .dstMemoryType = CU_MEMORYTYPE_HOST,
            .dstHost = uvPlane,
            .dstPitch = width * bpp,
            .WidthInBytes = width * bpp,
            .Height = (height + 1) / 2
        };
        if (externalDeviceSource) {
            yCpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            yCpy.srcDevice = nvBackingImageDevicePlane(srcImg, 0);
            yCpy.srcPitch = (uint32_t) srcImg->strides[0];
            uvCpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            uvCpy.srcDevice = nvBackingImageDevicePlane(srcImg, 1);
            uvCpy.srcPitch = (uint32_t) srcImg->strides[1];
        } else {
            yCpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            yCpy.srcArray = srcImg->arrays[0];
            uvCpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            uvCpy.srcArray = srcImg->arrays[1];
        }

        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&yCpy))) {
            goto fail;
        }
        nvStatsAdd(drv, NV_STAT_HOST_COPY_BYTES, ySize);
        if (CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&uvCpy))) {
            goto fail;
        }
        nvStatsAdd(drv, NV_STAT_HOST_COPY_BYTES, uvSize);
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            int yy, u, v;
            if (is16Bit) {
                const uint16_t *yPlane16 = (const uint16_t*) yPlane;
                const uint16_t *uvPlane16 = (const uint16_t*) uvPlane;
                yy = yPlane16[(size_t) y * width + x] >> sampleInfo.sampleShift;
                const size_t uvIndex = (size_t) (y / 2) * width + (x & ~1u);
                u = uvPlane16[uvIndex] >> sampleInfo.sampleShift;
                v = uvPlane16[uvIndex + 1] >> sampleInfo.sampleShift;
            } else {
                yy = yPlane[(size_t) y * width + x];
                const size_t uvIndex = (size_t) (y / 2) * width + (x & ~1u);
                u = uvPlane[uvIndex];
                v = uvPlane[uvIndex + 1];
            }
            const int c = yy > sampleInfo.yOffset ? yy - sampleInfo.yOffset : 0;
            const int d = u - sampleInfo.uvOffset;
            const int e = v - sampleInfo.uvOffset;
            const uint8_t r = clampU8((sampleInfo.yScale * c + matrix->vToR * e + sampleInfo.rounding) >> sampleInfo.valueShift);
            const uint8_t g = clampU8((sampleInfo.yScale * c - matrix->uToG * d - matrix->vToG * e + sampleInfo.rounding) >> sampleInfo.valueShift);
            const uint8_t b = clampU8((sampleInfo.yScale * c + matrix->uToB * d + sampleInfo.rounding) >> sampleInfo.valueShift);
            uint8_t *row = externalHostDestination
                ? nvBackingImageHostPlane(dstImg, 0) + (size_t) y * dstImg->strides[0]
                : argb + (size_t) y * width * 4;
            writeRgbPixel(row + (size_t) x * 4, (uint32_t) dstImg->fourcc, r, g, b);
        }
    }

    if (externalHostDestination) {
        bool syncFailed = !nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        destinationAccessStarted = false;
        if (sourceAccessStarted) {
            syncFailed |= !nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            sourceAccessStarted = false;
        }
        pthread_mutex_unlock(&drv->exportMutex);
        return !syncFailed;
    }

    CUDA_MEMCPY2D argbCpy = {
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = argb,
        .srcPitch = width * 4,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = dstImg->arrays[0],
        .WidthInBytes = width * 4,
        .Height = height
    };
    bool failed = CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&argbCpy));
    if (!failed) {
        nvStatsAdd(drv, NV_STAT_HOST_COPY_BYTES, argbSize);
    }
    if (sourceAccessStarted) {
        failed |= !nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        sourceAccessStarted = false;
    }

    pthread_mutex_unlock(&drv->exportMutex);
    return !failed;

fail:
    if (destinationAccessStarted) {
        nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    }
    if (sourceAccessStarted) {
        nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }
    pthread_mutex_unlock(&drv->exportMutex);
    return false;
}

static bool copySurfaceBackingImage(NVDriver *drv, NVSurface *src, NVSurface *dst, const VAProcPipelineParameterBuffer *pipeline) {
    if (src == NULL || dst == NULL || pipeline == NULL) {
        // The destination (render target) was marked resolving in nvBeginPicture;
        // clear it so a later vaSyncSurface doesn't block forever on a blit we
        // never performed.
        setSurfaceResolving(dst, false);
        return false;
    }

    VARectangle srcRegion = { 0, 0, src->width, src->height };
    VARectangle dstRegion = { 0, 0, dst->width, dst->height };
    if (pipeline->surface_region != NULL) {
        srcRegion = *pipeline->surface_region;
    }
    if (pipeline->output_region != NULL) {
        dstRegion = *pipeline->output_region;
    }

    if (srcRegion.x != 0 || srcRegion.y != 0 || dstRegion.x != 0 || dstRegion.y != 0 ||
        srcRegion.width != dstRegion.width || srcRegion.height != dstRegion.height) {
        LOG("Unsupported VideoProc blit: src=%dx%d+%d+%d dst=%dx%d+%d+%d",
            srcRegion.width, srcRegion.height, srcRegion.x, srcRegion.y,
            dstRegion.width, dstRegion.height, dstRegion.x, dstRegion.y);
        setSurfaceResolving(dst, false);
        return false;
    }

    waitSurfaceResolved(src);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), (setSurfaceResolving(dst, false), false));
    bool realised = drv->backend->realiseSurface(drv, src) && drv->backend->realiseSurface(drv, dst);
    if (!realised) {
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        setSurfaceResolving(dst, false);
        return false;
    }

    BackingImage *srcImg = src->backingImage;
    BackingImage *dstImg = dst->backingImage;
    nvSurfaceCopyColorMetadataFromBackingImage(src, srcImg);
    if (srcImg != NULL && dstImg != NULL && (srcImg->format == NV_FORMAT_NV12 || srcImg->format == NV_FORMAT_P010 || srcImg->format == NV_FORMAT_P012) && dstImg->format == NV_FORMAT_ARGB) {
        const VAProcColorStandardType colorStandard = effectiveSurfaceColorStandard(src, pipeline);
        const bool fullRange = effectiveSurfaceColorRangeFull(src, pipeline);
        const ColorMatrix *matrix = colorMatrixForStandard(colorStandard, srcRegion.width, fullRange);
        const VideoProcSampleInfo sampleInfo = videoProcSampleInfoForFormat(srcImg->format, fullRange);
        char srcFourcc[5];
        char dstFourcc[5];
        LOG_DEBUG("VideoProc color matrix: src=%s dst=%s size=%ux%u surface_color_standard=%s(%d) input_matrix_coefficients=%u input_range=%s(%u) decoded_color_standard=%s(%d) decoded_full_range=%d output_color_standard=%s(%d) effective_color_standard=%s(%d) effective_range=%s matrix=%s coeffs=%d,%d,%d,%d sample_shift=%d y_scale=%d y_offset=%d uv_offset=%d value_shift=%d",
            fourccString(formatsInfo[srcImg->format].vaFormat.fourcc, srcFourcc),
            fourccString(formatsInfo[dstImg->format].vaFormat.fourcc, dstFourcc),
            srcRegion.width, srcRegion.height,
            nvColorStandardName(pipeline->surface_color_standard), pipeline->surface_color_standard,
            pipeline->input_color_properties.matrix_coefficients,
            sourceRangeName(pipeline->input_color_properties.color_range), pipeline->input_color_properties.color_range,
            nvColorStandardName(src->colorStandard), src->colorStandard, src->colorRangeFull,
            nvColorStandardName(pipeline->output_color_standard), pipeline->output_color_standard,
            nvColorStandardName(colorStandard), colorStandard,
            effectiveRangeName(fullRange),
            colorMatrixName(matrix), matrix->vToR, matrix->uToG, matrix->vToG, matrix->uToB,
            sampleInfo.sampleShift, sampleInfo.yScale, sampleInfo.yOffset, sampleInfo.uvOffset, sampleInfo.valueShift);
        bool ret = convertNV12ToARGB(drv, srcImg, dstImg, srcRegion.width, srcRegion.height, matrix, sampleInfo);
        bool popFailed = CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        if (!ret || popFailed) {
            setSurfaceResolving(dst, false);
            return false;
        }
        goto done;
    }

    if (srcImg == NULL || dstImg == NULL || srcImg->format != dstImg->format) {
        LOG("Unsupported VideoProc formats: %d -> %d",
            srcImg != NULL ? srcImg->format : NV_FORMAT_NONE,
            dstImg != NULL ? dstImg->format : NV_FORMAT_NONE);
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        setSurfaceResolving(dst, false);
        return false;
    }

    const NVFormatInfo *fmtInfo = &formatsInfo[srcImg->format];
    const bool externalHostSource = nvBackingImageHasExternalHostMemory(srcImg);
    const bool externalDeviceSource = nvBackingImageHasExternalDeviceMemory(srcImg);
    const bool externalHostDestination = nvBackingImageHasExternalHostMemory(dstImg);
    const bool externalDeviceDestination = nvBackingImageHasExternalDeviceMemory(dstImg);
    bool sourceAccessStarted = false;
    bool destinationAccessStarted = false;

    if (externalHostSource) {
        sourceAccessStarted = nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        if (!sourceAccessStarted) {
            nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        }
    }
    if (sourceAccessStarted || !externalHostSource) {
        if (externalHostDestination) {
            destinationAccessStarted = nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            if (!destinationAccessStarted) {
                nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            }
        }
    }

    bool copyFailed = (externalHostSource && !sourceAccessStarted) ||
                      (externalHostDestination && !destinationAccessStarted);

    for (uint32_t i = 0; !copyFailed && i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        CUDA_MEMCPY2D cpy = {
            .WidthInBytes = ((uint32_t) srcRegion.width >> p->ss.x) * fmtInfo->bppc * p->channelCount,
            .Height = (uint32_t) srcRegion.height >> p->ss.y
        };

        if (externalHostSource) {
            cpy.srcMemoryType = CU_MEMORYTYPE_HOST;
            cpy.srcHost = nvBackingImageHostPlane(srcImg, i);
            cpy.srcPitch = (uint32_t) srcImg->strides[i];
        } else if (externalDeviceSource) {
            cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            cpy.srcDevice = nvBackingImageDevicePlane(srcImg, i);
            cpy.srcPitch = (uint32_t) srcImg->strides[i];
        } else {
            cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            cpy.srcArray = srcImg->arrays[i];
        }

        if (externalHostDestination) {
            cpy.dstMemoryType = CU_MEMORYTYPE_HOST;
            cpy.dstHost = nvBackingImageHostPlane(dstImg, i);
            cpy.dstPitch = (uint32_t) dstImg->strides[i];
        } else if (externalDeviceDestination) {
            cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            cpy.dstDevice = nvBackingImageDevicePlane(dstImg, i);
            cpy.dstPitch = (uint32_t) dstImg->strides[i];
        } else {
            cpy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            cpy.dstArray = dstImg->arrays[i];
        }

        const bool hostCopy = externalHostSource || externalHostDestination;
        copyFailed = hostCopy || i == fmtInfo->numPlanes - 1
            ? CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy))
            : CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0));
        if (!copyFailed) {
            nvStatsAdd(drv,
                hostCopy ? NV_STAT_HOST_COPY_BYTES : NV_STAT_DEVICE_COPY_BYTES,
                (uint64_t) cpy.WidthInBytes * cpy.Height);
        }
    }

    if (destinationAccessStarted &&
        !nvSyncBackingImageHostAccess(dstImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE)) {
        copyFailed = true;
    }
    if (sourceAccessStarted &&
        !nvSyncBackingImageHostAccess(srcImg, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) {
        copyFailed = true;
    }
    if (copyFailed) {
        CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));
        setSurfaceResolving(dst, false);
        return false;
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), (setSurfaceResolving(dst, false), false));

done:
    pthread_mutex_lock(&dst->mutex);
    dst->context = src->context;
    dst->progressiveFrame = src->progressiveFrame;
    dst->topFieldFirst = src->topFieldFirst;
    dst->secondField = src->secondField;
    dst->decodeFailed = src->decodeFailed;
    nvSurfaceCopyColorMetadata(dst, src);
    pthread_mutex_unlock(&dst->mutex);

    // Clears both the surface and its backing-image resolving flags and wakes
    // any waiter (vaSyncSurface / a later blit that reuses this surface).
    setSurfaceResolving(dst, false);

    return true;
}

static VAStatus nvBeginPicture(
        VADriverContextP ctx,
        VAContextID context,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (nvCtx->entrypoint == VAEntrypointVideoProc) {
        setSurfaceResolving(surface, true);
        nvCtx->renderTarget = surface;
        return VA_STATUS_SUCCESS;
    }

    if (surface->context != NULL && surface->context != nvCtx) {
        //this surface was last used on a different context, we need to free up the backing image (it might not be the correct size)
        if (surface->backingImage != NULL) {
            drv->backend->detachBackingImageFromSurface(drv, surface);
        }
        //...and reset the pictureIdx
        surface->pictureIdx = -1;
    }

    VAStatus decoderStatus = recreateDecoderForSurface(nvCtx, surface);
    if (decoderStatus != VA_STATUS_SUCCESS) {
        return decoderStatus;
    }

    //if this surface hasn't been used before, give it a new picture index
    if (surface->pictureIdx == -1) {
        const uint32_t surfaceLimit = nvCtx->decoder != NULL
            ? (uint32_t) nvCtx->surfaceCount
            : drv->decodeSurfacesMaximum;
        if ((uint32_t) nvCtx->currentPictureId >= surfaceLimit) {
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        surface->pictureIdx = nvCtx->currentPictureId++;
    }

    setSurfaceResolving(surface, true);

    nvCtx->bitstreamBuffer.failed = false;
    nvCtx->sliceOffsets.failed = false;
    nvCtx->inputValidationFailed = false;
    nvCtx->bitstreamDataOffset = 0;

    memset(&nvCtx->pPicParams, 0, sizeof(CUVIDPICPARAMS));
    nvCtx->renderTarget = surface;
    nvCtx->displayTarget = surface;
    nvCtx->renderTarget->progressiveFrame = true; //assume we're producing progressive frame unless the codec says otherwise
    nvSurfaceResetColorMetadata(nvCtx->renderTarget);
    nvCtx->pPicParams.CurrPicIdx = nvCtx->renderTarget->pictureIdx;
    if (nvCtx->codec != NULL && nvCtx->codec->beginPicture != NULL) {
        nvCtx->codec->beginPicture(nvCtx);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvRenderPicture(
        VADriverContextP ctx,
        VAContextID context,
        VABufferID *buffers,
        int num_buffers
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (nvCtx->entrypoint == VAEntrypointVideoProc) {
        bool processed = false;
        for (int i = 0; i < num_buffers; i++) {
            NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
            if (buf == NULL || buf->ptr == NULL || buf->bufferType != VAProcPipelineParameterBufferType) {
                LOG("Invalid VideoProc buffer detected, skipping: %d", buffers[i]);
                continue;
            }

            nvStatsIncrement(drv, NV_STAT_VIDEOPROC_REQUESTS);
            processed = true;
            VAProcPipelineParameterBuffer *pipeline = (VAProcPipelineParameterBuffer*) buf->ptr;
            NVSurface *src = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, pipeline->surface);
            const uint64_t videoProcStart = nvStatsTimestamp(drv);
            // copySurfaceBackingImage always clears the render target's resolving
            // flag, on both success and every failure path.
            const bool copied = copySurfaceBackingImage(drv, src, nvCtx->renderTarget, pipeline);
            const uint64_t videoProcEnd = nvStatsTimestamp(drv);
            if (videoProcStart != 0 && videoProcEnd >= videoProcStart) {
                nvStatsAdd(drv, NV_STAT_VIDEOPROC_NS, videoProcEnd - videoProcStart);
            }
            if (!copied) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        }

        // If no pipeline buffer touched the render target, it was still marked
        // resolving in nvBeginPicture; clear it so vaSyncSurface can't hang.
        if (!processed) {
            setSurfaceResolving(nvCtx->renderTarget, false);
        }

        return VA_STATUS_SUCCESS;
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    for (int i = 0; i < num_buffers; i++) {
        NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
        if (buf == NULL || buf->ptr == NULL) {
            LOG("Invalid buffer detected, skipping: %d", buffers[i]);
            continue;
        }
        // Validate again at the trust boundary. This protects dispatch even
        // if a buffer object is corrupted or originates outside nvCreateBuffer.
        if (!isValidBufferType(buf->bufferType)) {
            LOG("Invalid buffer type: %d", buf->bufferType);
            return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
        }
        HandlerFunc func = nvCtx->codec->handlers[buf->bufferType];
        if (func != NULL) {
            func(nvCtx, buf, picParams);
            if (nvCtx->inputValidationFailed) {
                LOG("Invalid codec slice range");
                if (nvCtx->displayTarget != NULL) {
                    setSurfaceResolving(nvCtx->displayTarget, false);
                }
                if (nvCtx->renderTarget != NULL &&
                    nvCtx->renderTarget != nvCtx->displayTarget) {
                    setSurfaceResolving(nvCtx->renderTarget, false);
                }
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            if (nvCtx->bitstreamBuffer.failed || nvCtx->sliceOffsets.failed) {
                LOG("Unable to grow codec bitstream buffers");
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
        } else {
            LOG("Unhandled buffer type: %d", buf->bufferType);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvEndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx != NULL && nvCtx->entrypoint == VAEntrypointVideoProc) {
        return VA_STATUS_SUCCESS;
    }

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (nvCtx->inputValidationFailed) {
        nvCtx->bitstreamBuffer.size = 0;
        nvCtx->sliceOffsets.size = 0;
        nvCtx->inputValidationFailed = false;
        if (nvCtx->displayTarget != NULL) {
            setSurfaceResolving(nvCtx->displayTarget, false);
        }
        if (nvCtx->renderTarget != NULL && nvCtx->renderTarget != nvCtx->displayTarget) {
            setSurfaceResolving(nvCtx->renderTarget, false);
        }
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (nvCtx->bitstreamBuffer.failed || nvCtx->sliceOffsets.failed) {
        nvCtx->bitstreamBuffer.size = 0;
        nvCtx->sliceOffsets.size = 0;
        nvCtx->bitstreamBuffer.failed = false;
        nvCtx->sliceOffsets.failed = false;
        if (nvCtx->displayTarget != NULL) {
            setSurfaceResolving(nvCtx->displayTarget, false);
        }
        if (nvCtx->renderTarget != NULL && nvCtx->renderTarget != nvCtx->displayTarget) {
            setSurfaceResolving(nvCtx->renderTarget, false);
        }
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    VAStatus decoderStatus = createDecoderForContext(nvCtx);
    if (decoderStatus != VA_STATUS_SUCCESS) {
        nvCtx->bitstreamBuffer.size = 0;
        nvCtx->sliceOffsets.size = 0;
        if (nvCtx->displayTarget != NULL) {
            setSurfaceResolving(nvCtx->displayTarget, false);
        }
        if (nvCtx->renderTarget != NULL && nvCtx->renderTarget != nvCtx->displayTarget) {
            setSurfaceResolving(nvCtx->renderTarget, false);
        }
        return decoderStatus;
    }

    picParams->pBitstreamData = PTROFF(nvCtx->bitstreamBuffer.buf, nvCtx->bitstreamDataOffset);
    picParams->pSliceDataOffsets = nvCtx->sliceOffsets.buf;
    nvCtx->bitstreamBuffer.size = 0;
    nvCtx->sliceOffsets.size = 0;

    if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
        setSurfaceResolving(nvCtx->displayTarget != NULL ? nvCtx->displayTarget : nvCtx->renderTarget, false);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    CUresult result = cv->cuvidDecodePicture(nvCtx->decoder, picParams);
    if (CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL))) {
        setSurfaceResolving(nvCtx->displayTarget != NULL ? nvCtx->displayTarget : nvCtx->renderTarget, false);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    nvStatsIncrement(drv, NV_STAT_DECODE_PICTURES);

    VAStatus status = VA_STATUS_SUCCESS;

    if (result != CUDA_SUCCESS) {
        LOG("cuvidDecodePicture failed: %d", result);
        status = VA_STATUS_ERROR_DECODING_ERROR;
    }
    //LOG("Decoded frame successfully to idx: %d (%p)", picParams->CurrPicIdx, nvCtx->renderTarget);

    NVSurface *surface = nvCtx->displayTarget != NULL ? nvCtx->displayTarget : nvCtx->renderTarget;
    if (surface != nvCtx->renderTarget) {
        setSurfaceResolving(surface, true);
        setSurfaceResolving(nvCtx->renderTarget, false);
    }

    surface->context = nvCtx;
    surface->topFieldFirst = !picParams->bottom_field_flag;
    surface->secondField = picParams->second_field;
    surface->decodeFailed = status != VA_STATUS_SUCCESS;

    if (!resolveQueuePush(&nvCtx->resolveQueue, surface)) {
        setSurfaceResolving(surface, false);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return status;
}

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    //LOG("Syncing on surface: %d (%p)", surface->pictureIdx, surface);

    waitSurfaceResolved(surface);

    //LOG("Surface %d resolved (%p)", surface->pictureIdx, surface);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status	/* out */
    )
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQuerySurfaceError(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VAStatus error_status,
        void **error_info /*out*/
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags /* de-interlacing flags */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryImageFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        int *num_formats           /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //LOG("In %s", __func__);

    *num_formats = 0;
    for (unsigned int i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].is16bits && !drv->supports16BitSurface) {
            continue;
        }
        if (formatsInfo[i].isYuv444 && !drv->supports444Surface) {
            continue;
        }
        format_list[(*num_formats)++] = formatsInfo[i].vaFormat;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateImage(
        VADriverContextP ctx,
        VAImageFormat *format,
        int width,
        int height,
        VAImage *image     /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (format == NULL || image == NULL || width <= 0 || height <= 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    image->image_id = VA_INVALID_ID;
    image->buf = VA_INVALID_ID;

    NVFormat nvFormat = nvFormatFromVaFormat(format->fourcc);
    if (nvFormat == NV_FORMAT_NONE) {
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    const NVFormatInfo *fmtInfo = &formatsInfo[nvFormat];
    const NVFormatPlane *p = fmtInfo->plane;

    size_t planeSizes[3] = {0};
    size_t imageSize = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size_t planePixels = 0;
        size_t bytesPerSample = 0;
        if (!checkedMultiplySize((size_t) width >> p[i].ss.x,
                                 (size_t) height >> p[i].ss.y,
                                 &planePixels) ||
            !checkedMultiplySize(fmtInfo->bppc, p[i].channelCount, &bytesPerSample) ||
            !checkedMultiplySize(planePixels, bytesPerSample, &planeSizes[i]) ||
            !checkedAddSize(imageSize, planeSizes[i], &imageSize)) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }
    size_t planePitches[3] = {0};
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        size_t planeWidth = (size_t) width >> p[i].ss.x;
        size_t bytesPerSample = 0;
        if (!checkedMultiplySize(fmtInfo->bppc, p[i].channelCount, &bytesPerSample) ||
            !checkedMultiplySize(planeWidth, bytesPerSample, &planePitches[i]) ||
            planePitches[i] > UINT32_MAX) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }
    if (imageSize > UINT32_MAX) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    Object imageObj = allocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    if (imageObj == NULL) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    //LOG("created image id: %d", imageObj->id);

    NVImage *img = (NVImage*) imageObj->obj;
    img->width = width;
    img->height = height;
    img->format = nvFormat;

    //allocate buffer to hold image when we copy down from the GPU
    //TODO could probably put these in a pool, they appear to be allocated, used, then freed
    Object imageBufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    if (imageBufferObject == NULL) {
        deleteObject(drv, imageObj->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    NVBuffer *imageBuffer = (NVBuffer*) imageBufferObject->obj;
    imageBuffer->bufferType = VAImageBufferType;
    imageBuffer->size = imageSize;
    imageBuffer->elements = 1;
    if (!allocateBufferMemory(drv, imageBuffer, imageBuffer->size)) {
        deleteObject(drv, imageBufferObject->id);
        deleteObject(drv, imageObj->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    memset(imageBuffer->ptr, 0, imageBuffer->size);

    img->imageBuffer = imageBuffer;
    img->imageBufferId = imageBufferObject->id;

    image->image_id = imageObj->id;
    memcpy(&image->format, format, sizeof(VAImageFormat));
    image->buf = imageBufferObject->id;	/* image data buffer */
    /*
     * Image data will be stored in a buffer of type VAImageBufferType to facilitate
     * data store on the server side for optimal performance. The buffer will be
     * created by the CreateImage function, and proper storage allocated based on the image
     * size and format. This buffer is managed by the library implementation, and
     * accessed by the client through the buffer Map/Unmap functions.
     */
    image->width = width;
    image->height = height;
    image->data_size = imageBuffer->size;
    image->num_planes = fmtInfo->numPlanes;	/* can not be greater than 3 */
    /*
     * An array indicating the scanline pitch in bytes for each plane.
     * Each plane may have a different pitch. Maximum 3 planes for planar formats
     */
    image->pitches[0] = (uint32_t) planePitches[0];
    image->pitches[1] = (uint32_t) planePitches[1];
    image->pitches[2] = (uint32_t) planePitches[2];
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    image->offsets[0] = 0;
    image->offsets[1] = planeSizes[0];
    image->offsets[2] = planeSizes[0] + planeSizes[1];

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDeriveImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImage *image     /* out */
    )
{
    //LOG("In %s", __func__);
    //FAILED because we don't support it
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus nvDestroyImage(
        VADriverContextP ctx,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVImage *img = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (img == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    if (getObjectPtr(drv, OBJECT_TYPE_BUFFER, img->imageBufferId) != NULL) {
        releaseBufferMemory(drv, img->imageBuffer);
        deleteObject(drv, img->imageBufferId);
    }

    deleteObject(drv, image);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvSetImagePalette(
            VADriverContextP ctx,
            VAImageID image,
            /*
                 * pointer to an array holding the palette data.  The size of the array is
                 * num_palette_entries * entry_bytes in size.  The order of the components
                 * in the palette is described by the component_order in VAImage struct
                 */
                unsigned char *palette
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvGetImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        int x,     /* coordinates of the upper left source pixel */
        int y,
        unsigned int width, /* width and height of the region */
        unsigned int height,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    NVImage *imageObj = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (surfaceObj == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (imageObj == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    NVContext *context = (NVContext*) surfaceObj->context;
    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];

    if (context == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (surfaceObj->backingImage == NULL ||
        surfaceObj->backingImage->format != imageObj->format ||
        x < 0 || y < 0 || width == 0 || height == 0 ||
        (uint64_t) (unsigned int) x + width > surfaceObj->width ||
        (uint64_t) (unsigned int) y + height > surfaceObj->height ||
        width > imageObj->width || height > imageObj->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    //wait for the surface to be decoded
    nvSyncSurface(ctx, surface);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    size_t destinationOffset = 0;
    VAStatus status = VA_STATUS_SUCCESS;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        const size_t bytesPerSample = (size_t) fmtInfo->bppc * p->channelCount;
        const size_t destinationPitch = ((size_t) imageObj->width >> p->ss.x) * bytesPerSample;
        CUDA_MEMCPY2D memcpy2d = {
        .srcXInBytes = ((unsigned int) x >> p->ss.x) * bytesPerSample,
        .srcY = (unsigned int) y >> p->ss.y,
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = surfaceObj->backingImage->arrays[i],

        .dstXInBytes = 0, .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = (char *)imageObj->imageBuffer->ptr + destinationOffset,
        .dstPitch = destinationPitch,

        .WidthInBytes = (width >> p->ss.x) * bytesPerSample,
        .Height = height >> p->ss.y
        };

        CUresult result = cu->cuMemcpy2D(&memcpy2d);
        if (result != CUDA_SUCCESS) {
            LOG("cuMemcpy2D failed: %d", result);
            status = VA_STATUS_ERROR_DECODING_ERROR;
            break;
        }
        destinationOffset += destinationPitch * ((size_t) imageObj->height >> p->ss.y);
    }
    CUresult popResult = cu->cuCtxPopCurrent(NULL);
    if (popResult != CUDA_SUCCESS) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return status;
}

static VAStatus nvPutImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImageID image,
        int src_x,
        int src_y,
        unsigned int src_width,
        unsigned int src_height,
        int dest_x,
        int dest_y,
        unsigned int dest_width,
        unsigned int dest_height
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySubpictureFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        unsigned int *flags,       /* out */
        unsigned int *num_formats  /* out */
    )
{
    LOG("In %s", __func__);
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSubpicture(
        VADriverContextP ctx,
        VAImageID image,
        VASubpictureID *subpicture   /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDestroySubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureImage(
                VADriverContextP ctx,
                VASubpictureID subpicture,
                VAImageID image
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureChromakey(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        unsigned int chromakey_min,
        unsigned int chromakey_max,
        unsigned int chromakey_mask
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureGlobalAlpha(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        float global_alpha
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvAssociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces,
        short src_x, /* upper left offset in subpicture */
        short src_y,
        unsigned short src_width,
        unsigned short src_height,
        short dest_x, /* upper left offset in surface */
        short dest_y,
        unsigned short dest_width,
        unsigned short dest_height,
        /*
         * whether to enable chroma-keying or global-alpha
         * see VA_SUBPICTURE_XXX values
         */
        unsigned int flags
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDeassociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* out */
        int *num_attributes		/* out */
        )
{
    LOG("In %s", __func__);
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* in/out */
        int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetDisplayAttributes(
        VADriverContextP ctx,
                VADisplayAttribute *attr_list,
                int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQuerySurfaceAttributes(
        VADriverContextP    ctx,
	    VAConfigID          config,
	    VASurfaceAttrib    *attrib_list,
	    unsigned int       *num_attribs
	)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config);

    if (num_attribs == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->entrypoint == VAEntrypointVideoProc) {
        // libva supplies array capacity through *num_attribs. Publish the
        // required count, but never write when the caller's array is smaller.
        const unsigned int required = drv->supports16BitSurface ? 14 : 13;
        const unsigned int capacity = *num_attribs;
        *num_attribs = required;

        if (attrib_list == NULL) {
            return VA_STATUS_SUCCESS;
        }
        if (capacity < required) {
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }

        {
            attrib_list[0].type = VASurfaceAttribMinWidth;
            attrib_list[0].flags = 0;
            attrib_list[0].value.type = VAGenericValueTypeInteger;
            attrib_list[0].value.value.i = 1;

            attrib_list[1].type = VASurfaceAttribMinHeight;
            attrib_list[1].flags = 0;
            attrib_list[1].value.type = VAGenericValueTypeInteger;
            attrib_list[1].value.value.i = 1;

            attrib_list[2].type = VASurfaceAttribMaxWidth;
            attrib_list[2].flags = 0;
            attrib_list[2].value.type = VAGenericValueTypeInteger;
            attrib_list[2].value.value.i = 16384;

            attrib_list[3].type = VASurfaceAttribMaxHeight;
            attrib_list[3].flags = 0;
            attrib_list[3].value.type = VAGenericValueTypeInteger;
            attrib_list[3].value.value.i = 16384;

            int attrib_idx = 4;
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_NV12;
            attrib_idx++;

            if (drv->supports16BitSurface) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P010;
                attrib_idx++;
            }

            const uint32_t rgbFormats[] = {
                VA_FOURCC_ARGB,
                VA_FOURCC_XRGB,
                VA_FOURCC_ABGR,
                VA_FOURCC_XBGR,
                VA_FOURCC_RGBA,
                VA_FOURCC_RGBX,
                VA_FOURCC_BGRA,
                VA_FOURCC_BGRX
            };
            for (uint32_t i = 0; i < ARRAY_SIZE(rgbFormats); i++) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = (int) rgbFormats[i];
                attrib_idx++;
            }
        }

        return VA_STATUS_SUCCESS;
    }

    //LOG("with %d (%d) %p %d", cfg->cudaCodec, cfg->bitDepth, attrib_list, *num_attribs);

    if (cfg->chromaFormat != cudaVideoChromaFormat_420 && cfg->chromaFormat != cudaVideoChromaFormat_444) {
        //TODO not sure what pixel formats are needed for 422 formats
        LOG("Unknown chroma format: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if ((cfg->chromaFormat == cudaVideoChromaFormat_444 || cfg->surfaceFormat == cudaVideoSurfaceFormat_YUV444_16Bit) && !drv->supports444Surface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("YUV444 surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->surfaceFormat == cudaVideoSurfaceFormat_P016 && !drv->supports16BitSurface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("16 bits surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    int required = 4;
    if (cfg->chromaFormat == cudaVideoChromaFormat_444) {
        required += 1;
#if VA_CHECK_VERSION(1, 20, 0)
        required += 1;
#endif
    } else {
        required += 1;
        if (drv->supports16BitSurface) {
            required += 3;
        }
    }
    // Apply the same capacity contract to decode attributes before querying
    // CUDA or writing any caller-owned memory.
    const unsigned int capacity = *num_attribs;
    *num_attribs = (unsigned int) required;

    if (attrib_list == NULL) {
        return VA_STATUS_SUCCESS;
    }
    if (capacity < (unsigned int) required) {
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    {
        CUVIDDECODECAPS videoDecodeCaps = {
            .eCodecType      = cfg->cudaCodec,
            .eChromaFormat   = cfg->chromaFormat,
            .nBitDepthMinus8 = cfg->bitDepth - 8
        };

        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
        CHECK_CUDA_RESULT_RETURN(cv->cuvidGetDecoderCaps(&videoDecodeCaps), VA_STATUS_ERROR_OPERATION_FAILED);
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

        attrib_list[0].type = VASurfaceAttribMinWidth;
        attrib_list[0].flags = 0;
        attrib_list[0].value.type = VAGenericValueTypeInteger;
        attrib_list[0].value.value.i = videoDecodeCaps.nMinWidth;

        attrib_list[1].type = VASurfaceAttribMinHeight;
        attrib_list[1].flags = 0;
        attrib_list[1].value.type = VAGenericValueTypeInteger;
        attrib_list[1].value.value.i = videoDecodeCaps.nMinHeight;

        attrib_list[2].type = VASurfaceAttribMaxWidth;
        attrib_list[2].flags = 0;
        attrib_list[2].value.type = VAGenericValueTypeInteger;
        attrib_list[2].value.value.i = videoDecodeCaps.nMaxWidth;

        attrib_list[3].type = VASurfaceAttribMaxHeight;
        attrib_list[3].flags = 0;
        attrib_list[3].value.type = VAGenericValueTypeInteger;
        attrib_list[3].value.value.i = videoDecodeCaps.nMaxHeight;

        //LOG("Returning constraints: width: %d - %d, height: %d - %d", attrib_list[0].value.value.i, attrib_list[2].value.value.i, attrib_list[1].value.value.i, attrib_list[3].value.value.i);

        int attrib_idx = 4;

        /* returning all the surfaces here probably isn't the best thing we could do
         * but we don't always have enough information to determine exactly which
         * pixel formats should be used (for instance, AV1 10-bit videos) */
        if (cfg->chromaFormat == cudaVideoChromaFormat_444) {
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_444P;
            attrib_idx += 1;
#if VA_CHECK_VERSION(1, 20, 0)
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_Q416;
            attrib_idx += 1;
#endif
        } else {
            attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
            attrib_list[attrib_idx].flags = 0;
            attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
            attrib_list[attrib_idx].value.value.i = VA_FOURCC_NV12;
            attrib_idx += 1;
            if (drv->supports16BitSurface) {
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P010;
                attrib_idx += 1;
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P012;
                attrib_idx += 1;
                attrib_list[attrib_idx].type = VASurfaceAttribPixelFormat;
                attrib_list[attrib_idx].flags = 0;
                attrib_list[attrib_idx].value.type = VAGenericValueTypeInteger;
                attrib_list[attrib_idx].value.value.i = VA_FOURCC_P016;
                attrib_idx += 1;
            }
        }
    }

    return VA_STATUS_SUCCESS;
}

/* used by va trace */
static VAStatus nvBufferInfo(
           VADriverContextP ctx,      /* in */
           VABufferID buf_id,         /* in */
           VABufferType *type,        /* out */
           unsigned int *size,        /* out */
           unsigned int *num_elements /* out */
)
{
    LOG("In %s", __func__);
    *size=0;
    *num_elements=0;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvAcquireBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id,         /* in */
            VABufferInfo *      buf_info        /* in/out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvReleaseBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id          /* in */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

//        /* lock/unlock surface for external access */
static VAStatus nvLockSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        unsigned int *fourcc, /* out  for follow argument */
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name, /* if it is not NULL, assign the low lever
                                    * surface buffer name
                                    */
        void **buffer /* if it is not NULL, map the surface buffer for
                       * CPU access
                       */
)
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvUnlockSurface(
        VADriverContextP ctx,
                VASurfaceID surface
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvCreateMFContext(
            VADriverContextP ctx,
            VAMFContextID *mfe_context    /* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFAddContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFReleaseContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFSubmit(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID *contexts,
            int num_contexts
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus nvCreateBuffer2(
            VADriverContextP ctx,
            VAContextID context,                /* in */
            VABufferType type,                  /* in */
            unsigned int width,                 /* in */
            unsigned int height,                /* in */
            unsigned int *unit_size,            /* out */
            unsigned int *pitch,                /* out */
            VABufferID *buf_id                  /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryProcessingRate(
            VADriverContextP ctx,               /* in */
            VAConfigID config_id,               /* in */
            VAProcessingRateParameter *proc_buf,/* in */
            unsigned int *processing_rate	/* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvExportSurfaceHandle(
            VADriverContextP    ctx,
            VASurfaceID         surface_id,     /* in */
            uint32_t            mem_type,       /* in */
            uint32_t            flags,          /* in */
            void               *descriptor      /* out */
)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    if (descriptor == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if ((mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) == 0) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_id);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    //LOG("Exporting surface: %d (%p)", surface->pictureIdx, surface);

    waitSurfaceResolved(surface);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    if (!drv->backend->realiseSurface(drv, surface)) {
        LOG("Unable to export surface");
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;

    const bool descriptorFilled = drv->backend->fillExportDescriptor(drv, surface, ptr);

    // LOG("Exporting with w:%d h:%d o:%d p:%d m:%" PRIx64 " o:%d p:%d m:%" PRIx64, ptr->width, ptr->height, ptr->layers[0].offset[0],
    //                                                             ptr->layers[0].pitch[0], ptr->objects[0].drm_format_modifier,
    //                                                             ptr->layers[1].offset[0], ptr->layers[1].pitch[0],
    //                                                             ptr->objects[1].drm_format_modifier);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    if (!descriptorFilled) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvTerminate( VADriverContextP ctx )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Terminating %p", ctx);

    pthread_mutex_lock(&drv->objectCreationMutex);
    drv->terminating = true;
    pthread_mutex_unlock(&drv->objectCreationMutex);

    nvStatsLog(drv, "pre_cleanup");

    // Stop and join every worker before touching any surface or backing image.
    // deleteAllContexts never holds objectCreationMutex across pthread_join.
    if (!deleteAllContexts(drv)) {
        LOG("Unable to join all resolve threads; retaining driver state");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (CHECK_CUDA_RESULT(cu->cuCtxPushCurrent(drv->cudaContext))) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    drv->backend->destroyAllBackingImage(drv);

    // Contexts are already gone. Remove all remaining objects from the end so
    // array compaction cannot skip adjacent entries.
    deleteAllObjects(drv);

    if (drv->videoProcStream != NULL) {
        CHECK_CUDA_RESULT(cu->cuStreamSynchronize(drv->videoProcStream));
    }
    if (drv->videoProcEvent != NULL) {
        CHECK_CUDA_RESULT(cu->cuEventDestroy(drv->videoProcEvent));
        drv->videoProcEvent = NULL;
    }
    if (drv->videoProcStream != NULL) {
        CHECK_CUDA_RESULT(cu->cuStreamDestroy(drv->videoProcStream));
        drv->videoProcStream = NULL;
    }
    if (drv->videoProcArrayModule != NULL) {
        CHECK_CUDA_RESULT(cu->cuModuleUnload(drv->videoProcArrayModule));
        drv->videoProcArrayModule = NULL;
        drv->arrayToArgbKernel = NULL;
    }
    if (drv->videoProcModule != NULL) {
        CHECK_CUDA_RESULT(cu->cuModuleUnload(drv->videoProcModule));
        drv->videoProcModule = NULL;
        drv->nv12ToArgbKernel = NULL;
    }
    if (drv->videoProcModuleP010 != NULL) {
        CHECK_CUDA_RESULT(cu->cuModuleUnload(drv->videoProcModuleP010));
        drv->videoProcModuleP010 = NULL;
        drv->p010ToArgbKernel = NULL;
    }
    trimVideoProcScratchLocked(drv, true, true);

    drv->backend->releaseExporter(drv);

    bool failed = CHECK_CUDA_RESULT(cu->cuCtxPopCurrent(NULL));

    destroyBufferPool(drv);
    nvStatsLog(drv, "final");

    failed |= CHECK_CUDA_RESULT(cu->cuCtxDestroy(drv->cudaContext));
    drv->cudaContext = NULL;

    nvdObjectTableDestroy(&drv->objects);
    free(drv->images.buf);
    if (drv->bufferPoolMutexInitialized) {
        pthread_mutex_destroy(&drv->bufferPoolMutex);
        drv->bufferPoolMutexInitialized = false;
    }
    pthread_mutex_destroy(&drv->exportMutex);
    pthread_mutex_destroy(&drv->imagesMutex);
    pthread_mutex_destroy(&drv->objectCreationMutex);
    ctx->pDriverData = NULL;
    releaseInstanceSlot();
    free(drv);

    return failed ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}

extern const NVBackend DIRECT_BACKEND;
extern const NVBackend EGL_BACKEND;

#define VTABLE(func) .va ## func = &nv ## func
static const struct VADriverVTable vtable = {
    VTABLE(Terminate),
    VTABLE(QueryConfigProfiles),
    VTABLE(QueryConfigEntrypoints),
    VTABLE(QueryConfigAttributes),
    VTABLE(CreateConfig),
    VTABLE(DestroyConfig),
    VTABLE(GetConfigAttributes),
    VTABLE(CreateSurfaces),
    VTABLE(CreateSurfaces2),
    VTABLE(DestroySurfaces),
    VTABLE(CreateContext),
    VTABLE(DestroyContext),
    VTABLE(CreateBuffer),
    VTABLE(BufferSetNumElements),
    VTABLE(MapBuffer),
    VTABLE(UnmapBuffer),
    VTABLE(DestroyBuffer),
    VTABLE(BeginPicture),
    VTABLE(RenderPicture),
    VTABLE(EndPicture),
    VTABLE(SyncSurface),
    VTABLE(QuerySurfaceStatus),
    VTABLE(QuerySurfaceError),
    VTABLE(PutSurface),
    VTABLE(QueryImageFormats),
    VTABLE(CreateImage),
    VTABLE(DeriveImage),
    VTABLE(DestroyImage),
    VTABLE(SetImagePalette),
    VTABLE(GetImage),
    VTABLE(PutImage),
    VTABLE(QuerySubpictureFormats),
    VTABLE(CreateSubpicture),
    VTABLE(DestroySubpicture),
    VTABLE(SetSubpictureImage),
    VTABLE(SetSubpictureChromakey),
    VTABLE(SetSubpictureGlobalAlpha),
    VTABLE(AssociateSubpicture),
    VTABLE(DeassociateSubpicture),
    VTABLE(QueryDisplayAttributes),
    VTABLE(GetDisplayAttributes),
    VTABLE(SetDisplayAttributes),
    VTABLE(QuerySurfaceAttributes),
    VTABLE(BufferInfo),
    VTABLE(AcquireBufferHandle),
    VTABLE(ReleaseBufferHandle),
    VTABLE(LockSurface),
    VTABLE(UnlockSurface),
    VTABLE(CreateMFContext),
    VTABLE(MFAddContext),
    VTABLE(MFReleaseContext),
    VTABLE(MFSubmit),
    VTABLE(CreateBuffer2),
    VTABLE(QueryProcessingRate),
    VTABLE(ExportSurfaceHandle),
};

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx);

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    LOG("Initialising NVIDIA VA-API Driver");

    //drm_state can be passed in with any display type, including X11. But if it's X11, we don't
    //want to use the fd as it'll likely be an Intel GPU, as NVIDIA doesn't support DRI3 at the moment
    bool isDrm = ctx->drm_state != NULL && ((struct drm_state*) ctx->drm_state)->fd > 0;
    int drmFd = (gpu == -1 && isDrm) ? ((struct drm_state*) ctx->drm_state)->fd : -1;

    //check if the drmFd is actually an nvidia one
    LOG("Got DRM FD: %d %d", isDrm, drmFd)
    if (drmFd != -1) {
        if (!isNvidiaDrmFd(drmFd, true)) {
            LOG("Passed in DRM FD does not belong to the NVIDIA driver, ignoring");
            drmFd = -1;
        } else if (!checkModesetParameterFromFd(drmFd)) {
            //we have an NVIDIA fd but no modeset (which means no DMA-BUF support)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    pthread_mutex_lock(&concurrency_mutex);
    LOG("Now have %d (%d max) instances", instances, max_instances);
    if (max_instances > 0 && instances >= max_instances) {
        pthread_mutex_unlock(&concurrency_mutex);
        return VA_STATUS_ERROR_HW_BUSY;
    }
    instances++;
    pthread_mutex_unlock(&concurrency_mutex);

    //check to make sure we initialised the CUDA functions correctly
    if (cu == NULL || cv == NULL) {
        releaseInstanceSlot();
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    NVDriver *drv = (NVDriver*) calloc(1, sizeof(NVDriver));
    if (drv == NULL) {
        releaseInstanceSlot();
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    nvdObjectTableInit(&drv->objects);
    ctx->pDriverData = drv;
    bool objectMutexInitialized = false;
    bool imagesMutexInitialized = false;
    bool exportMutexInitialized = false;
    bool exporterInitialized = false;
    bool cudaContextCreated = false;

    drv->cu = cu;
    drv->cv = cv;
    drv->useCorrectNV12Format = true;
    drv->cudaGpuId = gpu;
    //make sure that we want the default GPU, and that a DRM fd that we care about is passed in
    drv->drmFd = drmFd;
    drv->maxDetachedBackingImageBytes =
        parseEnvU64("NVD_MAX_DETACHED_BACKING_IMAGE_BYTES", defaultMaxDetachedBackingImageBytes(drv->cudaGpuId));
    drv->maxDetachedBackingImages =
        (uint32_t) parseEnvU64("NVD_MAX_DETACHED_BACKING_IMAGES", DEFAULT_MAX_DETACHED_BACKING_IMAGES);
    drv->memoryBudgetBytes = parseEnvU64("NVD_MEMORY_BUDGET_BYTES", 0);
    drv->decodeSurfacesOverride = parseDecodeSurfaceOverride(&drv->decodeSurfacesAuto);
    drv->decodeSurfacesMinimum =
        (uint32_t) parseEnvU64("NVD_DECODE_SURFACES_MIN", 2);
    drv->decodeSurfacesMaximum =
        (uint32_t) parseEnvU64("NVD_DECODE_SURFACES_MAX", 32);
    if (drv->decodeSurfacesMinimum == 0) {
        drv->decodeSurfacesMinimum = 1;
    }
    if (drv->decodeSurfacesMaximum < drv->decodeSurfacesMinimum) {
        drv->decodeSurfacesMaximum = drv->decodeSurfacesMinimum;
    }
    if (drv->decodeSurfacesMaximum > 32) {
        drv->decodeSurfacesMaximum = 32;
    }
    if (drv->decodeSurfacesMinimum > drv->decodeSurfacesMaximum) {
        drv->decodeSurfacesMinimum = drv->decodeSurfacesMaximum;
    }
    drv->videoProcScratchMaxBytes =
        parseEnvU64("NVD_VIDEOPROC_SCRATCH_MAX_BYTES", DEFAULT_VIDEOPROC_SCRATCH_MAX_BYTES);
    drv->bufferPoolMaxBytes =
        parseEnvU64("NVD_BUFFER_POOL_MAX_BYTES", 64ULL * 1024ULL * 1024ULL);

    nvStatsInit(drv);

    if (backend == EGL) {
        LOG("Selecting EGL backend");
        drv->backend = &EGL_BACKEND;
    } else if (backend == DIRECT) {
        LOG("Selecting Direct backend");
        drv->backend = &DIRECT_BACKEND;
    }

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_display_attributes = 1;
    ctx->max_image_formats = ARRAY_SIZE(formatsInfo) - 1;
    ctx->max_subpic_formats = 1;

    if (backend == DIRECT) {
        ctx->str_vendor = "VA-API NVDEC driver [direct backend]";
    } else if (backend == EGL) {
        ctx->str_vendor = "VA-API NVDEC driver [egl backend]";
    }

    pthread_mutexattr_t attrib;
    if (pthread_mutexattr_init(&attrib) != 0) {
        goto fail;
    }
    if (pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE) != 0) {
        pthread_mutexattr_destroy(&attrib);
        goto fail;
    }
    if (pthread_mutex_init(&drv->objectCreationMutex, &attrib) != 0) {
        pthread_mutexattr_destroy(&attrib);
        goto fail;
    }
    objectMutexInitialized = true;
    if (pthread_mutex_init(&drv->imagesMutex, &attrib) != 0) {
        pthread_mutexattr_destroy(&attrib);
        goto fail;
    }
    imagesMutexInitialized = true;
    pthread_mutexattr_destroy(&attrib);
    if (pthread_mutex_init(&drv->exportMutex, NULL) != 0) {
        goto fail;
    }
    exportMutexInitialized = true;
    if (pthread_mutex_init(&drv->bufferPoolMutex, NULL) != 0) {
        goto fail;
    }
    drv->bufferPoolMutexInitialized = true;

    if (!drv->backend->initExporter(drv)) {
        LOG("Exporter failed");
        goto fail;
    }
    exporterInitialized = true;

    if (CHECK_CUDA_RESULT(cu->cuCtxCreate(&drv->cudaContext, CU_CTX_SCHED_BLOCKING_SYNC, drv->cudaGpuId))) {
        goto fail;
    }
    cudaContextCreated = true;

    //CHECK_CUDA_RESULT_RETURN(cv->cuvidCtxLockCreate(&drv->vidLock, drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    if (nvQueryConfigProfiles2(ctx, drv->profiles, &drv->profileCount) != VA_STATUS_SUCCESS) {
        goto fail;
    }

    if (drv->profileCount == 0) {
        LOG("Hardware doesn't seem to support profiles, bailing out");
        goto fail;
    }

    *ctx->vtable = vtable;
    return VA_STATUS_SUCCESS;

fail:
    if (exporterInitialized) {
        drv->backend->releaseExporter(drv);
    }
    if (cudaContextCreated) {
        CHECK_CUDA_RESULT(cu->cuCtxDestroy(drv->cudaContext));
        drv->cudaContext = NULL;
    }
    if (exportMutexInitialized) {
        pthread_mutex_destroy(&drv->exportMutex);
    }
    if (drv->bufferPoolMutexInitialized) {
        destroyBufferPool(drv);
        pthread_mutex_destroy(&drv->bufferPoolMutex);
        drv->bufferPoolMutexInitialized = false;
    }
    if (imagesMutexInitialized) {
        pthread_mutex_destroy(&drv->imagesMutex);
    }
    if (objectMutexInitialized) {
        pthread_mutex_destroy(&drv->objectCreationMutex);
    }
    nvdObjectTableDestroy(&drv->objects);
    free(drv->images.buf);
    free(drv);
    ctx->pDriverData = NULL;
    releaseInstanceSlot();
    return VA_STATUS_ERROR_OPERATION_FAILED;
}
