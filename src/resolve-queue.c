#include "resolve-queue.h"

#include <string.h>

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

bool resolveQueuePush(ResolveQueue *queue, void *item) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == RESOLVE_QUEUE_CAPACITY && !queue->exiting) {
        pthread_cond_wait(&queue->notFull, &queue->mutex);
    }

    if (queue->exiting) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    queue->items[queue->writeIdx] = item;
    queue->writeIdx = (queue->writeIdx + 1) % RESOLVE_QUEUE_CAPACITY;
    queue->count++;
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

void resolveQueueDestroy(ResolveQueue *queue) {
    if (!queue->initialized) {
        return;
    }

    pthread_cond_destroy(&queue->notFull);
    pthread_cond_destroy(&queue->notEmpty);
    pthread_mutex_destroy(&queue->mutex);
    queue->initialized = false;
}
