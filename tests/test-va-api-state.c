#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>

static void requireStatus(const char *operation, VAStatus actual, VAStatus expected) {
    if (actual != expected) {
        fprintf(stderr, "%s: expected %s, got %s\n", operation,
                vaErrorStr(expected), vaErrorStr(actual));
        exit(EXIT_FAILURE);
    }
}

static void requireFailure(const char *operation, VAStatus status) {
    if (status == VA_STATUS_SUCCESS) {
        fprintf(stderr, "%s unexpectedly succeeded\n", operation);
        exit(EXIT_FAILURE);
    }
}

typedef struct {
    VADisplay display;
    VAConfigID config;
    atomic_bool stop;
    atomic_bool failed;
    atomic_uint iterations;
} ConfigQueryRace;

static void *queryConfigUntilDestroyed(void *opaque) {
    ConfigQueryRace *race = opaque;
    while (!atomic_load_explicit(&race->stop, memory_order_acquire)) {
        VAProfile profile = VAProfileNone;
        VAEntrypoint entrypoint = 0;
        VAConfigAttrib attrib = {0};
        int numAttribs = 0;
        VAStatus status = vaQueryConfigAttributes(
            race->display, race->config, &profile, &entrypoint, &attrib,
            &numAttribs);
        if (status != VA_STATUS_SUCCESS &&
            status != VA_STATUS_ERROR_INVALID_CONFIG) {
            atomic_store_explicit(&race->failed, true, memory_order_release);
            break;
        }
        atomic_fetch_add_explicit(&race->iterations, 1U,
                                  memory_order_relaxed);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
        return EXIT_FAILURE;
    }

    VADisplay display = vaGetDisplayDRM(fd);
    int major = 0;
    int minor = 0;
    requireStatus("vaInitialize", vaInitialize(display, &major, &minor),
                  VA_STATUS_SUCCESS);

    VAEntrypoint unsupportedEntrypoints[4] = {0};
    int unsupportedEntrypointCount = 4;
    requireStatus("reject unadvertised H264 Baseline profile",
                  vaQueryConfigEntrypoints(display, (VAProfile) 5,
                                           unsupportedEntrypoints,
                                           &unsupportedEntrypointCount),
                  VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    VAConfigID config = VA_INVALID_ID;
    requireStatus("vaCreateConfig",
                  vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc,
                                 NULL, 0, &config),
                  VA_STATUS_SUCCESS);
    requireStatus("query config with NULL outputs",
                  vaQueryConfigAttributes(display, config, NULL, NULL,
                                          NULL, NULL),
                  VA_STATUS_ERROR_INVALID_PARAMETER);

    VASurfaceID invalidSurface = VA_INVALID_ID;
    requireFailure("zero-width vaCreateSurfaces",
                   vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, 0, 64,
                                    &invalidSurface, 1, NULL, 0));

    VASurfaceID surfaces[2] = { VA_INVALID_ID, VA_INVALID_ID };
    requireStatus("vaCreateSurfaces",
                  vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, 64, 64,
                                   surfaces, 2, NULL, 0),
                  VA_STATUS_SUCCESS);

    VAContextID zeroSizeVppContext = VA_INVALID_ID;
    requireStatus("zero-size VideoProc vaCreateContext",
                  vaCreateContext(display, config, 0, 0, 0, NULL, 0,
                                  &zeroSizeVppContext),
                  VA_STATUS_SUCCESS);
    requireStatus("destroy zero-size VideoProc context",
                  vaDestroyContext(display, zeroSizeVppContext),
                  VA_STATUS_SUCCESS);

    VAContextID invalidContext = VA_INVALID_ID;
    requireFailure("mixed zero/non-zero VideoProc vaCreateContext",
                   vaCreateContext(display, config, 0, 64, 0, surfaces, 2,
                                   &invalidContext));

    VAContextID context = VA_INVALID_ID;
    requireStatus("vaCreateContext",
                  vaCreateContext(display, config, 64, 64, 0, surfaces, 2,
                                  &context),
                  VA_STATUS_SUCCESS);

    requireFailure("EndPicture before BeginPicture", vaEndPicture(display, context));
    requireFailure("RenderPicture before BeginPicture",
                   vaRenderPicture(display, context, NULL, 0));
    requireStatus("DestroyConfig with context ID",
                  vaDestroyConfig(display, (VAConfigID) context),
                  VA_STATUS_ERROR_INVALID_CONFIG);
    requireStatus("DestroyConfig with surface ID",
                  vaDestroyConfig(display, (VAConfigID) surfaces[0]),
                  VA_STATUS_ERROR_INVALID_CONFIG);

    requireStatus("BeginPicture", vaBeginPicture(display, context, surfaces[0]),
                  VA_STATUS_SUCCESS);
    requireFailure("second BeginPicture",
                   vaBeginPicture(display, context, surfaces[1]));
    requireStatus("EndPicture", vaEndPicture(display, context), VA_STATUS_SUCCESS);
    requireFailure("second EndPicture", vaEndPicture(display, context));

    VAProcPipelineParameterBuffer pipeline = { .surface = surfaces[0] };
    VABufferID pipelineBuffer = VA_INVALID_ID;
    requireStatus("create in-place pipeline buffer",
                  vaCreateBuffer(display, context,
                                 VAProcPipelineParameterBufferType,
                                 sizeof(pipeline), 1, &pipeline, &pipelineBuffer),
                  VA_STATUS_SUCCESS);
    requireStatus("begin in-place VideoProc",
                  vaBeginPicture(display, context, surfaces[0]), VA_STATUS_SUCCESS);
    requireFailure("in-place VideoProc",
                   vaRenderPicture(display, context, &pipelineBuffer, 1));
    requireFailure("end failed in-place VideoProc", vaEndPicture(display, context));
    requireStatus("sync failed in-place VideoProc",
                  vaSyncSurface(display, surfaces[0]),
                  VA_STATUS_ERROR_DECODING_ERROR);
    requireStatus("destroy in-place pipeline buffer",
                  vaDestroyBuffer(display, pipelineBuffer), VA_STATUS_SUCCESS);

    VARectangle oversized = { .x = 0, .y = 0, .width = 65, .height = 64 };
    pipeline = (VAProcPipelineParameterBuffer) {
        .surface = surfaces[0],
        .surface_region = &oversized,
    };
    pipelineBuffer = VA_INVALID_ID;
    requireStatus("create oversized pipeline buffer",
                  vaCreateBuffer(display, context,
                                 VAProcPipelineParameterBufferType,
                                 sizeof(pipeline), 1, &pipeline, &pipelineBuffer),
                  VA_STATUS_SUCCESS);
    requireStatus("begin oversized VideoProc",
                  vaBeginPicture(display, context, surfaces[1]), VA_STATUS_SUCCESS);
    requireFailure("oversized VideoProc",
                   vaRenderPicture(display, context, &pipelineBuffer, 1));
    requireFailure("end failed oversized VideoProc", vaEndPicture(display, context));
    requireStatus("sync failed oversized VideoProc",
                  vaSyncSurface(display, surfaces[1]),
                  VA_STATUS_ERROR_DECODING_ERROR);
    requireStatus("destroy oversized pipeline buffer",
                  vaDestroyBuffer(display, pipelineBuffer), VA_STATUS_SUCCESS);

    for (unsigned int i = 0; i < 2; i++) {
        requireStatus("begin completion reset",
                      vaBeginPicture(display, context, surfaces[i]),
                      VA_STATUS_SUCCESS);
        requireStatus("render empty completion reset",
                      vaRenderPicture(display, context, NULL, 0),
                      VA_STATUS_SUCCESS);
        requireStatus("end completion reset", vaEndPicture(display, context),
                      VA_STATUS_SUCCESS);
        requireStatus("sync completion reset",
                      vaSyncSurface(display, surfaces[i]), VA_STATUS_SUCCESS);
    }

    pipeline = (VAProcPipelineParameterBuffer) { .surface = surfaces[0] };
    pipelineBuffer = VA_INVALID_ID;
    requireStatus("create normal pipeline buffer",
                  vaCreateBuffer(display, context,
                                 VAProcPipelineParameterBufferType,
                  sizeof(pipeline), 1, &pipeline, &pipelineBuffer),
                  VA_STATUS_SUCCESS);
    requireStatus("DestroyConfig with buffer ID",
                  vaDestroyConfig(display, (VAConfigID) pipelineBuffer),
                  VA_STATUS_ERROR_INVALID_CONFIG);
    requireStatus("map buffer with NULL output",
                  vaMapBuffer(display, pipelineBuffer, NULL),
                  VA_STATUS_ERROR_INVALID_PARAMETER);
    requireStatus("unmap invalid buffer",
                  vaUnmapBuffer(display, VA_INVALID_ID),
                  VA_STATUS_ERROR_INVALID_BUFFER);
    requireStatus("begin normal VideoProc",
                  vaBeginPicture(display, context, surfaces[1]), VA_STATUS_SUCCESS);
    requireStatus("normal VideoProc",
                  vaRenderPicture(display, context, &pipelineBuffer, 1),
                  VA_STATUS_SUCCESS);
    requireStatus("end normal VideoProc", vaEndPicture(display, context),
                  VA_STATUS_SUCCESS);
    requireStatus("sync normal VideoProc", vaSyncSurface(display, surfaces[1]),
                  VA_STATUS_SUCCESS);
    requireStatus("destroy normal pipeline buffer",
                  vaDestroyBuffer(display, pipelineBuffer), VA_STATUS_SUCCESS);

    VAContextID foreignContext = VA_INVALID_ID;
    requireStatus("create foreign context",
                  vaCreateContext(display, config, 64, 64, 0, surfaces, 2,
                                  &foreignContext),
                  VA_STATUS_SUCCESS);
    pipeline = (VAProcPipelineParameterBuffer) { .surface = surfaces[0] };
    VABufferID foreignBuffer = VA_INVALID_ID;
    requireStatus("create foreign pipeline buffer",
                  vaCreateBuffer(display, foreignContext,
                                 VAProcPipelineParameterBufferType,
                                 sizeof(pipeline), 1, &pipeline, &foreignBuffer),
                  VA_STATUS_SUCCESS);
    requireStatus("begin foreign-buffer test",
                  vaBeginPicture(display, context, surfaces[1]), VA_STATUS_SUCCESS);
    requireStatus("reject foreign-context buffer",
                  vaRenderPicture(display, context, &foreignBuffer, 1),
                  VA_STATUS_ERROR_INVALID_BUFFER);
    requireStatus("end failed foreign-buffer picture",
                  vaEndPicture(display, context), VA_STATUS_ERROR_INVALID_BUFFER);
    requireStatus("destroy foreign pipeline buffer",
                  vaDestroyBuffer(display, foreignBuffer), VA_STATUS_SUCCESS);
    requireStatus("destroy foreign context",
                  vaDestroyContext(display, foreignContext), VA_STATUS_SUCCESS);
    requireStatus("begin post-foreign completion reset",
                  vaBeginPicture(display, context, surfaces[1]), VA_STATUS_SUCCESS);
    requireStatus("render post-foreign completion reset",
                  vaRenderPicture(display, context, NULL, 0), VA_STATUS_SUCCESS);
    requireStatus("end post-foreign completion reset",
                  vaEndPicture(display, context), VA_STATUS_SUCCESS);

    VAImageFormat imageFormat = {
        .fourcc = VA_FOURCC_ARGB,
        .byte_order = VA_LSB_FIRST,
        .bits_per_pixel = 32,
    };
    VAImage image = {0};
    requireStatus("create image", vaCreateImage(display, &imageFormat, 8, 8, &image),
                  VA_STATUS_SUCCESS);
    requireStatus("get image from detached VPP output",
                  vaGetImage(display, surfaces[1], 0, 0, 8, 8, image.image_id),
                  VA_STATUS_SUCCESS);
    requireStatus("reject direct destroy of image-owned buffer",
                  vaDestroyBuffer(display, image.buf),
                  VA_STATUS_ERROR_OPERATION_FAILED);
    void *imageData = NULL;
    requireStatus("map image-owned buffer", vaMapBuffer(display, image.buf, &imageData),
                  VA_STATUS_SUCCESS);
    if (imageData == NULL) {
        fprintf(stderr, "mapped image buffer is NULL\n");
        return EXIT_FAILURE;
    }
    requireStatus("unmap image-owned buffer", vaUnmapBuffer(display, image.buf),
                  VA_STATUS_SUCCESS);
    const VABufferID imageBuffer = image.buf;
    requireStatus("destroy image", vaDestroyImage(display, image.image_id),
                  VA_STATUS_SUCCESS);
    requireStatus("image buffer removed with image",
                  vaMapBuffer(display, imageBuffer, &imageData),
                  VA_STATUS_ERROR_INVALID_BUFFER);

    VAConfigID av1Config = VA_INVALID_ID;
    requireStatus("create AV1 config",
                  vaCreateConfig(display, VAProfileAV1Profile0,
                                 VAEntrypointVLD, NULL, 0, &av1Config),
                  VA_STATUS_SUCCESS);
    VAContextID av1Context = VA_INVALID_ID;
    requireStatus("create AV1 churn context",
                  vaCreateContext(display, av1Config, 64, 64, 0, NULL, 0,
                                  &av1Context),
                  VA_STATUS_SUCCESS);
    pipeline = (VAProcPipelineParameterBuffer) { .surface = surfaces[0] };
    VABufferID vppOwnedBuffer = VA_INVALID_ID;
    requireStatus("create VPP-owned churn abort buffer",
                  vaCreateBuffer(display, context,
                                 VAProcPipelineParameterBufferType,
                                 sizeof(pipeline), 1, &pipeline, &vppOwnedBuffer),
                  VA_STATUS_SUCCESS);
    for (unsigned int i = 0; i < 40; i++) {
        VASurfaceID churnSurface = VA_INVALID_ID;
        requireStatus("create AV1 churn surface",
                      vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 64, 64,
                                       &churnSurface, 1, NULL, 0),
                      VA_STATUS_SUCCESS);
        requireStatus("begin AV1 churn picture",
                      vaBeginPicture(display, av1Context, churnSurface),
                      VA_STATUS_SUCCESS);
        requireStatus("abort AV1 churn with foreign buffer",
                      vaRenderPicture(display, av1Context, &vppOwnedBuffer, 1),
                      VA_STATUS_ERROR_INVALID_BUFFER);
        requireStatus("finish aborted AV1 churn picture",
                      vaEndPicture(display, av1Context),
                      VA_STATUS_ERROR_INVALID_BUFFER);
        requireStatus("destroy AV1 churn surface",
                      vaDestroySurfaces(display, &churnSurface, 1),
                      VA_STATUS_SUCCESS);
    }
    requireStatus("destroy VPP-owned churn abort buffer",
                  vaDestroyBuffer(display, vppOwnedBuffer), VA_STATUS_SUCCESS);
    requireStatus("destroy AV1 churn context",
                  vaDestroyContext(display, av1Context), VA_STATUS_SUCCESS);
    requireStatus("destroy AV1 config", vaDestroyConfig(display, av1Config),
                  VA_STATUS_SUCCESS);

    VASurfaceID transactionSurfaces[2] = { VA_INVALID_ID, VA_INVALID_ID };
    requireStatus("create destroy-transaction surfaces",
                  vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, 16, 16,
                                   transactionSurfaces, 2, NULL, 0),
                  VA_STATUS_SUCCESS);
    VASurfaceID invalidDestroyList[2] = {
        transactionSurfaces[0], VA_INVALID_ID
    };
    requireStatus("reject partially invalid surface destroy",
                  vaDestroySurfaces(display, invalidDestroyList, 2),
                  VA_STATUS_ERROR_INVALID_SURFACE);
    requireStatus("first surface survived invalid transaction",
                  vaSyncSurface(display, transactionSurfaces[0]),
                  VA_STATUS_SUCCESS);
    VASurfaceID duplicateDestroyList[2] = {
        transactionSurfaces[0], transactionSurfaces[0]
    };
    requireStatus("reject duplicate surface destroy",
                  vaDestroySurfaces(display, duplicateDestroyList, 2),
                  VA_STATUS_ERROR_INVALID_SURFACE);
    requireStatus("surface survived duplicate transaction",
                  vaSyncSurface(display, transactionSurfaces[0]),
                  VA_STATUS_SUCCESS);
    requireStatus("destroy transaction surfaces",
                  vaDestroySurfaces(display, transactionSurfaces, 2),
                  VA_STATUS_SUCCESS);

    VAConfigID raceConfig = VA_INVALID_ID;
    requireStatus("create config lifetime race",
                  vaCreateConfig(display, VAProfileNone,
                                 VAEntrypointVideoProc, NULL, 0,
                                 &raceConfig),
                  VA_STATUS_SUCCESS);
    ConfigQueryRace race = {
        .display = display,
        .config = raceConfig,
    };
    atomic_init(&race.stop, false);
    atomic_init(&race.failed, false);
    atomic_init(&race.iterations, 0U);
    pthread_t queryThread;
    if (pthread_create(&queryThread, NULL, queryConfigUntilDestroyed,
                       &race) != 0) {
        perror("pthread_create config lifetime race");
        return EXIT_FAILURE;
    }
    while (atomic_load_explicit(&race.iterations, memory_order_relaxed) < 1000U &&
           !atomic_load_explicit(&race.failed, memory_order_acquire)) {
        sched_yield();
    }
    requireStatus("destroy config during concurrent query",
                  vaDestroyConfig(display, raceConfig), VA_STATUS_SUCCESS);
    atomic_store_explicit(&race.stop, true, memory_order_release);
    if (pthread_join(queryThread, NULL) != 0 ||
        atomic_load_explicit(&race.failed, memory_order_acquire)) {
        fprintf(stderr, "config lifetime race failed\n");
        return EXIT_FAILURE;
    }
    VAProfile destroyedProfile = VAProfileNone;
    VAEntrypoint destroyedEntrypoint = 0;
    VAConfigAttrib destroyedAttrib = {0};
    int destroyedNumAttribs = 0;
    requireStatus("query destroyed race config",
                  vaQueryConfigAttributes(display, raceConfig,
                                          &destroyedProfile,
                                          &destroyedEntrypoint,
                                          &destroyedAttrib,
                                          &destroyedNumAttribs),
                  VA_STATUS_ERROR_INVALID_CONFIG);

    requireStatus("vaDestroyContext", vaDestroyContext(display, context),
                  VA_STATUS_SUCCESS);
    requireStatus("vaDestroySurfaces", vaDestroySurfaces(display, surfaces, 2),
                  VA_STATUS_SUCCESS);
    requireStatus("vaDestroyConfig", vaDestroyConfig(display, config),
                  VA_STATUS_SUCCESS);
    requireStatus("vaTerminate", vaTerminate(display), VA_STATUS_SUCCESS);
    close(fd);

    printf("VA-API state probe passed on %s (VA-API %d.%d)\n", device, major, minor);
    return EXIT_SUCCESS;
}
