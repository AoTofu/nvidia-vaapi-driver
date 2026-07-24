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
    const uint64_t hostBytes = statLoad(drv, NV_STAT_VIDEOPROC_CPU_SCRATCH_BYTES);
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
#define S(counter) ((unsigned long long) statLoad(drv, counter))
    fprintf(out,
        "%10ld.%09ld [%d-%d] Stats[%s]: decoder_creates=%llu decode_surfaces_selected=%llu decode_surfaces_auto_candidate=%llu decode_surfaces_legacy=%llu decode_pictures=%llu resolve_frames=%llu export_copies=%llu export_host_copies=%llu export_descriptors=%llu single_descriptors=%llu multi_descriptors=%llu videoproc_requests=%llu videoproc_cuda=%llu videoproc_cuda_failures=%llu videoproc_cpu_fallback=%llu device_copy_bytes=%llu host_copy_bytes=%llu host_fallback_frames=%llu resolve_queue_depth=%llu resolve_queue_high_water=%llu resolve_queue_full_waits=%llu resolve_queue_wait_ns=%llu backing_alloc_count=%llu backing_alloc_ns=%llu backing_prune_count=%llu backing_cache_hits=%llu active_backing_images=%llu detached_backing_images=%llu borrowed_backing_images=%llu external_backing_images=%llu active_backing_bytes=%llu detached_backing_bytes=%llu active_backing_bytes_peak=%llu detached_backing_bytes_peak=%llu total_backing_bytes_peak=%llu videoproc_gpu_scratch_bytes=%llu videoproc_gpu_scratch_bytes_peak=%llu videoproc_cpu_scratch_bytes=%llu videoproc_cpu_scratch_bytes_peak=%llu tracked_vram_equivalent_bytes=%llu tracked_vram_equivalent_bytes_peak=%llu tracked_ram_equivalent_bytes=%llu tracked_ram_equivalent_bytes_peak=%llu videoproc_ns=%llu object_lookup_count=%llu object_lookup_steps=%llu buffer_pool_hits=%llu buffer_pool_misses=%llu av1_compact_count=%llu av1_compact_bytes=%llu jpeg_copy_bytes=%llu detached_backing_limit_bytes=%llu detached_backing_limit_images=%u memory_budget_bytes=%llu\n",
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
        S(NV_STAT_OBJECT_LOOKUP_COUNT), S(NV_STAT_OBJECT_LOOKUP_STEPS),
        S(NV_STAT_BUFFER_POOL_HITS), S(NV_STAT_BUFFER_POOL_MISSES),
        S(NV_STAT_AV1_COMPACT_COUNT), S(NV_STAT_AV1_COMPACT_BYTES), S(NV_STAT_JPEG_COPY_BYTES),
        (unsigned long long) drv->maxDetachedBackingImageBytes, drv->maxDetachedBackingImages,
        (unsigned long long) drv->memoryBudgetBytes);
#undef S
    fflush(out);
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
