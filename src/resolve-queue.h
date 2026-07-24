#ifndef RESOLVE_QUEUE_H
#define RESOLVE_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define RESOLVE_QUEUE_CAPACITY 16

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
} ResolveQueue;

bool resolveQueueInit(ResolveQueue *queue);
bool resolveQueuePush(ResolveQueue *queue, void *item);
bool resolveQueuePop(ResolveQueue *queue, void **item);
void resolveQueueShutdown(ResolveQueue *queue);
void resolveQueueDestroy(ResolveQueue *queue);

#endif
