#include "stats.h"
#include "vabackend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t statLoad(const NVDriver *drv, NVStatCounter counter) {
    return atomic_load_explicit(&drv->stats[counter], memory_order_relaxed);
}

static const char *const timingNames[NV_TIMING_COUNT] = {
    "decode_submit_cpu", "queue_residence", "map_cpu", "export_cpu",
    "unmap_cpu", "shared_end_wait", "copy_wait_cpu", "lifetime_read_wait",
    "lifetime_write_wait", "security_clear_cpu", "av1_validation_cpu",
};

static void histogramAdd(NVTimingHistogram *histogram, uint64_t ns) {
    unsigned bucket = 0;
    uint64_t upper = 1000;
    while (ns > upper && bucket < NV_TIMING_BUCKETS - 1) {
        upper <<= 1;
        bucket++;
    }
    atomic_fetch_add_explicit(&histogram->buckets[bucket], 1, memory_order_relaxed);
}

void nvStatsObserve(NVDriver *drv, NVTimingStage stage, uint64_t ns) {
    if (drv != NULL && drv->statsEnabled && (unsigned) stage < NV_TIMING_COUNT) {
        histogramAdd(&drv->timings[stage], ns);
    }
}

void nvStatsRecord(NVDriver *drv, NVTimingStage stage, uint64_t start) {
    if (start == 0) return;
    const uint64_t end = nvStatsTimestamp(drv);
    if (end >= start) nvStatsObserve(drv, stage, end - start);
}

void nvStatsRecordContext(NVContext *ctx, NVTimingStage stage,
                          bool shared, uint64_t start) {
    if (ctx == NULL || start == 0 || (unsigned) stage >= NV_TIMING_CONTEXT_COUNT) return;
    const uint64_t end = nvStatsTimestamp(ctx->drv);
    if (end >= start) {
        const uint64_t ns = end - start;
        nvStatsObserve(ctx->drv, stage, ns);
        histogramAdd(&ctx->timings[shared ? 1 : 0][stage], ns);
    }
}

static void logHistogram(FILE *out, const NVTimingHistogram *histogram,
                          const char *scope, unsigned contextId,
                          const char *sharing, NVTimingStage stage) {
    uint64_t bins[NV_TIMING_BUCKETS], count = 0;
    for (unsigned i = 0; i < NV_TIMING_BUCKETS; i++) {
        bins[i] = atomic_load_explicit(&histogram->buckets[i], memory_order_relaxed);
        count += bins[i];
    }
    if (count == 0) return;
    const unsigned percentiles[] = {50, 95, 99};
    uint64_t upper[3] = {0};
    for (unsigned p = 0; p < 3; p++) {
        const uint64_t rank = (count / 100) * percentiles[p] +
            ((count % 100) * percentiles[p] + 99) / 100;
        uint64_t cumulative = 0;
        for (unsigned i = 0; i < NV_TIMING_BUCKETS; i++) {
            cumulative += bins[i];
            if (cumulative >= rank) {
                upper[p] = i == NV_TIMING_BUCKETS - 1 ? UINT64_MAX : UINT64_C(1000) << i;
                break;
            }
        }
    }
    fprintf(out, "Timing[%s]: context=%u sharing=%s stage=%s count=%llu p50_upper_ns=%llu p95_upper_ns=%llu p99_upper_ns=%llu bins=",
            scope, contextId, sharing, timingNames[stage],
            (unsigned long long) count, (unsigned long long) upper[0],
            (unsigned long long) upper[1], (unsigned long long) upper[2]);
    for (unsigned i = 0; i < NV_TIMING_BUCKETS; i++)
        fprintf(out, "%s%llu", i ? "," : "", (unsigned long long) bins[i]);
    fputc('\n', out);
}

void nvStatsContextLog(NVContext *ctx) {
    if (ctx == NULL || ctx->drv == NULL || !ctx->drv->statsEnabled) return;
    FILE *out = nvStatsOutput();
    if (out == NULL) return;
    flockfile(out);
    fprintf(out, "ContextStats: context=%u codec=%d format=%d width=%u height=%u queue_high_water=%zu queue_capacity=%u\n",
            ctx->id, ctx->cudaCodec, ctx->decoderSurfaceFormat,
            ctx->width, ctx->height, ctx->resolveQueue.highWater, RESOLVE_QUEUE_CAPACITY);
    for (unsigned sharing = 0; sharing < 2; sharing++)
        for (unsigned stage = 0; stage < NV_TIMING_CONTEXT_COUNT; stage++)
            logHistogram(out, &ctx->timings[sharing][stage], "context_final", ctx->id,
                          sharing ? "shared" : "private", stage);
    funlockfile(out);
    nvStatsLog(ctx->drv, "context_retired");
}

