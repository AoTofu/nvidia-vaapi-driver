#define _GNU_SOURCE
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
#include <va/va_vpp.h>

// VideoProc pixel checks on a 64x66 surface:
// - host (memfd) -> device and device -> device same-format blits copy exactly;
// - NV12/P010 -> BGRA for the full surface and an odd 63x65 region match
//   between the direct CUDA-array kernel (device source) and the CPU fallback
//   (host source), and NV12 matches a BT.709 limited-range reference.

enum { W = 64, H = 66 };

static VADisplay dpy;
static VAConfigID config;

static void check(VAStatus status, const char *what) {
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "%s failed: %s\n", what, vaErrorStr(status));
        exit(1);
    }
}

static VASurfaceID createSurface(unsigned rt, unsigned fourcc, VASurfaceAttrib *extra, unsigned extraCount) {
    VASurfaceAttrib attrs[3] = {
        {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = fourcc}},
    };
    for (unsigned i = 0; i < extraCount; i++) attrs[1 + i] = extra[i];
    VASurfaceID surface;
    check(vaCreateSurfaces(dpy, rt, W, H, &surface, 1, attrs, 1 + extraCount), "vaCreateSurfaces");
    return surface;
}

static void blit(VAContextID context, VASurfaceID src, VASurfaceID dst, int w, int h) {
    VARectangle region = {0, 0, w, h};
    VAProcPipelineParameterBuffer pipeline = {.surface = src, .surface_region = &region,
        .output_region = &region, .surface_color_standard = VAProcColorStandardBT709,
        .output_color_standard = VAProcColorStandardBT709};
    VABufferID buffer;
    check(vaCreateBuffer(dpy, context, VAProcPipelineParameterBufferType, sizeof(pipeline), 1,
                         &pipeline, &buffer), "vaCreateBuffer");
    check(vaBeginPicture(dpy, context, dst), "vaBeginPicture");
    check(vaRenderPicture(dpy, context, &buffer, 1), "vaRenderPicture");
    check(vaEndPicture(dpy, context), "vaEndPicture");
    check(vaSyncSurface(dpy, dst), "vaSyncSurface");
    check(vaDestroyBuffer(dpy, buffer), "vaDestroyBuffer");
}

// Copies the surface into tightly packed rows of rowBytes per plane.
static void readSurface(VASurfaceID surface, unsigned fourcc, unsigned planes,
                        const unsigned rowBytes[2], const unsigned rows[2], uint8_t *out) {
    VAImageFormat format = {.fourcc = fourcc, .byte_order = VA_LSB_FIRST};
    VAImage image;
    check(vaCreateImage(dpy, &format, W, H, &image), "vaCreateImage");
    check(vaGetImage(dpy, surface, 0, 0, W, H, image.image_id), "vaGetImage");
    uint8_t *data;
    check(vaMapBuffer(dpy, image.buf, (void **) &data), "vaMapBuffer");
    for (unsigned p = 0; p < planes; p++) {
        for (unsigned y = 0; y < rows[p]; y++)
            memcpy(out + y * rowBytes[p], data + image.offsets[p] + y * image.pitches[p], rowBytes[p]);
        out += rowBytes[p] * rows[p];
    }
    check(vaUnmapBuffer(dpy, image.buf), "vaUnmapBuffer");
    check(vaDestroyImage(dpy, image.image_id), "vaDestroyImage");
}

static uint8_t clampU8(double value) {
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t) (value + 0.5);
}

