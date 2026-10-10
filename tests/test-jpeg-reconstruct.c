#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
// Include the codec to exercise its internal frame reconstruction.
#include "../src/jpeg.c"

static const char *lastLog;
void logger(const char *filename, const char *function, int line, const char *msg, ...) {
    lastLog = msg;
}

static JPEGContext validContext(void) {
    JPEGContext ctx = {
        .hasPicParams = true,
        .hasIQMatrix = true,
        .validQuantTablesMask = 1,
    };
    ctx.picParams.picture_width = 16;
    ctx.picParams.picture_height = 16;
    ctx.picParams.num_components = 1;
    ctx.picParams.components[0].component_id = 1;
    ctx.picParams.components[0].h_sampling_factor = 1;
    ctx.picParams.components[0].v_sampling_factor = 1;
    ctx.iqMatrix.load_quantiser_table[0] = 1;
    for (unsigned i = 0; i < 64; i++) ctx.iqMatrix.quantiser_table[0][i] = (uint8_t) (i + 1);
    return ctx;
}

static VASliceParameterBufferJPEGBaseline slice(uint32_t offset, uint32_t size) {
    VASliceParameterBufferJPEGBaseline s = {
        .slice_data_size = size,
        .slice_data_offset = offset,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL,
        .num_components = 1,
    };
    s.components[0].component_selector = 1;
    return s;
}

// FNV-1a over the reconstructed frame, printed so the byte stream can be
// compared against another build of the reconstruction.
static uint64_t hashBytes(const uint8_t *data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; i++) hash = (hash ^ data[i]) * 1099511628211ULL;
    return hash;
}

static void testValidFrame(void) {
    const JPEGContext ctx = validContext();
    const uint8_t ecs[] = {0x12, 0x34, 0x56, 0x78, 0x9a};
    const VASliceParameterBufferJPEGBaseline slices[] = {slice(0, 3), slice(3, 2)};
    AppendableBuffer out = {0};
    uint32_t size = 0;
    assert(reconstructJPEG(&ctx, slices, 2, ecs, sizeof(ecs), &out, &size));
    assert(out.size == size && size > sizeof(jfifHeader) + sizeof(ecs));
    const uint8_t *frame = out.buf;
    assert(frame[0] == JPEG_MARKER && frame[1] == JPEG_SOI);
    assert(frame[size - 2] == JPEG_MARKER && frame[size - 1] == JPEG_EOI);
    assert(memcmp(frame + size - 2 - sizeof(ecs), ecs, sizeof(ecs)) == 0);
    printf("valid frame: %u bytes, hash %016llx\n", size, (unsigned long long) hashBytes(frame, size));
    freeAppendableBuffer(&out);
}

// OPT-05: a frame whose slice data alone exceeds the 32-bit CUVID length is
// rejected before the worst-case output is reserved or any slice is copied.
// sliceData is never dereferenced on that path, so no large input is needed.
static void testOversizedFramesRejectedEarly(void) {
    const JPEGContext ctx = validContext();
    const uint8_t *bogus = (const uint8_t *) (uintptr_t) 0x10;
    const uint32_t big = UINT32_C(0xf0000000);
    // Overlapping slices repeat one valid range; the sum exceeds UINT32_MAX.
    const VASliceParameterBufferJPEGBaseline repeat[] = {slice(0, big), slice(0, big)};
    AppendableBuffer out = {0};
    uint32_t size = 0;
    lastLog = NULL;
    assert(!reconstructJPEG(&ctx, repeat, 2, bogus, big, &out, &size));
    assert(out.buf == NULL && out.size == 0 && out.allocated == 0);
    assert(lastLog != NULL && strstr(lastLog, "CUVID limit") != NULL);

    // One slice that fits alone but not after already-buffered output.
    const VASliceParameterBufferJPEGBaseline one[] = {slice(0, big)};
    uint8_t existing[16] = {0};
    assert(appendBuffer(&out, existing, sizeof(existing)));
    out.size = (size_t) UINT32_MAX - big;
    const size_t allocated = out.allocated;
    lastLog = NULL;
    assert(!reconstructJPEG(&ctx, one, 1, bogus, big, &out, &size));
    assert(out.size == (size_t) UINT32_MAX - big && out.allocated == allocated);
    assert(lastLog != NULL && strstr(lastLog, "CUVID limit") != NULL);
    out.size = sizeof(existing);
    freeAppendableBuffer(&out);
}

int main(void) {
    testValidFrame();
    testOversizedFramesRejectedEarly();
    return 0;
}
