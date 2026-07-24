#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NV_STAT_DECODER_CREATES,
    NV_STAT_DECODE_PICTURES,
    NV_STAT_RESOLVE_FRAMES,
    NV_STAT_EXPORT_COPIES,
    NV_STAT_EXPORT_HOST_COPIES,
    NV_STAT_EXPORT_DESCRIPTORS,
    NV_STAT_EXPORT_DESCRIPTORS_SINGLE,
    NV_STAT_EXPORT_DESCRIPTORS_MULTI,
    NV_STAT_VIDEOPROC_REQUESTS,
    NV_STAT_VIDEOPROC_CUDA,
    NV_STAT_VIDEOPROC_CUDA_FAILURES,
    NV_STAT_VIDEOPROC_CPU_FALLBACK,
    NV_STAT_DEVICE_COPY_BYTES,
    NV_STAT_HOST_COPY_BYTES,
    NV_STAT_HOST_FALLBACK_FRAMES,
    NV_STAT_RESOLVE_QUEUE_DEPTH,
    NV_STAT_RESOLVE_QUEUE_HIGH_WATER,
    NV_STAT_RESOLVE_QUEUE_FULL_WAITS,
    NV_STAT_RESOLVE_QUEUE_WAIT_NS,
    NV_STAT_BACKING_ALLOC_COUNT,
    NV_STAT_BACKING_ALLOC_NS,
    NV_STAT_BACKING_PRUNE_COUNT,
    NV_STAT_BACKING_CACHE_HITS,
    NV_STAT_ACTIVE_BACKING_IMAGES,
    NV_STAT_DETACHED_BACKING_IMAGES,
    NV_STAT_BORROWED_BACKING_IMAGES,
    NV_STAT_EXTERNAL_BACKING_IMAGES,
    NV_STAT_ACTIVE_BACKING_BYTES,
    NV_STAT_DETACHED_BACKING_BYTES,
    NV_STAT_ACTIVE_BACKING_BYTES_PEAK,
    NV_STAT_DETACHED_BACKING_BYTES_PEAK,
    NV_STAT_TOTAL_BACKING_BYTES_PEAK,
    NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES,
    NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES_PEAK,
    NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES,
    NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES_PEAK,
    NV_STAT_TRACKED_GPU_BYTES,
    NV_STAT_TRACKED_GPU_BYTES_PEAK,
    NV_STAT_TRACKED_HOST_BYTES,
    NV_STAT_TRACKED_HOST_BYTES_PEAK,
    NV_STAT_VIDEOPROC_NS,
    NV_STAT_OBJECT_LOOKUP_COUNT,
    NV_STAT_OBJECT_LOOKUP_STEPS,
    NV_STAT_BUFFER_POOL_HITS,
    NV_STAT_BUFFER_POOL_MISSES,
    NV_STAT_AV1_COMPACT_COUNT,
    NV_STAT_AV1_COMPACT_BYTES,
    NV_STAT_JPEG_COPY_BYTES,
    NV_STAT_COUNT
} NVStatCounter;

struct _NVDriver;
struct _BackingImage;

// Reads NVD_STATS (and its optional interval) and enables periodic + final
// statistics logging on the driver.
void nvStatsInit(struct _NVDriver *drv);

// Atomically increments a counter. On NV_STAT_DECODE_PICTURES ticks it may emit
// a periodic dump once statsLogInterval pictures have been decoded.
void nvStatsIncrement(struct _NVDriver *drv, NVStatCounter counter);

// Counter and gauge helpers. All are no-ops unless NVD_STATS is enabled.
void nvStatsAdd(struct _NVDriver *drv, NVStatCounter counter, uint64_t value);
void nvStatsSet(struct _NVDriver *drv, NVStatCounter counter, uint64_t value);
void nvStatsSetMax(struct _NVDriver *drv, NVStatCounter counter, uint64_t value);

// Returns CLOCK_MONOTONIC nanoseconds while statistics are enabled, otherwise
// zero. Callers can therefore leave timing instrumentation in hot paths without
// paying for a clock read in normal operation.
uint64_t nvStatsTimestamp(struct _NVDriver *drv);

// Incremental backing-image accounting avoids walking the image list at every
// periodic statistics dump. These functions are idempotent for partial cleanup.
void nvStatsBackingImageCreated(struct _NVDriver *drv, struct _BackingImage *img, bool active);
void nvStatsBackingImageSetActive(struct _NVDriver *drv, struct _BackingImage *img, bool active);
void nvStatsBackingImageDestroyed(struct _NVDriver *drv, struct _BackingImage *img);
void nvStatsUpdateMemoryEstimates(struct _NVDriver *drv);

// Dumps the current counters together with live backing-image accounting to the
// stats log stream. No-op unless stats are enabled.
void nvStatsLog(struct _NVDriver *drv, const char *reason);

#endif
