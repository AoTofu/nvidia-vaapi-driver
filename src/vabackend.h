#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <va/va_backend.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <stdio.h>
#include <pthread.h>
#include "list.h"
#include "resolve-queue.h"
#include "appendable-buffer.h"
#include "direct/nv-driver.h"
#include "common.h"
#include "stats.h"
#include "surface-import.h"
#include "object-table.h"

#define MAX_IMAGE_COUNT 64
#define MAX_PROFILES 32
#define NVD_BUFFER_POOL_CLASS_COUNT 11

typedef enum
{
    OBJECT_TYPE_CONFIG,
    OBJECT_TYPE_CONTEXT,
    OBJECT_TYPE_SURFACE,
    OBJECT_TYPE_BUFFER,
    OBJECT_TYPE_IMAGE
} ObjectType;

typedef NVDObject *Object;

typedef struct
{
    unsigned int    elements;
    size_t          elementSize;
    size_t          size;
    VABufferType    bufferType;
    VAContextID     ownerContextId;
    bool            imageOwned;
    void            *ptr;
    size_t          capacity;
    int8_t          poolClass;
} NVBuffer;

typedef struct _NVBufferPoolBlock {
    struct _NVBufferPoolBlock *next;
} NVBufferPoolBlock;

struct _NVContext;
struct _BackingImage;

#define NVD_MAX_DECODE_SURFACES 32U
#define NVD_MAX_AV1_TILES 4096U

typedef struct
{
    uint32_t                width;
    uint32_t                height;
    cudaVideoSurfaceFormat  format;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    int                     pictureIdx;
    struct _NVContext       *context;
    VAContextID             contextId;
    int                     progressiveFrame;
    int                     topFieldFirst;
    int                     secondField;
    int                     order_hint; //needed for AV1
    VAProcColorStandardType colorStandard;
    bool                    colorRangeFull;
    struct _BackingImage    *backingImage;
    int                     resolving;
    int                     fourcc;
    pthread_mutex_t         mutex;
    pthread_cond_t          cond;
    bool                    syncInitialized;
    bool                    decodeFailed;
    uint64_t                submittedGeneration;
    uint64_t                completedGeneration;
    VAStatus                completionStatus;
} NVSurface;

typedef enum
{
    NV_FORMAT_NONE,
    NV_FORMAT_NV12,
    NV_FORMAT_P010,
    NV_FORMAT_P012,
    NV_FORMAT_P016,
    NV_FORMAT_444P,
    NV_FORMAT_Q416,
    NV_FORMAT_ARGB
} NVFormat;

typedef struct
{
    uint32_t    width;
    uint32_t    height;
    NVFormat    format;
    NVBuffer    *imageBuffer;
    VABufferID  imageBufferId;
} NVImage;

typedef struct {
    CUexternalMemory extMem;
    CUmipmappedArray mipmapArray;
} NVCudaImage;

typedef uint64_t NVCUsurfObject;
typedef CUresult CUDAAPI NVCuSurfObjectCreate(NVCUsurfObject *surfaceObject,
                                              const CUDA_RESOURCE_DESC *resourceDesc);
typedef CUresult CUDAAPI NVCuSurfObjectDestroy(NVCUsurfObject surfaceObject);

typedef struct _BackingImage {
    NVSurface   *surface;
    EGLImage    image;
    CUarray     arrays[3];
    uint32_t    width;
    uint32_t    height;
    int         fourcc;
    int         fds[4];
    dev_t       st_dev[4];
    ino_t       st_ino[4];
    int         offsets[4];
    int         strides[4];
    uint64_t    mods[4];
    uint32_t    size[4];
    uint64_t    objectSize[4];
    uint32_t    planeObjectIndex[4];
    uint32_t    numObjects;
    uint32_t    numPlanes;
    //direct backend only
    NVCudaImage cudaImages[3];
    NVFormat    format;
    VAProcColorStandardType colorStandard;
    bool        colorRangeFull;
    uint64_t    totalSize;
    CUexternalMemory extMem;
    bool        isSingleBuffer;
    bool        isExternalBuffer;
    bool        borrowedCudaResources;
    struct _BackingImage *borrowedBackingImage;
    atomic_uint borrowCount;
    bool        syncInitialized;
    bool        resolving;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    void        *externalMappings[4];
    uint64_t    externalMappingSize[4];
    CUexternalMemory externalObjectMems[4];
    CUdeviceptr externalDevicePtrs[4];
    uint64_t    externalDeviceSize[4];
    CUtexObject cachedVideoProcTextures[3];
    NVCUsurfObject cachedVideoProcSurfaces[3];
    uint64_t    detachedSerial;
    struct _BackingImage *detachedPrev;
    struct _BackingImage *detachedNext;
    uint64_t    statsBytes;
    bool        statsTracked;
    bool        statsActive;
    bool        statsBorrowed;
    bool        statsExternal;
} BackingImage;

