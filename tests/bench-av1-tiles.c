#define _POSIX_C_SOURCE 200809L
#include "../src/vabackend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern const NVCodec av1Codec;
static uint64_t ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(1);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(int argc, char **argv) {
    unsigned tiles = argc > 1 ? strtoul(argv[1],NULL,10) : 256;
    unsigned frames = argc > 2 ? strtoul(argv[2],NULL,10) : 1000;
    if (!tiles || tiles > NVD_MAX_AV1_TILES || (tiles > 64 && tiles % 64) || !frames || frames > 100000) return 1;
    VASliceParameterBufferAV1 *params = calloc(tiles,sizeof(*params));
    if (!params) return 1;
    unsigned cols = tiles < 64 ? tiles : 64;
    for (unsigned i = 0; i < tiles; i++) params[i] = (VASliceParameterBufferAV1){
        .tile_row = i / cols, .tile_column = i % cols,
        .slice_data_offset = i*4, .slice_data_size = 4};
    NVDriver drv = {0};
    NVContext ctx = {.drv=&drv};
    ctx.bitstreamBuffer.size = tiles * 4;
    NVBuffer buffer = {.ptr=params, .elements=tiles};
    CUVIDPICPARAMS pic = {.nNumSlices=tiles};
    pic.CodecSpecific.av1.num_tile_cols = cols;
    pic.CodecSpecific.av1.num_tile_rows = tiles/cols;
    uint64_t start = ns();
    for (unsigned f = 0; f < frames; f++) {
        ctx.av1TileOffsetsSeen = 0;
        ctx.av1TileMinOffset = UINT32_MAX;
        ctx.av1TileMaxEnd = 0;
        memset(ctx.av1TileSeen,0,sizeof(ctx.av1TileSeen));
#ifdef NVD_BENCH_INDEX
        nvdIntervalIndexReset(&ctx.av1TileIntervals);
#endif
        av1Codec.handlers[VASliceParameterBufferType](&ctx,&buffer,&pic);
        if (ctx.inputValidationFailed || ctx.sliceOffsets.failed || ctx.av1TileOffsetsSeen != tiles) return 1;
    }
    uint64_t elapsed = ns() - start;
    printf("{\"tiles\":%u,\"frames\":%u,\"correct_frames\":%u,\"validation_ns_per_frame\":%.3f,\"elapsed_ns\":%llu}\n",tiles,frames,frames,(double)elapsed/frames,(unsigned long long)elapsed);
    freeAppendableBuffer(&ctx.sliceOffsets);
#ifdef NVD_BENCH_INDEX
    nvdIntervalIndexDestroy(&ctx.av1TileIntervals);
#endif
    free(params);
    return 0;
}
