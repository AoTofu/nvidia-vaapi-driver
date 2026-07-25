#include "vabackend.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
    uint8_t storage[16] = { 0 };
    NVBuffer buffer = {
        .ptr = storage,
        .size = sizeof(storage),
    };
    NVContext context = { 0 };
    const void *data = NULL;

    assert(nvValidateSliceRange(&context, &buffer, 4, 12, &data));
    assert(data == &storage[4]);
    assert(!context.inputValidationFailed);

    assert(!nvValidateSliceRange(&context, &buffer, 17, 0, &data));
    assert(data == NULL);
    assert(context.inputValidationFailed);

    context.inputValidationFailed = false;
    assert(!nvValidateSliceRange(&context, &buffer, 15, 2, NULL));
    assert(context.inputValidationFailed);

    context.inputValidationFailed = false;
    assert(nvValidateSliceRange(&context, &buffer, 16, 0, NULL));
    assert(!context.inputValidationFailed);
    return 0;
}
