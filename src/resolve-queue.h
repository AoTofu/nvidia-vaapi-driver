#ifndef RESOLVE_QUEUE_H
#define RESOLVE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

#define RESOLVE_QUEUE_CAPACITY 16

typedef struct {
    atomic_uint_fast64_t *depth;
    atomic_uint_fast64_t *highWater;
    atomic_uint_fast64_t *fullWaits;
    atomic_uint_fast64_t *waitNs;
} ResolveQueueTelemetry;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t notEmpty;
    pthread_cond_t notFull;
    void *items[RESOLVE_QUEUE_CAPACITY];
    size_t readIdx;
    size_t writeIdx;
    size_t count;
    bool exiting;
    bool initialized;
    ResolveQueueTelemetry telemetry;
} ResolveQueue;

bool resolveQueueInit(ResolveQueue *queue);
void resolveQueueSetTelemetry(ResolveQueue *queue, ResolveQueueTelemetry telemetry);
bool resolveQueuePush(ResolveQueue *queue, void *item);
bool resolveQueuePop(ResolveQueue *queue, void **item);
void resolveQueueShutdown(ResolveQueue *queue);
size_t resolveQueueCancel(ResolveQueue *queue, void **cancelled, size_t capacity);
void resolveQueueDestroy(ResolveQueue *queue);

#endif