typedef enum {
    NVD_PICTURE_IDLE = 0,
    NVD_PICTURE_BUILDING,
    NVD_PICTURE_FAILED,
    NVD_PICTURE_SUBMITTED,
} NVDPictureState;

struct _NVDriver;

typedef struct {
    const char *name;
    bool (*initExporter)(struct _NVDriver *drv);
    void (*releaseExporter)(struct _NVDriver *drv);
    bool (*exportCudaPtr)(struct _NVDriver *drv, CUdeviceptr ptr, NVSurface *surface,
                          uint32_t pitch, CUstream stream, CUevent completeEvent);
    void (*detachBackingImageFromSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*realiseSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*fillExportDescriptor)(struct _NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc);
    void (*destroyAllBackingImage)(struct _NVDriver *drv);
    bool (*pruneToMemoryBudget)(struct _NVDriver *drv, uint64_t extraGpuBytes);
} NVBackend;

typedef struct _NVDriver
{
    CudaFunctions           *cu;
    CuvidFunctions          *cv;
    CUcontext               cudaContext;
    // A failed completion check leaves GPU ownership unknown. Retain resources
    // until process exit and reject further work instead of recycling them.
    atomic_bool             cudaWorkUnsafe;
    CUvideoctxlock          vidLock;
    NVDObjectTable          objects;
    // Coarse lifetime barrier for VA objects. Ordinary entrypoints hold a
    // read lock while using table-derived pointers; destroy entrypoints hold
    // the write lock before unpublishing and freeing an object.
    // Opaque here so users of this shared header do not need POSIX rwlock
    // feature-test macros; vabackend.c owns the pthread_rwlock_t allocation.
    void                    *objectLifetimeLock;
    pthread_mutex_t         objectCreationMutex;
    bool                    terminating;
    bool                    useCorrectNV12Format;
    bool                    supports16BitSurface;
    bool                    supports444Surface;
    int                     cudaGpuId;
    int                     drmFd;
    pthread_mutex_t         exportMutex;
    pthread_mutex_t         imagesMutex;
    pthread_mutex_t         bufferPoolMutex;
    bool                    bufferPoolMutexInitialized;
    pthread_mutex_t         securityClearMutex;
    bool                    securityClearMutexInitialized;
    CUstream                securityClearStream;
    CUdeviceptr             securityClearBuffer;
    size_t                  securityClearBufferSize;
    NVBufferPoolBlock       *bufferPool[NVD_BUFFER_POOL_CLASS_COUNT];
    uint32_t                bufferPoolCounts[NVD_BUFFER_POOL_CLASS_COUNT];
    uint64_t                bufferPoolBytes;
    uint64_t                bufferPoolMaxBytes;
    Array/*<NVEGLImage>*/   images;
    const NVBackend         *backend;
    //fields for direct backend
    NVDriverContext         driverContext;
    //fields for egl backend
    EGLDeviceEXT            eglDevice;
    EGLDisplay              eglDisplay;
    EGLContext              eglContext;
    EGLStreamKHR            eglStream;
    CUeglStreamConnection   cuStreamConnection;
    int                     numFramesPresented;
    int                     profileCount;
    VAProfile               profiles[MAX_PROFILES];
    CUmodule                videoProcModule;
    CUfunction              nv12ToArgbKernel;
    CUfunction              p010ToArgbKernel;
    CUmodule                videoProcModuleP010;
    CUmodule                videoProcArrayModule;
    CUfunction              arrayToArgbKernel;
    bool                    videoProcKernelP010Failed;
    bool                    videoProcKernelFailed;
    bool                    videoProcArrayKernelFailed;
    CUstream                videoProcStream;
    CUevent                 videoProcEvent;
    NVCuSurfObjectCreate    *cuSurfObjectCreate;
    NVCuSurfObjectDestroy   *cuSurfObjectDestroy;
    bool                    surfaceFunctionsLoaded;
    CUdeviceptr             videoProcYBuffer;
    CUdeviceptr             videoProcUVBuffer;
    CUdeviceptr             videoProcArgbBuffer;
    size_t                  videoProcYBufferSize;
    size_t                  videoProcUVBufferSize;
    size_t                  videoProcArgbBufferSize;
    void                    *cpuVideoProcYBuffer;
    void                    *cpuVideoProcUVBuffer;
    void                    *cpuVideoProcArgbBuffer;
    size_t                  cpuVideoProcYBufferSize;
    size_t                  cpuVideoProcUVBufferSize;
    size_t                  cpuVideoProcArgbBufferSize;
    uint64_t                videoProcScratchMaxBytes;
    uint32_t                videoProcCudaFramesSinceCpuFallback;
    bool                    statsEnabled;
    uint64_t                statsLogInterval;
    atomic_uint_fast64_t    stats[NV_STAT_COUNT];
    uint64_t                maxDetachedBackingImageBytes;
    uint32_t                maxDetachedBackingImages;
    uint64_t                detachedBackingImageSerial;
    BackingImage            *detachedBackingImageHead;
    BackingImage            *detachedBackingImageTail;
    uint64_t                detachedBackingImageBytes;
    uint32_t                detachedBackingImageCount;
    uint64_t                memoryBudgetBytes;
    uint32_t                decodeSurfacesOverride;
    bool                    decodeSurfacesAuto;
    uint32_t                decodeSurfacesMinimum;
    uint32_t                decodeSurfacesMaximum;
    uint64_t                hostBufferTrimThresholdBytes;
    uint32_t                hostBufferTrimFrames;
} NVDriver;