void nvStatsContextHostBuffers(NVContext *ctx) {
    if (ctx == NULL || ctx->drv == NULL || !ctx->drv->statsEnabled) return;
    const uint64_t bytes = (uint64_t) ctx->bitstreamBuffer.allocated +
        ctx->sliceOffsets.allocated + ctx->sliceParamsBuffer.allocated +
        ctx->av1TileIntervals.storage.allocated;
    if (bytes >= ctx->statsHostBufferBytes)
        nvStatsAdd(ctx->drv, NV_STAT_CONTEXT_HOST_BUFFER_BYTES, bytes - ctx->statsHostBufferBytes);
    else
        nvStatsSubtract(ctx->drv, NV_STAT_CONTEXT_HOST_BUFFER_BYTES, ctx->statsHostBufferBytes - bytes);
    ctx->statsHostBufferBytes = bytes;
    nvStatsUpdateMemoryEstimates(ctx->drv);
}

static uint64_t backingImageStatsSize(const BackingImage *img) {
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

void nvStatsAdd(NVDriver *drv, NVStatCounter counter, uint64_t value) {
    if (drv == NULL || !drv->statsEnabled || counter >= NV_STAT_COUNT || value == 0) {
        return;
    }
    atomic_fetch_add_explicit(&drv->stats[counter], value, memory_order_relaxed);
}

void nvStatsSubtract(NVDriver *drv, NVStatCounter counter, uint64_t value) {
    if (drv != NULL && drv->statsEnabled && (unsigned) counter < NV_STAT_COUNT && value != 0)
        atomic_fetch_sub_explicit(&drv->stats[counter], value, memory_order_relaxed);
}

void nvStatsSet(NVDriver *drv, NVStatCounter counter, uint64_t value) {
    if (drv == NULL || !drv->statsEnabled || counter >= NV_STAT_COUNT) {
        return;
    }
    atomic_store_explicit(&drv->stats[counter], value, memory_order_relaxed);
}

void nvStatsSetMax(NVDriver *drv, NVStatCounter counter, uint64_t value) {
    if (drv == NULL || !drv->statsEnabled || counter >= NV_STAT_COUNT) {
        return;
    }

    uint_fast64_t current = statLoad(drv, counter);
    while (current < value &&
           !atomic_compare_exchange_weak_explicit(&drv->stats[counter], &current, value,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

uint64_t nvStatsTimestamp(NVDriver *drv) {
    if (drv == NULL || !drv->statsEnabled) {
        return 0;
    }

    struct timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC, &tp) != 0) {
        return 0;
    }
    return (uint64_t) tp.tv_sec * 1000000000ULL + (uint64_t) tp.tv_nsec;
}

static void updateBackingPeaks(NVDriver *drv) {
    const uint64_t active = statLoad(drv, NV_STAT_ACTIVE_BACKING_BYTES);
    const uint64_t detached = statLoad(drv, NV_STAT_DETACHED_BACKING_BYTES);
    nvStatsSetMax(drv, NV_STAT_ACTIVE_BACKING_BYTES_PEAK, active);
    nvStatsSetMax(drv, NV_STAT_DETACHED_BACKING_BYTES_PEAK, detached);
    nvStatsSetMax(drv, NV_STAT_TOTAL_BACKING_BYTES_PEAK, active + detached);
    nvStatsUpdateMemoryEstimates(drv);
}

void nvStatsUpdateMemoryEstimates(NVDriver *drv) {
    if (drv == NULL || !drv->statsEnabled) {
        return;
    }
    const uint64_t gpuBytes = statLoad(drv, NV_STAT_ACTIVE_BACKING_BYTES) +
                              statLoad(drv, NV_STAT_DETACHED_BACKING_BYTES) +
                              statLoad(drv, NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES);
    const uint64_t ownedGpuBytes = statLoad(drv, NV_STAT_UNIQUE_OWNED_BACKING_BYTES) +
        statLoad(drv, NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES) +
        statLoad(drv, NV_STAT_SECURITY_CLEAR_SCRATCH_BYTES);
    const uint64_t hostBytes = statLoad(drv, NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES) +
        statLoad(drv, NV_STAT_BUFFER_LIVE_CAPACITY_BYTES) +
        statLoad(drv, NV_STAT_BUFFER_POOL_RETAINED_BYTES) +
        statLoad(drv, NV_STAT_CONTEXT_HOST_BUFFER_BYTES) +
        statLoad(drv, NV_STAT_SECURITY_CLEAR_HOST_BYTES);
    nvStatsSet(drv, NV_STAT_OWNED_GPU_BYTES, ownedGpuBytes);
    nvStatsSetMax(drv, NV_STAT_OWNED_GPU_BYTES_PEAK, ownedGpuBytes);
    nvStatsSet(drv, NV_STAT_TRACKED_GPU_BYTES, gpuBytes);
    nvStatsSetMax(drv, NV_STAT_TRACKED_GPU_BYTES_PEAK, gpuBytes);
    nvStatsSet(drv, NV_STAT_TRACKED_HOST_BYTES, hostBytes);
    nvStatsSetMax(drv, NV_STAT_TRACKED_HOST_BYTES_PEAK, hostBytes);
}

void nvStatsBackingImageCreated(NVDriver *drv, BackingImage *img, bool active) {
    if (drv == NULL || !drv->statsEnabled || img == NULL || img->statsTracked) {
        return;
    }

    img->statsBytes = backingImageStatsSize(img);
    img->statsTracked = true;
    img->statsActive = active;
    nvStatsAdd(drv, active ? NV_STAT_ACTIVE_BACKING_IMAGES : NV_STAT_DETACHED_BACKING_IMAGES, 1);
    nvStatsAdd(drv, active ? NV_STAT_ACTIVE_BACKING_BYTES : NV_STAT_DETACHED_BACKING_BYTES, img->statsBytes);
    img->statsBorrowed = img->borrowedCudaResources || img->borrowedBackingImage != NULL;
    img->statsExternal = img->isExternalBuffer;
    nvStatsAdd(drv, img->statsBorrowed ? NV_STAT_BORROWED_VIEW_BYTES :
        img->statsExternal ? NV_STAT_EXTERNAL_IMPORT_BYTES :
        NV_STAT_UNIQUE_OWNED_BACKING_BYTES, img->statsBytes);
    nvStatsSetMax(drv, NV_STAT_UNIQUE_OWNED_BACKING_BYTES_PEAK,
                  statLoad(drv, NV_STAT_UNIQUE_OWNED_BACKING_BYTES));
    if (img->statsBorrowed) {
        nvStatsAdd(drv, NV_STAT_BORROWED_BACKING_IMAGES, 1);
    }
    if (img->statsExternal) {
        nvStatsAdd(drv, NV_STAT_EXTERNAL_BACKING_IMAGES, 1);
    }
    updateBackingPeaks(drv);
}

void nvStatsBackingImageSetActive(NVDriver *drv, BackingImage *img, bool active) {
    if (drv == NULL || !drv->statsEnabled || img == NULL || !img->statsTracked || img->statsActive == active) {
        return;
    }

    const NVStatCounter oldCount = img->statsActive ? NV_STAT_ACTIVE_BACKING_IMAGES : NV_STAT_DETACHED_BACKING_IMAGES;
    const NVStatCounter newCount = active ? NV_STAT_ACTIVE_BACKING_IMAGES : NV_STAT_DETACHED_BACKING_IMAGES;
    const NVStatCounter oldBytes = img->statsActive ? NV_STAT_ACTIVE_BACKING_BYTES : NV_STAT_DETACHED_BACKING_BYTES;
    const NVStatCounter newBytes = active ? NV_STAT_ACTIVE_BACKING_BYTES : NV_STAT_DETACHED_BACKING_BYTES;
    atomic_fetch_sub_explicit(&drv->stats[oldCount], 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&drv->stats[oldBytes], img->statsBytes, memory_order_relaxed);
    nvStatsAdd(drv, newCount, 1);
    nvStatsAdd(drv, newBytes, img->statsBytes);
    img->statsActive = active;
    updateBackingPeaks(drv);
}

void nvStatsBackingImageDestroyed(NVDriver *drv, BackingImage *img) {
    if (drv == NULL || !drv->statsEnabled || img == NULL || !img->statsTracked) {
        return;
    }

    const NVStatCounter count = img->statsActive ? NV_STAT_ACTIVE_BACKING_IMAGES : NV_STAT_DETACHED_BACKING_IMAGES;
    const NVStatCounter bytes = img->statsActive ? NV_STAT_ACTIVE_BACKING_BYTES : NV_STAT_DETACHED_BACKING_BYTES;
    atomic_fetch_sub_explicit(&drv->stats[count], 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&drv->stats[bytes], img->statsBytes, memory_order_relaxed);
    if (img->statsBorrowed) {
        atomic_fetch_sub_explicit(&drv->stats[NV_STAT_BORROWED_BACKING_IMAGES], 1, memory_order_relaxed);
    }
    if (img->statsExternal) {
        atomic_fetch_sub_explicit(&drv->stats[NV_STAT_EXTERNAL_BACKING_IMAGES], 1, memory_order_relaxed);
    }
    nvStatsSubtract(drv, img->statsBorrowed ? NV_STAT_BORROWED_VIEW_BYTES :
        img->statsExternal ? NV_STAT_EXTERNAL_IMPORT_BYTES :
        NV_STAT_UNIQUE_OWNED_BACKING_BYTES, img->statsBytes);
    img->statsTracked = false;
    img->statsBytes = 0;
    img->statsBorrowed = false;
    img->statsExternal = false;
    nvStatsUpdateMemoryEstimates(drv);
}

void nvStatsLog(NVDriver *drv, const char *reason) {
    if (drv == NULL || !drv->statsEnabled) {
        return;
    }

    FILE *out = nvStatsOutput();
    if (out == NULL) {
        return;
    }

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    flockfile(out);
#define S(counter) ((unsigned long long) statLoad(drv, counter))
    fprintf(out,
        "%10ld.%09ld [%d-%d] Stats[%s]: decoder_creates=%llu decode_surfaces_selected=%llu decode_surfaces_auto_candidate=%llu decode_surfaces_legacy=%llu decode_pictures=%llu resolve_frames=%llu export_copies=%llu export_host_copies=%llu export_descriptors=%llu single_descriptors=%llu multi_descriptors=%llu videoproc_requests=%llu videoproc_cuda=%llu videoproc_cuda_failures=%llu videoproc_cpu_fallback=%llu device_copy_bytes=%llu host_copy_bytes=%llu host_fallback_frames=%llu resolve_queue_depth=%llu resolve_queue_high_water=%llu resolve_queue_full_waits=%llu resolve_queue_wait_ns=%llu backing_alloc_count=%llu backing_alloc_ns=%llu backing_prune_count=%llu backing_cache_hits=%llu active_backing_images=%llu detached_backing_images=%llu borrowed_backing_images=%llu external_backing_images=%llu active_backing_bytes=%llu detached_backing_bytes=%llu active_backing_bytes_peak=%llu detached_backing_bytes_peak=%llu total_backing_bytes_peak=%llu videoproc_gpu_scratch_bytes=%llu videoproc_gpu_scratch_bytes_peak=%llu videoproc_cpu_scratch_bytes=%llu videoproc_cpu_scratch_bytes_peak=%llu tracked_vram_equivalent_bytes=%llu tracked_vram_equivalent_bytes_peak=%llu tracked_ram_equivalent_bytes=%llu tracked_ram_equivalent_bytes_peak=%llu videoproc_ns=%llu videoproc_texture_creates=%llu videoproc_surface_creates=%llu videoproc_object_create_ns=%llu videoproc_mutex_wait_ns=%llu videoproc_direct_array_frames=%llu object_lookup_count=%llu object_lookup_steps=%llu buffer_pool_hits=%llu buffer_pool_misses=%llu buffer_requested_bytes=%llu buffer_capacity_bytes=%llu buffer_internal_fragmentation_bytes=%llu buffer_pool_retained_bytes=%llu security_clear_bytes=%llu security_clear_ns=%llu security_clear_gpu_bytes=%llu security_clear_host_fallbacks=%llu av1_compact_count=%llu av1_compact_bytes=%llu jpeg_copy_bytes=%llu host_buffer_trim_count=%llu host_buffer_trim_bytes=%llu detached_backing_limit_bytes=%llu detached_backing_limit_images=%u memory_budget_bytes=%llu\n",
        (long)tp.tv_sec, tp.tv_nsec, getpid(), nv_gettid(), reason,
        S(NV_STAT_DECODER_CREATES), S(NV_STAT_DECODE_SURFACES_SELECTED),
        S(NV_STAT_DECODE_SURFACES_AUTO_CANDIDATE), S(NV_STAT_DECODE_SURFACES_LEGACY),
        S(NV_STAT_DECODE_PICTURES), S(NV_STAT_RESOLVE_FRAMES),
        S(NV_STAT_EXPORT_COPIES), S(NV_STAT_EXPORT_HOST_COPIES), S(NV_STAT_EXPORT_DESCRIPTORS),
        S(NV_STAT_EXPORT_DESCRIPTORS_SINGLE), S(NV_STAT_EXPORT_DESCRIPTORS_MULTI),
        S(NV_STAT_VIDEOPROC_REQUESTS), S(NV_STAT_VIDEOPROC_CUDA),
        S(NV_STAT_VIDEOPROC_CUDA_FAILURES), S(NV_STAT_VIDEOPROC_CPU_FALLBACK),
        S(NV_STAT_DEVICE_COPY_BYTES), S(NV_STAT_HOST_COPY_BYTES), S(NV_STAT_HOST_FALLBACK_FRAMES),
        S(NV_STAT_RESOLVE_QUEUE_DEPTH), S(NV_STAT_RESOLVE_QUEUE_HIGH_WATER),
        S(NV_STAT_RESOLVE_QUEUE_FULL_WAITS), S(NV_STAT_RESOLVE_QUEUE_WAIT_NS),
        S(NV_STAT_BACKING_ALLOC_COUNT), S(NV_STAT_BACKING_ALLOC_NS),
        S(NV_STAT_BACKING_PRUNE_COUNT), S(NV_STAT_BACKING_CACHE_HITS),
        S(NV_STAT_ACTIVE_BACKING_IMAGES), S(NV_STAT_DETACHED_BACKING_IMAGES),
        S(NV_STAT_BORROWED_BACKING_IMAGES), S(NV_STAT_EXTERNAL_BACKING_IMAGES),
        S(NV_STAT_ACTIVE_BACKING_BYTES), S(NV_STAT_DETACHED_BACKING_BYTES),
        S(NV_STAT_ACTIVE_BACKING_BYTES_PEAK), S(NV_STAT_DETACHED_BACKING_BYTES_PEAK),
        S(NV_STAT_TOTAL_BACKING_BYTES_PEAK), S(NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES),
        S(NV_STAT_VIDEOPROC_GPU_SCRATCH_BYTES_PEAK), S(NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES),
        S(NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES_PEAK), S(NV_STAT_TRACKED_GPU_BYTES),
        S(NV_STAT_TRACKED_GPU_BYTES_PEAK), S(NV_STAT_TRACKED_HOST_BYTES),
        S(NV_STAT_TRACKED_HOST_BYTES_PEAK), S(NV_STAT_VIDEOPROC_NS),
        S(NV_STAT_VIDEOPROC_TEXTURE_CREATES), S(NV_STAT_VIDEOPROC_SURFACE_CREATES),
        S(NV_STAT_VIDEOPROC_OBJECT_CREATE_NS), S(NV_STAT_VIDEOPROC_MUTEX_WAIT_NS),
        S(NV_STAT_VIDEOPROC_DIRECT_ARRAY_FRAMES),
        S(NV_STAT_OBJECT_LOOKUP_COUNT), S(NV_STAT_OBJECT_LOOKUP_STEPS),
        S(NV_STAT_BUFFER_POOL_HITS), S(NV_STAT_BUFFER_POOL_MISSES),
        S(NV_STAT_BUFFER_REQUESTED_BYTES), S(NV_STAT_BUFFER_CAPACITY_BYTES),
        S(NV_STAT_BUFFER_INTERNAL_FRAGMENTATION_BYTES),
        S(NV_STAT_BUFFER_POOL_RETAINED_BYTES), S(NV_STAT_SECURITY_CLEAR_BYTES),
        S(NV_STAT_SECURITY_CLEAR_NS), S(NV_STAT_SECURITY_CLEAR_GPU_BYTES),
        S(NV_STAT_SECURITY_CLEAR_HOST_FALLBACKS),
        S(NV_STAT_AV1_COMPACT_COUNT), S(NV_STAT_AV1_COMPACT_BYTES), S(NV_STAT_JPEG_COPY_BYTES),
        S(NV_STAT_HOST_BUFFER_TRIM_COUNT), S(NV_STAT_HOST_BUFFER_TRIM_BYTES),
        (unsigned long long) drv->maxDetachedBackingImageBytes, drv->maxDetachedBackingImages,
        (unsigned long long) drv->memoryBudgetBytes);
#undef S
    fprintf(out, "MemoryStats[%s]: logical_view_bytes=%llu unique_owned_backing_bytes=%llu unique_owned_backing_bytes_peak=%llu borrowed_view_bytes=%llu external_import_view_bytes=%llu security_clear_scratch_bytes=%llu security_clear_host_bytes=%llu buffer_live_requested_bytes=%llu buffer_live_capacity_bytes=%llu buffer_pool_retained_bytes=%llu context_host_buffer_bytes=%llu owned_gpu_bytes=%llu owned_gpu_bytes_peak=%llu owned_host_bytes=%llu owned_host_bytes_peak=%llu reclaimable_cache_budget_bytes=%llu nvdec_internal_bytes=unknown cuda_internal_bytes=unknown\n",
        reason,
        (unsigned long long) (statLoad(drv, NV_STAT_ACTIVE_BACKING_BYTES) + statLoad(drv, NV_STAT_DETACHED_BACKING_BYTES)),
        (unsigned long long) statLoad(drv, NV_STAT_UNIQUE_OWNED_BACKING_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_UNIQUE_OWNED_BACKING_BYTES_PEAK),
        (unsigned long long) statLoad(drv, NV_STAT_BORROWED_VIEW_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_EXTERNAL_IMPORT_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_SECURITY_CLEAR_SCRATCH_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_SECURITY_CLEAR_HOST_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_BUFFER_LIVE_REQUESTED_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_BUFFER_LIVE_CAPACITY_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_BUFFER_POOL_RETAINED_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_CONTEXT_HOST_BUFFER_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_OWNED_GPU_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_OWNED_GPU_BYTES_PEAK),
        (unsigned long long) statLoad(drv, NV_STAT_TRACKED_HOST_BYTES),
        (unsigned long long) statLoad(drv, NV_STAT_TRACKED_HOST_BYTES_PEAK),
        (unsigned long long) drv->memoryBudgetBytes);
    for (unsigned stage = 0; stage < NV_TIMING_COUNT; stage++)
        logHistogram(out, &drv->timings[stage], reason, VA_INVALID_ID, "all", stage);
    fflush(out);
    funlockfile(out);
}

void nvStatsIncrement(NVDriver *drv, NVStatCounter counter) {
    if (drv == NULL || !drv->statsEnabled || counter >= NV_STAT_COUNT) {
        return;
    }

    uint64_t value = atomic_fetch_add_explicit(&drv->stats[counter], 1, memory_order_relaxed) + 1;
    if (counter == NV_STAT_DECODE_PICTURES && drv->statsLogInterval > 0 &&
        value % drv->statsLogInterval == 0) {
        nvStatsLog(drv, "periodic");
    }
}

void nvStatsInit(NVDriver *drv) {
    const char *statsEnv = getenv("NVD_STATS");
    if (statsEnv != NULL && strcmp(statsEnv, "0") != 0) {
        drv->statsEnabled = true;
        drv->statsLogInterval = 120;
        if (strcmp(statsEnv, "final") == 0) drv->statsLogInterval = 0;
        if (strcmp(statsEnv, "1") != 0) {
            char *end = NULL;
            unsigned long long interval = strtoull(statsEnv, &end, 10);
            if (end != statsEnv && interval > 0) {
                drv->statsLogInterval = interval;
            }
        }
        LOG("Stats enabled: interval=%llu decoded pictures", (unsigned long long) drv->statsLogInterval)
    }
}
