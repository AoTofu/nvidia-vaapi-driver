#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

static void check(VAStatus status) {
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "VA operation failed: %s\n", vaErrorStr(status));
        exit(1);
    }
}

static unsigned fdCount(void) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) exit(1);
    unsigned count = 0;
    while (readdir(dir)) count++;
    closedir(dir);
    return count;
}

static void roundtrip(VADisplay dpy, VAConfigID config, unsigned w, unsigned h,
                      unsigned bpp, unsigned seed) {
    const unsigned fourcc = bpp == 1 ? VA_FOURCC_NV12 : VA_FOURCC_P010;
    const unsigned rt = bpp == 1 ? VA_RT_FORMAT_YUV420 : VA_RT_FORMAT_YUV420_10;
    const unsigned pitch = w * bpp, size = pitch * h * 3 / 2;
    int fd = memfd_create("prime-reimport-pixels", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) exit(1);
    uint8_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) exit(1);
    for (unsigned i = 0; i < size / bpp; i++) {
        unsigned value = 16 + (i * 17 + i / w * 31 + seed) % 220;
        if (bpp == 1) pixels[i] = value;
        else ((uint16_t *) pixels)[i] = value << 8;
    }
    uintptr_t buffer = fd;
    VASurfaceAttribExternalBuffers host = {.pixel_format = fourcc, .width = w, .height = h,
        .data_size = size, .num_planes = 2, .pitches = {pitch, pitch},
        .offsets = {0, pitch * h}, .buffers = &buffer, .num_buffers = 1};
    VASurfaceAttrib attrs[] = {
        {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = fourcc}},
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = &host}},
    };
    VASurfaceID source, output;
    check(vaCreateSurfaces(dpy, rt, w, h, &source, 1, attrs, 3));
    check(vaCreateSurfaces(dpy, rt, w, h, &output, 1, attrs, 1));
    VAContextID context;
    check(vaCreateContext(dpy, config, w, h, 0, &output, 1, &context));
    VAProcPipelineParameterBuffer pipeline = {.surface = source};
    VABufferID pipelineBuffer;
    check(vaCreateBuffer(dpy, context, VAProcPipelineParameterBufferType,
                        sizeof(pipeline), 1, &pipeline, &pipelineBuffer));
    check(vaBeginPicture(dpy, context, output));
    check(vaRenderPicture(dpy, context, &pipelineBuffer, 1));
    check(vaEndPicture(dpy, context));
    check(vaSyncSurface(dpy, output));
    check(vaDestroyBuffer(dpy, pipelineBuffer));
    check(vaDestroyContext(dpy, context));
    check(vaDestroySurfaces(dpy, &source, 1));

    VADRMPRIMESurfaceDescriptor desc = {0};
    check(vaExportSurfaceHandle(dpy, output, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
          VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
    if (desc.num_objects != 1 || desc.num_layers != 2) exit(1);
    check(vaDestroySurfaces(dpy, &output, 1));
    // Zero detached-cache capacity ensures the original CUDA mapping is gone.
    // Only this exported FD keeps the patterned image alive.
    buffer = desc.objects[0].fd;
    host.data_size = desc.objects[0].size;
    for (unsigned p = 0; p < 2; p++) {
        host.pitches[p] = desc.layers[p].pitch[0];
        host.offsets[p] = desc.layers[p].offset[0];
    }
    VAImageFormat format = {.fourcc = fourcc, .byte_order = VA_LSB_FIRST};
    VAImage image;
    check(vaCreateImage(dpy, &format, w, h, &image));
    for (unsigned mode = 0; mode < 2; mode++) {
        attrs[1].value.value.i = mode ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
        attrs[2].value.value.p = mode ? (void *) &desc : (void *) &host;
        check(vaCreateSurfaces(dpy, rt, w, h, &output, 1, attrs, 3));
        check(vaGetImage(dpy, output, 0, 0, w, h, image.image_id));
        uint8_t *actual;
        check(vaMapBuffer(dpy, image.buf, (void **) &actual));
        for (unsigned p = 0; p < 2; p++) {
            for (unsigned y = 0; y < (p ? h / 2 : h); y++) {
                if (memcmp(actual + image.offsets[p] + y * image.pitches[p],
                           pixels + (p ? pitch * h : 0) + y * pitch, pitch)) {
                    fprintf(stderr, "Pixel mismatch: %ux%u bpp=%u mode=%u plane=%u row=%u\n",
                            w, h, bpp, mode, p, y);
                    exit(1);
                }
            }
        }
        check(vaUnmapBuffer(dpy, image.buf));
        check(vaDestroySurfaces(dpy, &output, 1));
    }
    check(vaDestroyImage(dpy, image.image_id));
    close(desc.objects[0].fd);
    munmap(pixels, size);
    close(fd);
}

int main(void) {
    const char *enabled = getenv("NVD_RUN_GPU_TESTS");
    if (!enabled || strcmp(enabled, "1")) return 77;
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    VADisplay dpy = vaGetDisplayDRM(fd);
    int major, minor;
    check(vaInitialize(dpy, &major, &minor));
    VAConfigID config;
    check(vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &config));
    unsigned baseline = 0;
    const unsigned sizes[][2] = {{64, 48}, {168, 96}, {854, 480}, {1280, 720}, {1920, 1080}};
    for (unsigned iteration = 0; iteration < 10; iteration++) {
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            for (unsigned bpp = 1; bpp <= 2; bpp++)
                roundtrip(dpy, config, sizes[i][0], sizes[i][1], bpp, iteration * 7);
        unsigned current = fdCount();
        if (iteration == 0) baseline = current;
        else if (current > baseline) {
            fprintf(stderr, "FD count grew: %u -> %u after iteration %u\n", baseline, current, iteration);
            return 1;
        }
    }
    check(vaDestroyConfig(dpy, config));
    check(vaTerminate(dpy));
    close(fd);
    puts("100 patterned buffers survived eviction and both PRIME imports; FD count stable");
    return 0;
}