struct _NVCodec;

typedef struct _NVContext
{
    NVDriver            *drv;
    VAContextID         id;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
    uint32_t            width;
    uint32_t            height;
    CUvideodecoder      decoder;
    NVSurface           *renderTarget;
    NVSurface           *displayTarget;
    void                *codecData;
    void                *lastSliceParams;
    unsigned int        lastSliceParamsCount;
    AppendableBuffer    bitstreamBuffer;
    size_t              bitstreamDataOffset;
    AppendableBuffer    sliceOffsets;
    AppendableBuffer    sliceParamsBuffer;
    bool                av1SequenceEnableRestoration;
    uint32_t            av1TileOffsetsSeen;
    uint64_t            av1TileSeen[NVD_MAX_AV1_TILES / 64U];
    uint32_t            av1TileMinOffset;
    uint32_t            av1TileMaxEnd;
    bool                av1BitstreamCompacted;
    CUVIDPICPARAMS      pPicParams;
    const struct _NVCodec *codec;
    cudaVideoCodec      cudaCodec;
    cudaVideoSurfaceFormat decoderSurfaceFormat;
    cudaVideoChromaFormat decoderChromaFormat;
    int                 decoderBitDepth;
    int                 currentPictureId;
    NVSurface           *pictureIndexOwners[NVD_MAX_DECODE_SURFACES];
    uint32_t            activeReferenceMask;
    uint32_t            buildingReferenceMask;
    uint32_t            pictureIndexRecycleCursor;
    pthread_t           resolveThread;
    bool                resolveThreadStarted;
    CUstream            resolveStream;
    CUevent             resolveCompleteEvent;
    ResolveQueue        resolveQueue;
    pthread_mutex_t     surfaceCreationMutex;
    bool                surfaceCreationMutexInitialized;
    pthread_mutex_t     pictureMutex;
    bool                pictureMutexInitialized;
    int                 surfaceCount;
    uint32_t            clientRenderTargetCount;
    uint32_t            decodeSurfaceReferenceHint;
    uint32_t            hostBufferUnderuseFrames;
    bool                destroying;
    bool                inputValidationFailed;
    NVDPictureState     pictureState;
    VAStatus            pictureFailure;
} NVContext;

bool nvValidateSliceRange(NVContext *ctx, const NVBuffer *buffer,
                          uint32_t offset, size_t size, const void **data);
bool nvAddCuvidSlices(NVContext *ctx, CUVIDPICPARAMS *picParams, size_t count);
bool nvCanAppendCuvidBitstream(NVContext *ctx, size_t additionalBytes);
bool nvCommitCuvidBitstreamLength(NVContext *ctx, CUVIDPICPARAMS *picParams);
size_t nvBuildVP8FrameHeader(uint8_t header[10], bool keyFrame,
                             uint8_t version, bool showFrame,
                             uint32_t firstPartitionSize,
                             uint16_t width, uint16_t height);

