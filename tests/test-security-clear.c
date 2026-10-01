#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

typedef struct { unsigned fourcc, rt, bytes, planes; } ClearFormat;
static const ClearFormat clearFormats[] = {
    {VA_FOURCC_NV12, VA_RT_FORMAT_YUV420, 1, 2},
    {VA_FOURCC_P010, VA_RT_FORMAT_YUV420_10, 2, 2},
    {VA_FOURCC_P012, VA_RT_FORMAT_YUV420_12, 2, 2},
    {VA_FOURCC_444P, VA_RT_FORMAT_YUV444, 1, 3},
    {VA_FOURCC_ARGB, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_XRGB, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_ABGR, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_XBGR, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_RGBA, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_RGBX, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_BGRA, VA_RT_FORMAT_RGB32, 4, 1},
    {VA_FOURCC_BGRX, VA_RT_FORMAT_RGB32, 4, 1},
};
static void check(VAStatus status) {
    if (status != VA_STATUS_SUCCESS) { fprintf(stderr, "%s\n", vaErrorStr(status)); exit(1); }
}
static uint64_t ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) exit(1);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void verify(VADisplay dpy, VASurfaceID surface, ClearFormat fmt, unsigned w, unsigned h) {
    VAImageFormat format = {.fourcc = fmt.bytes == 4 ? VA_FOURCC_ARGB : fmt.fourcc, .byte_order = VA_LSB_FIRST};
    VAImage image;
    check(vaCreateImage(dpy, &format, w, h, &image));
    check(vaGetImage(dpy, surface, 0, 0, w, h, image.image_id));
    uint8_t *pixels;
    check(vaMapBuffer(dpy, image.buf, (void **)&pixels));
    for (unsigned p = 0; p < fmt.planes; p++) {
        unsigned rows = p && fmt.planes == 2 ? (h + 1) / 2 : h;
        unsigned cols = p && fmt.planes == 2 ? ((w + 1) / 2) * 2 : w;
        for (unsigned y = 0; y < rows; y++) {
            uint8_t *row = pixels + image.offsets[p] + (size_t)y * image.pitches[p];
            for (unsigned x = 0; x < cols; x++) {
                int ok;
                if (fmt.bytes == 4) {
                    // RGB allocation uses its existing zero initialization, not clearBackingImage.
                    const uint8_t expected[] = {0, 0, 0, 0};
                    ok = memcmp(row + x * 4, expected, 4) == 0;
                } else if (fmt.bytes == 2) {
                    uint16_t sample;
                    memcpy(&sample, row + x * 2, 2);
                    ok = sample == (p ? 0x8000 : 0x1000);
                } else ok = row[x] == (p ? 128 : 16);
                if (!ok) { fprintf(stderr, "actual=%02x,%02x,%02x,%02x offset=%u pitch=%u\n",row[x*fmt.bytes],row[x*fmt.bytes+1],row[x*fmt.bytes+2],row[x*fmt.bytes+3],image.offsets[p],image.pitches[p]); fprintf(stderr, "Clear mismatch: fourcc=%08x %ux%u plane=%u x=%u y=%u\n", fmt.fourcc,w,h,p,x,y); exit(1); }
            }
        }
    }
    check(vaUnmapBuffer(dpy, image.buf));
    check(vaDestroyImage(dpy, image.image_id));
}
static void createAndCheck(VADisplay dpy, ClearFormat fmt, unsigned w, unsigned h,
                           uint64_t *wall, uint64_t *cpu) {
    VASurfaceAttrib attr = {.type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {.type = VAGenericValueTypeInteger, .value.i = fmt.fourcc}};
    VASurfaceID surface;
    check(vaCreateSurfaces(dpy, fmt.rt, w, h, &surface, 1, &attr, 1));
    VADRMPRIMESurfaceDescriptor desc;
    uint64_t start = ns(CLOCK_MONOTONIC), cpuStart = ns(CLOCK_THREAD_CPUTIME_ID);
    check(vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
    *cpu = ns(CLOCK_THREAD_CPUTIME_ID) - cpuStart;
    *wall = ns(CLOCK_MONOTONIC) - start;
    verify(dpy, surface, fmt, w, h);
    for (unsigned i = 0; i < desc.num_objects; i++) close(desc.objects[i].fd);
    check(vaDestroySurfaces(dpy, &surface, 1));
}

typedef struct {
    VADisplay dpy;
    ClearFormat format;
    unsigned count, width, height;
    uint64_t *wall, *cpu;
} ClearBenchWorker;
static void *benchWorker(void *opaque) {
    ClearBenchWorker *worker = opaque;
    for (unsigned i = 0; i < worker->count + 5; i++) {
        uint64_t wall, cpu;
        createAndCheck(worker->dpy,worker->format,worker->width,worker->height,&wall,&cpu);
        if (i >= 5) { worker->wall[i-5] = wall; worker->cpu[i-5] = cpu; }
    }
    return NULL;
}
static void runWorkers(VADisplay dpy, ClearFormat fmt, unsigned count, unsigned w, unsigned h,
                        unsigned threads, const char *name, int mixed) {
    pthread_t ids[4];
    ClearBenchWorker workers[4];
    for (unsigned t = 0; t < threads; t++) {
        workers[t] = (ClearBenchWorker){.dpy=dpy,.format=mixed ? clearFormats[t%4] : fmt,
            .count=count,.width=w,.height=h,.wall=calloc(count,sizeof(uint64_t)),.cpu=calloc(count,sizeof(uint64_t))};
        if (!workers[t].wall || !workers[t].cpu || pthread_create(&ids[t],NULL,benchWorker,&workers[t])) exit(1);
    }
    for (unsigned t = 0; t < threads; t++) if (pthread_join(ids[t],NULL)) exit(1);
    if (name) {
        printf("{\"workload\":\"cold-clear-%s\",\"width\":%u,\"height\":%u,\"threads\":%u,\"correct_images\":%u,\"warmup_per_thread\":5,\"cpu_clock\":\"CLOCK_THREAD_CPUTIME_ID\",\"samples\":[",name,w,h,threads,count*threads);
        for (unsigned t = 0; t < threads; t++) for (unsigned i = 0; i < count; i++)
            printf("%s{\"worker\":%u,\"export_wall_ns\":%llu,\"export_cpu_ns\":%llu}",t || i ? "," : "",t,(unsigned long long)workers[t].wall[i],(unsigned long long)workers[t].cpu[i]);
        puts("]}");
    }
    for (unsigned t = 0; t < threads; t++) { free(workers[t].wall); free(workers[t].cpu); }
}
int main(int argc, char **argv) {
    const char *enabled = getenv("NVD_RUN_GPU_TESTS");
    if (!enabled || strcmp(enabled, "1")) return 77;
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    VADisplay dpy = vaGetDisplayDRM(fd);
    int major, minor;
    check(vaInitialize(dpy, &major, &minor));
    if (argc > 1) {
        if ((argc != 6 && argc != 7) || strcmp(argv[1], "--bench")) return 1;
        unsigned f = !strcmp(argv[2], "nv12") ? 0 : !strcmp(argv[2], "p010") ? 1 : !strcmp(argv[2], "argb") ? 4 : 99;
        unsigned count = strtoul(argv[3], NULL, 10), w = strtoul(argv[4], NULL, 10), h = strtoul(argv[5], NULL, 10);
        if (f == 99 || count < 1 || count > 1000 || w < 2 || h < 2 || w > 8192 || h > 8192) return 1;
        unsigned threads = argc == 7 ? strtoul(argv[6],NULL,10) : 1;
        if (threads < 1 || threads > 4) return 1;
        runWorkers(dpy,clearFormats[f],count,w,h,threads,argv[2],0);
    } else {
        const unsigned dimensions[][2] = {{63,65}, {128,128}, {3840,2160}};
        for (unsigned f = 0; f < sizeof(clearFormats)/sizeof(clearFormats[0]); f++)
            for (unsigned d = 0; d < sizeof(dimensions)/sizeof(dimensions[0]); d++) {
                uint64_t wall, cpu;
                createAndCheck(dpy, clearFormats[f], dimensions[d][0], dimensions[d][1], &wall, &cpu);
            }
        runWorkers(dpy,clearFormats[0],4,128,128,4,NULL,1);
        puts("36 fresh images plus mixed parallel clears retain the existing pattern, including odd dimensions and multi-chunk 4K clears");
    }
    check(vaTerminate(dpy));
    close(fd);
    return 0;
}
