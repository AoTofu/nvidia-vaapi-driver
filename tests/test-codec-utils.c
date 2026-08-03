#include "vabackend.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void testVP8FrameHeaders(void) {
    uint8_t header[10] = { 0 };
    assert(nvBuildVP8FrameHeader(header, false, 3, true, 0x12345,
                                 0, 0) == 3);
    const uint32_t interTag = 1U | (3U << 1U) | (1U << 4U) |
                              (0x12345U << 5U);
    assert(header[0] == (uint8_t) interTag);
    assert(header[1] == (uint8_t) (interTag >> 8U));
    assert(header[2] == (uint8_t) (interTag >> 16U));

    memset(header, 0, sizeof(header));
    assert(nvBuildVP8FrameHeader(header, true, 0, true, 1234,
                                 1920, 1080) == 10);
    const uint8_t syncCode[] = { 0x9d, 0x01, 0x2a };
    assert(memcmp(&header[3], syncCode, sizeof(syncCode)) == 0);
    assert(header[6] == 0x80 && header[7] == 0x07);
    assert(header[8] == 0x38 && header[9] == 0x04);

    assert(nvBuildVP8FrameHeader(header, false, 8, true, 0, 0, 0) == 0);
    assert(nvBuildVP8FrameHeader(header, false, 0, true, 0x80000,
                                 0, 0) == 0);
    assert(nvBuildVP8FrameHeader(header, true, 0, true, 0,
                                 0x4000, 1) == 0);
}

static void testBufferSchemas(void) {
    uint8_t storage[32] = { 0 };
    const BufferSchema schema = {
        .minElementSize = 8,
        .minElements = 1,
        .maxElements = 2,
    };
    NVBuffer buffer = {
        .ptr = storage,
        .elementSize = 8,
        .elements = 2,
        .size = 16,
    };
    assert(nvValidateBufferSchema(&buffer, &schema));
    buffer.elementSize = 7;
    assert(!nvValidateBufferSchema(&buffer, &schema));
    buffer.elementSize = 8;
    buffer.elements = 3;
    buffer.size = 24;
    assert(!nvValidateBufferSchema(&buffer, &schema));
    buffer.elements = 2;
    buffer.size = 15;
    assert(!nvValidateBufferSchema(&buffer, &schema));
}

static void testPictureIndexSelection(void) {
    assert(nvdSelectPictureIndex(0, 20, 0, 0, 0) == 0);
    assert(nvdSelectPictureIndex(19, 20, UINT32_MAX, UINT32_MAX, 0) == 19);

    // Once the decoder's index space is full, never recycle an active DPB
    // entry or a surface whose resolve copy is still in flight.
    assert(nvdSelectPictureIndex(20, 20, 0x0000ffffU, 1U << 16, 0) == 17);
    assert(nvdSelectPictureIndex(20, 20, 0x0000ffffU, 1U << 16, 19) == 19);
    assert(nvdSelectPictureIndex(20, 20, 0x000fffffU, 0, 0) == -1);

    assert(nvdSelectPictureIndex(32, 32, UINT32_MAX, 0, 0) == -1);
    assert(nvdSelectPictureIndex(32, 32, UINT32_MAX ^ (1U << 31), 0, 0) == 31);
    assert(nvdSelectPictureIndex(0, 0, 0, 0, 0) == -1);
    assert(nvdSelectPictureIndex(0, 33, 0, 0, 0) == -1);
}

int main(void) {
    assert(nvPlaneExtent(350, 1) == 175);
    assert(nvPlaneExtent(351, 1) == 176);
    assert(nvPlaneExtent(1, 1) == 1);
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
    testVP8FrameHeaders();
    testBufferSchemas();
    testPictureIndexSelection();
    return 0;
}