typedef struct
{
    VAProfile               profile;
    VAEntrypoint            entrypoint;
    cudaVideoSurfaceFormat  surfaceFormat;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    cudaVideoCodec          cudaCodec;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);
typedef void (*CodecBeginPictureFunc)(NVContext*);
typedef void (*CodecDestroyFunc)(NVContext*);

typedef struct {
    size_t minElementSize;
    uint32_t minElements;
    uint32_t maxElements; /* zero means no schema-specific maximum */
} BufferSchema;

#define NVD_BUFFER_SCHEMA(type, minimum, maximum) \
    { .minElementSize = sizeof(type), .minElements = (minimum), \
      .maxElements = (maximum) }
#define NVD_BUFFER_SCHEMA_BYTES(minimumSize, minimum, maximum) \
    { .minElementSize = (minimumSize), .minElements = (minimum), \
      .maxElements = (maximum) }

bool nvValidateBufferSchema(const NVBuffer *buffer,
                            const BufferSchema *schema);
int nvdSelectPictureIndex(uint32_t allocatedCount, uint32_t surfaceLimit,
                          uint32_t activeMask, uint32_t busyMask,
                          uint32_t recycleCursor);

// Internals exposed for the stats subsystem (src/stats.c).
pid_t nv_gettid(void);
FILE *nvStatsOutput(void);

//padding/alignment is very important to this structure as it's placed in it's own section
//in the executable.
struct _NVCodec {
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    BufferSchema        schemas[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
    CodecBeginPictureFunc beginPicture;
    CodecDestroyFunc      destroy;
};

typedef struct _NVCodec NVCodec;

typedef struct
{
    uint32_t bppc; // bytes per pixel per channel
    uint32_t numPlanes;
    uint32_t fourcc;
    bool     is16bits;
    bool     isYuv444;
    NVFormatPlane plane[3];
    VAImageFormat vaFormat;
} NVFormatInfo;

extern const NVFormatInfo formatsInfo[];

int pictureIdxFromSurfaceId(NVContext *current, VASurfaceID surf);
NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf);
const char *nvColorStandardName(VAProcColorStandardType colorStandard);
VAProcColorStandardType nvColorStandardFromMatrixCoefficients(uint8_t matrixCoefficients);
void nvSurfaceResetColorMetadata(NVSurface *surface);
void nvSurfaceSetColorMetadata(NVSurface *surface, VAProcColorStandardType colorStandard, bool colorRangeFull);
void nvSurfaceCopyColorMetadata(NVSurface *dst, const NVSurface *src);
void nvSurfaceCopyColorMetadataFromBackingImage(NVSurface *surface, const BackingImage *img);
void nvBackingImageStoreSurfaceColorMetadata(BackingImage *img, const NVSurface *surface);
void nvBackingImageCopyColorMetadata(BackingImage *dst, const BackingImage *src);
bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line);
void logger(const char *filename, const char *function, int line, const char *msg, ...);
bool nvdLogDebugEnabled(void);
typedef enum {
    NVD_EXPORT_LAYOUT_PER_PLANE_NATURAL = 0,
    NVD_EXPORT_LAYOUT_PER_PLANE_SHARED_MODIFIER,
    NVD_EXPORT_LAYOUT_PACKED,
} NVDExportLayout;

NVDExportLayout nvdGetExportLayout(void);
// Compatibility helper for the legacy packed-layout selector.
bool nvdUseSingleBufferExport(void);
#define CHECK_CUDA_RESULT(err) checkCudaErrors(err, __FILE__, __func__, __LINE__)
#define CHECK_CUDA_RESULT_RETURN(err, ret) if (checkCudaErrors(err, __FILE__, __func__, __LINE__)) { return ret; }
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(...) logger(__FILE__, __func__, __LINE__, __VA_ARGS__);
#define LOG_DEBUG(...) do { if (nvdLogDebugEnabled()) { logger(__FILE__, __func__, __LINE__, __VA_ARGS__); } } while (0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTROFF(base, bytes) ((void *)((unsigned char *)(base) + (bytes)))
#define DECLARE_CODEC(name) \
    __attribute__((used)) \
    __attribute__((retain)) \
    __attribute__((section("nvd_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#define DECLARE_DISABLED_CODEC(name) \
    __attribute__((section("nvd_disabled_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#endif // VABACKEND_H
