#include "resolve-queue.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

static uint64_t monotonicNs(void) {
    struct timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC, &tp) != 0) {
        return 0;
    }
    return (uint64_t) tp.tv_sec * 1000000000ULL + (uint64_t) tp.tv_nsec;
}

static void updateHighWater(ResolveQueue *queue, uint64_t depth) {
    if (queue->telemetry.highWater == NULL) {
        return;
    }
    uint_fast64_t current = atomic_load_explicit(queue->telemetry.highWater, memory_order_relaxed);
    while (current < depth &&
           !atomic_compare_exchange_weak_explicit(queue->telemetry.highWater, &current, depth,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

bool resolveQueueInit(ResolveQueue *queue) {
    memset(queue, 0, sizeof(*queue));

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&queue->notEmpty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return false;
    }
    if (pthread_cond_init(&queue->notFull, NULL) != 0) {
        pthread_cond_destroy(&queue->notEmpty);
        pthread_mutex_destroy(&queue->mutex);
        return false;
    }

    queue->initialized = true;
    return true;
}

void resolveQueueSetTelemetry(ResolveQueue *queue, ResolveQueueTelemetry telemetry) {
    queue->telemetry = telemetry;
}

bool resolveQueuePush(ResolveQueue *queue, void *item) {
    pthread_mutex_lock(&queue->mutex);
    uint64_t waitStart = 0;
    bool waited = false;
    while (queue->count == RESOLVE_QUEUE_CAPACITY && !queue->exiting) {
        if (!waited && queue->telemetry.fullWaits != NULL) {
            atomic_fetch_add_explicit(queue->telemetry.fullWaits, 1, memory_order_relaxed);
            waitStart = monotonicNs();
            waited = true;
        }
        pthread_cond_wait(&queue->notFull, &queue->mutex);
    }

    if (waited && queue->telemetry.waitNs != NULL) {
        const uint64_t waitEnd = monotonicNs();
        if (waitStart != 0 && waitEnd >= waitStart) {
            atomic_fetch_add_explicit(queue->telemetry.waitNs, waitEnd - waitStart, memory_order_relaxed);
        }
    }

    if (queue->exiting) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    queue->items[queue->writeIdx] = item;
    queue->writeIdx = (queue->writeIdx + 1) % RESOLVE_QUEUE_CAPACITY;
    queue->count++;
    if (queue->telemetry.depth != NULL) {
        const uint_fast64_t depth = atomic_fetch_add_explicit(queue->telemetry.depth, 1, memory_order_relaxed) + 1;
        updateHighWater(queue, depth);
    }
    pthread_cond_signal(&queue->notEmpty);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

bool resolveQueuePop(ResolveQueue *queue, void **item) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->exiting) {
        pthread_cond_wait(&queue->notEmpty, &queue->mutex);
    }

    // Drain work already accepted before shutdown. Once the queue is empty,
    // exiting prevents the consumer from waiting again.
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    *item = queue->items[queue->readIdx];
    queue->items[queue->readIdx] = NULL;
    queue->readIdx = (queue->readIdx + 1) % RESOLVE_QUEUE_CAPACITY;
    queue->count--;
    if (queue->telemetry.depth != NULL) {
        atomic_fetch_sub_explicit(queue->telemetry.depth, 1, memory_order_relaxed);
    }
    pthread_cond_signal(&queue->notFull);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

void resolveQueueShutdown(ResolveQueue *queue) {
    if (!queue->initialized) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->exiting = true;
    pthread_cond_broadcast(&queue->notEmpty);
    pthread_cond_broadcast(&queue->notFull);
    pthread_mutex_unlock(&queue->mutex);
}

size_t resolveQueueCancel(ResolveQueue *queue, void **cancelled, size_t capacity) {
    if (!queue->initialized) {
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->exiting = true;
    const size_t pending = queue->count;
    const size_t returned = pending < capacity ? pending : capacity;
    for (size_t i = 0; i < pending; i++) {
        void *item = queue->items[queue->readIdx];
        queue->items[queue->readIdx] = NULL;
        queue->readIdx = (queue->readIdx + 1) % RESOLVE_QUEUE_CAPACITY;
        if (i < returned && cancelled != NULL) {
            cancelled[i] = item;
        }
    }
    queue->count = 0;
    queue->writeIdx = queue->readIdx;
    if (queue->telemetry.depth != NULL && pending != 0) {
        atomic_fetch_sub_explicit(queue->telemetry.depth, pending, memory_order_relaxed);
    }
    pthread_cond_broadcast(&queue->notEmpty);
    pthread_cond_broadcast(&queue->notFull);
    pthread_mutex_unlock(&queue->mutex);
    return returned;
}

void resolveQueueDestroy(ResolveQueue *queue) {
    if (!queue->initialized) {
        return;
    }

    pthread_cond_destroy(&queue->notFull);
    pthread_cond_destroy(&queue->notEmpty);
    pthread_mutex_destroy(&queue->mutex);
    queue->initialized = false;
}
