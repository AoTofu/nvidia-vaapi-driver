#include "resolve-queue.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define ITEM_COUNT 200000

typedef struct {
    ResolveQueue *queue;
    atomic_size_t consumed;
    uint64_t sum;
} ConsumerState;

static void *consume(void *opaque) {
    ConsumerState *state = opaque;
    void *item = NULL;
    while (resolveQueuePop(state->queue, &item)) {
        state->sum += (uintptr_t) item;
        atomic_fetch_add_explicit(&state->consumed, 1, memory_order_relaxed);
    }
    return NULL;
}

static void testWrapAndBackpressure(void) {
    ResolveQueue queue;
    assert(resolveQueueInit(&queue));

    ConsumerState state = {.queue = &queue};
    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, consume, &state) == 0);

    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    uint64_t expected = 0;
    for (uintptr_t i = 1; i <= ITEM_COUNT; i++) {
        assert(resolveQueuePush(&queue, (void *) i));
        expected += i;
    }

    resolveQueueShutdown(&queue);
    assert(pthread_join(consumer, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    assert(atomic_load(&state.consumed) == ITEM_COUNT);
    assert(state.sum == expected);
    assert(queue.count == 0);
    double seconds = (double) (finished.tv_sec - started.tv_sec) +
                     (double) (finished.tv_nsec - started.tv_nsec) / 1000000000.0;
    printf("queue throughput: %.0f items/s (%d items in %.6f s)\n",
           ITEM_COUNT / seconds, ITEM_COUNT, seconds);
    resolveQueueDestroy(&queue);
}

typedef struct {
    ResolveQueue *queue;
    uintptr_t first;
    uintptr_t count;
} ProducerState;

static void *produce(void *opaque) {
    ProducerState *state = opaque;
    for (uintptr_t i = 0; i < state->count; i++) {
        assert(resolveQueuePush(state->queue, (void *) (state->first + i)));
    }
    return NULL;
}

static void testMultipleProducersConsumers(void) {
    enum { THREAD_COUNT = 4, ITEMS_PER_PRODUCER = 50000 };
    ResolveQueue queue;
    assert(resolveQueueInit(&queue));

    pthread_t consumers[THREAD_COUNT];
    ConsumerState consumerStates[THREAD_COUNT] = {0};
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        consumerStates[i].queue = &queue;
        assert(pthread_create(&consumers[i], NULL, consume, &consumerStates[i]) == 0);
    }

    pthread_t producers[THREAD_COUNT];
    ProducerState producerStates[THREAD_COUNT];
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        producerStates[i] = (ProducerState) {
            .queue = &queue,
            .first = i * ITEMS_PER_PRODUCER + 1,
            .count = ITEMS_PER_PRODUCER,
        };
        assert(pthread_create(&producers[i], NULL, produce, &producerStates[i]) == 0);
    }
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        assert(pthread_join(producers[i], NULL) == 0);
    }
    resolveQueueShutdown(&queue);

    size_t consumed = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < THREAD_COUNT; i++) {
        assert(pthread_join(consumers[i], NULL) == 0);
        consumed += atomic_load(&consumerStates[i].consumed);
        sum += consumerStates[i].sum;
    }

    const uint64_t total = THREAD_COUNT * ITEMS_PER_PRODUCER;
    assert(consumed == total);
    assert(sum == total * (total + 1) / 2);
    resolveQueueDestroy(&queue);
}

typedef struct {
    ResolveQueue *queue;
    atomic_bool returned;
    bool result;
} BlockedProducerState;

static void *blockedProducer(void *opaque) {
    BlockedProducerState *state = opaque;
    state->result = resolveQueuePush(state->queue, (void *) (uintptr_t) 999);
    atomic_store(&state->returned, true);
    return NULL;
}

static void testShutdownWakesFullQueue(void) {
    ResolveQueue queue;
    assert(resolveQueueInit(&queue));
    atomic_uint_fast64_t depth = 0;
    atomic_uint_fast64_t highWater = 0;
    atomic_uint_fast64_t fullWaits = 0;
    atomic_uint_fast64_t waitNs = 0;
    resolveQueueSetTelemetry(&queue, (ResolveQueueTelemetry) {
        .depth = &depth,
        .highWater = &highWater,
        .fullWaits = &fullWaits,
        .waitNs = &waitNs,
    });
    for (uintptr_t i = 1; i <= RESOLVE_QUEUE_CAPACITY; i++) {
        assert(resolveQueuePush(&queue, (void *) i));
    }
    assert(atomic_load(&depth) == RESOLVE_QUEUE_CAPACITY);
    assert(atomic_load(&highWater) == RESOLVE_QUEUE_CAPACITY);

    BlockedProducerState state = {.queue = &queue};
    pthread_t producer;
    assert(pthread_create(&producer, NULL, blockedProducer, &state) == 0);

    struct timespec delay = {.tv_nsec = 1000000};
    nanosleep(&delay, NULL);
    assert(!atomic_load(&state.returned));

    resolveQueueShutdown(&queue);
    assert(pthread_join(producer, NULL) == 0);
    assert(atomic_load(&state.returned));
    assert(!state.result);

    void *item = NULL;
    size_t drained = 0;
    while (resolveQueuePop(&queue, &item)) {
        drained++;
    }
    assert(drained == RESOLVE_QUEUE_CAPACITY);
    assert(atomic_load(&depth) == 0);
    assert(atomic_load(&fullWaits) == 1);
    assert(atomic_load(&waitNs) > 0);
    resolveQueueDestroy(&queue);
}

int main(void) {
    testWrapAndBackpressure();
    testMultipleProducersConsumers();
    testShutdownWakesFullQueue();
    puts("resolve queue tests passed");
    return 0;
}