static int run(unsigned bpp) {
    const unsigned fourcc = bpp == 1 ? VA_FOURCC_NV12 : VA_FOURCC_P010;
    const unsigned rt = bpp == 1 ? VA_RT_FORMAT_YUV420 : VA_RT_FORMAT_YUV420_10;
    const unsigned pitch = W * bpp, size = pitch * H * 3 / 2;
    int fd = memfd_create("videoproc-pixels", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) exit(1);
    uint8_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) exit(1);
    for (unsigned y = 0; y < H * 3 / 2; y++) {
        for (unsigned x = 0; x < W; x++) {
            // Luma ramps; U and V differ per pair and per chroma row so a
            // misindexed odd-width chroma row changes the output.
            const unsigned value = y < H ? 16 + (x * 3 + y * 5) % 200
                : (x & 1) ? 40 + x * 3 + (y - H) : 220 - x * 2 - (y - H);
            if (bpp == 1) pixels[y * pitch + x] = (uint8_t) value;
            else ((uint16_t *) (pixels + y * pitch))[x] = (uint16_t) (value << 8);
        }
    }
    uintptr_t buffer = fd;
    VASurfaceAttribExternalBuffers external = {.pixel_format = fourcc, .width = W, .height = H,
        .data_size = size, .num_planes = 2, .pitches = {pitch, pitch},
        .offsets = {0, pitch * H}, .buffers = &buffer, .num_buffers = 1};
    VASurfaceAttrib importAttrs[] = {
        {.type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypeInteger, .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME}},
        {.type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
         .value = {.type = VAGenericValueTypePointer, .value.p = &external}},
    };
    VASurfaceID host = createSurface(rt, fourcc, importAttrs, 2);
    VASurfaceID device = createSurface(rt, fourcc, NULL, 0);
    VASurfaceID copy = createSurface(rt, fourcc, NULL, 0);
    VASurfaceID rgb = createSurface(VA_RT_FORMAT_RGB32, VA_FOURCC_BGRA, NULL, 0);
    VASurfaceID targets[] = {device, copy, rgb};
    VAContextID context;
    check(vaCreateContext(dpy, config, W, H, 0, targets, 3, &context), "vaCreateContext");

    const unsigned yuvRowBytes[2] = {pitch, pitch}, yuvRows[2] = {H, H / 2};
    uint8_t *yuv = malloc(size);
    blit(context, host, device, W, H);
    readSurface(device, fourcc, 2, yuvRowBytes, yuvRows, yuv);
    if (memcmp(yuv, pixels, size)) {
        fprintf(stderr, "host->device blit mismatch (bpp=%u)\n", bpp);
        return 1;
    }
    blit(context, device, copy, W, H);
    readSurface(copy, fourcc, 2, yuvRowBytes, yuvRows, yuv);
    if (memcmp(yuv, pixels, size)) {
        fprintf(stderr, "device->device blit mismatch (bpp=%u)\n", bpp);
        return 1;
    }

    const unsigned rgbRowBytes[2] = {W * 4, 0}, rgbRows[2] = {H, 0};
    uint8_t *gpu = calloc(W * 4, H), *cpu = calloc(W * 4, H);
    for (unsigned odd = 0; odd < 2; odd++) {
        const int w = odd ? W - 1 : W, h = odd ? H - 1 : H;
        blit(context, device, rgb, w, h);  // direct CUDA-array kernel
        readSurface(rgb, VA_FOURCC_ARGB, 1, rgbRowBytes, rgbRows, gpu);
        blit(context, host, rgb, w, h);    // CPU fallback (host source)
        readSurface(rgb, VA_FOURCC_ARGB, 1, rgbRowBytes, rgbRows, cpu);
        for (int y = 0; y < h; y++) {
            if (memcmp(gpu + y * W * 4, cpu + y * W * 4, w * 4)) {
                fprintf(stderr, "CUDA and CPU VideoProc differ: bpp=%u %dx%d row=%d\n", bpp, w, h, y);
                return 1;
            }
            for (int x = 0; bpp == 1 && x < w; x++) {
                const uint8_t *luma = pixels, *chroma = pixels + pitch * H + (y / 2) * pitch;
                const double c = luma[y * pitch + x] - 16.0;
                const double d = chroma[x & ~1] - 128.0, e = chroma[(x & ~1) + 1] - 128.0;
                const uint8_t ref[3] = {clampU8(1.164 * c + 2.112 * d),
                                        clampU8(1.164 * c - 0.213 * d - 0.533 * e),
                                        clampU8(1.164 * c + 1.793 * e)};
                for (int k = 0; k < 3; k++) {
                    if (abs(gpu[y * W * 4 + x * 4 + k] - ref[k]) > 1) {
                        fprintf(stderr, "NV12 VideoProc differs from BT.709: %dx%d (%d,%d) channel %d: %u vs %u\n",
                                w, h, x, y, k, gpu[y * W * 4 + x * 4 + k], ref[k]);
                        return 1;
                    }
                }
            }
        }
    }
    free(gpu);
    free(cpu);
    free(yuv);
    check(vaDestroyContext(dpy, context), "vaDestroyContext");
    VASurfaceID all[] = {host, device, copy, rgb};
    check(vaDestroySurfaces(dpy, all, 4), "vaDestroySurfaces");
    munmap(pixels, size);
    close(fd);
    return 0;
}

int main(void) {
    const char *enabled = getenv("NVD_RUN_GPU_TESTS");
    if (!enabled || strcmp(enabled, "1")) return 77;
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    dpy = vaGetDisplayDRM(fd);
    int major, minor;
    check(vaInitialize(dpy, &major, &minor), "vaInitialize");
    check(vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &config), "vaCreateConfig");
    if (run(1) || run(2)) return 1;
    check(vaDestroyConfig(dpy, config), "vaDestroyConfig");
    check(vaTerminate(dpy), "vaTerminate");
    close(fd);
    puts("VideoProc blits and NV12/P010 -> BGRA match across CUDA and CPU paths, including odd regions");
    return 0;
}
