#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"

static bool ensure_capacity(Array *arr, uint32_t new_capacity) {
    if (new_capacity <= arr->capacity) {
        //we already have enough capacity to hold the new element
        return true;
    }

    uint32_t capacity = arr->capacity;
    if (capacity == 0) {
        //if we're completely empty allocate a small amount
        capacity = 16;
    } else {
        //grow the capacity until we can hold the new amount
        while (new_capacity > capacity) {
            uint32_t growth = capacity >> 1;
            if (growth == 0 || capacity > UINT32_MAX - growth) {
                capacity = new_capacity;
                break;
            }
            capacity += growth;
        }
    }

    if (capacity != 0 && sizeof(void *) > SIZE_MAX / capacity) {
        return false;
    }
    void **new_buf = realloc(arr->buf, (size_t) capacity * sizeof(void *));
    if (new_buf == NULL) {
        return false;
    }

    //clear the new part of the array
    memset(&new_buf[arr->capacity], 0, (size_t)(capacity - arr->capacity) * sizeof(void *));
    arr->buf = new_buf;
    arr->capacity = capacity;
    return true;
}

bool add_element(Array *arr, void *element) {
    if (arr->size == UINT32_MAX || !ensure_capacity(arr, arr->size + 1)) {
        return false;
    }

    arr->buf[arr->size++] = element;
    return true;
}

void* alloc_and_add_element(Array *arr, size_t size) {
    void *element = calloc(1, size);
    if (element == NULL || !add_element(arr, element)) {
        free(element);
        return NULL;
    }
    return element;
}

void remove_element_at(Array *arr, uint32_t index) {
    if (index >= arr->size) {
        return;
    }

    arr->size--;
    if (index < arr->size) {
        for (uint32_t i = index; i < arr->size; i++) {
            arr->buf[i] = arr->buf[i+1];
        }
    }
    //clear out the remaining element
    arr->buf[arr->size] = NULL;
}

void remove_and_free_element_at(Array *arr, uint32_t index) {
    void *element = get_element_at(arr, index);
    remove_element_at(arr, index);
    free(element);
}

uint32_t get_size(Array *arr) {
    return arr->size;
}

void *get_element_at(Array *arr, uint32_t index) {
    if (arr->buf == NULL || index >= arr->size) {
        return NULL;
    }
    return arr->buf[index];
}
