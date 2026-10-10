#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
// Include the codec to exercise its header-state tracking.
#include "../src/vp9.c"

void logger(const char *filename, const char *function, int line, const char *msg, ...) {
}

bool nvdLogDebugEnabled(void) {
    return false;
}

int pictureIdxFromSurfaceId(NVContext *ctx, VASurfaceID surfaceId) {
    return -1;
}

void nvSurfaceSetColorMetadata(NVSurface *surface, VAProcColorStandardType colorStandard, bool colorRangeFull) {
}

const char *nvColorStandardName(VAProcColorStandardType colorStandard) {
    return "";
}

// Uncompressed headers of a libvpx 64x64 stream: a key frame that codes the
// default loop-filter ref deltas, then two inter frames that do not update them.
static const uint8_t frame0[] = {
    0x82, 0x49, 0x83, 0x42, 0x00, 0x03, 0xf0, 0x03, 0xf6, 0x06, 0x38, 0x24,
    0x1c, 0x18, 0x42, 0x00, 0x03, 0x10, 0x50, 0x60, 0x1f, 0xcb, 0xff, 0x9e,
    0xe9, 0xde, 0x9b, 0x97, 0xf6, 0xcf, 0xcf, 0xf3, 0x31, 0x77, 0xe7, 0xfc,
    0xbf, 0xf6, 0xd8, 0xc1,
};
static const uint8_t frame1[] = {
    0x86, 0x00, 0x40, 0x92, 0xf0, 0xa1, 0x25, 0x0c, 0xc0, 0x40, 0x70, 0x18,
    0x0c, 0x06, 0x03, 0x39, 0x80, 0xc1, 0x1b, 0x88, 0xdc, 0x00, 0x00, 0x00,
    0x00, 0x6c, 0x76, 0x07, 0x8f, 0xbd, 0xd4, 0x77, 0x8b, 0x06, 0x7b, 0xdf,
    0x69, 0xea, 0x3d, 0xef,
};
static const uint8_t frame2[] = {
    0x86, 0x00, 0x40, 0x92, 0xf2, 0x11, 0x55, 0x8c, 0xc0, 0x30, 0x18, 0x0c,
    0x02, 0x08, 0xbc, 0x45, 0xe0, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x7e, 0xa3,
    0x00, 0x00, 0x00, 0x7f, 0x28, 0x6e, 0x5d, 0x1c, 0xd6, 0x09, 0xc2, 0x06,
    0x06, 0x29, 0x3e, 0x5d,
};

static void assertRefDeltas(const CUVIDPICPARAMS *pic, int a, int b, int c, int d) {
    const int expected[4] = {a, b, c, d};
    for (int i = 0; i < 4; i++) {
        assert((int) pic->CodecSpecific.vp9.mbRefLfDelta[i] == expected[i]);
    }
}

static void testInterFramesKeepCodedDeltas(void) {
    NVContext ctx = {0};
    const uint8_t *frames[] = {frame0, frame1, frame2};
    for (int i = 0; i < 3; i++) {
        CUVIDPICPARAMS pic = {0};
        parseExtraInfo(&ctx, (void *) frames[i], 40, &pic);
        assert(!ctx.inputValidationFailed);
        // The inter frames leave mode_ref_delta_update clear; NVDEC must
        // still receive the deltas the key frame established.
        assertRefDeltas(&pic, 1, 0, -1, -1);
        assert(pic.CodecSpecific.vp9.modeRefLfEnabled == 1);
    }
    destroyVP9Context(&ctx);
    free(ctx.codecData);
}

static GstVp9FrameHdr interHeader(void) {
    GstVp9FrameHdr hdr = {.frame_type = GST_VP9_INTER_FRAME};
    hdr.loopfilter.mode_ref_delta_enabled = 1;
    return hdr;
}

static void testUpdatesAndResets(void) {
    VP9Context vp9 = {0};
    GstVp9FrameHdr key = {.frame_type = GST_VP9_KEY_FRAME};
    key.loopfilter.mode_ref_delta_enabled = 1;
    key.loopfilter.mode_ref_delta_update = 1;
    key.loopfilter.update_ref_deltas[1] = 1;
    key.loopfilter.ref_deltas[1] = 2;
    key.loopfilter.update_mode_deltas[0] = 1;
    key.loopfilter.mode_deltas[0] = 3;
    key.segmentation.enabled = 1;
    key.segmentation.update_data = 1;
    key.segmentation.abs_delta = 1;
    key.segmentation.data[2].alternate_quantizer_enabled = 1;
    key.segmentation.data[2].alternate_quantizer = -20;
    key.segmentation.data[5].reference_frame_enabled = 1;
    key.segmentation.data[5].reference_frame = 2;
    key.segmentation.data[6].reference_skip = 1;
    updateVP9PastState(&vp9, &key);
    // Uncoded deltas take the defaults, coded ones replace them.
    assert(vp9.refLfDeltas[0] == 1 && vp9.refLfDeltas[1] == 2 &&
           vp9.refLfDeltas[2] == -1 && vp9.refLfDeltas[3] == -1);
    assert(vp9.modeLfDeltas[0] == 3 && vp9.modeLfDeltas[1] == 0);
    assert(vp9.segmentAbsDelta);
    assert(vp9.segmentFeatureEnabled[2][0] && vp9.segmentFeatureData[2][0] == -20);
    assert(vp9.segmentFeatureEnabled[5][2] && vp9.segmentFeatureData[5][2] == 2);
    assert(vp9.segmentFeatureEnabled[6][3] && vp9.segmentFeatureData[6][3] == 0);

    // Inter frames without updates, including ones with segmentation
    // enabled but no new data, keep everything.
    GstVp9FrameHdr inter = interHeader();
    inter.segmentation.enabled = 1;
    updateVP9PastState(&vp9, &inter);
    assert(vp9.refLfDeltas[1] == 2 && vp9.modeLfDeltas[0] == 3);
    assert(vp9.segmentAbsDelta && vp9.segmentFeatureData[2][0] == -20);

    // A partial update changes only the flagged entries.
    inter = interHeader();
    inter.loopfilter.mode_ref_delta_update = 1;
    inter.loopfilter.update_ref_deltas[3] = 1;
    inter.loopfilter.ref_deltas[3] = 4;
    updateVP9PastState(&vp9, &inter);
    assert(vp9.refLfDeltas[1] == 2 && vp9.refLfDeltas[3] == 4 && vp9.modeLfDeltas[0] == 3);

    // Delta updates are not read while the deltas are disabled.
    inter = interHeader();
    inter.loopfilter.mode_ref_delta_enabled = 0;
    inter.loopfilter.mode_ref_delta_update = 1;
    inter.loopfilter.update_ref_deltas[0] = 1;
    inter.loopfilter.ref_deltas[0] = 7;
    updateVP9PastState(&vp9, &inter);
    assert(vp9.refLfDeltas[0] == 1);

    // New segment data replaces every feature, clearing disabled ones.
    inter = interHeader();
    inter.segmentation.enabled = 1;
    inter.segmentation.update_data = 1;
    inter.segmentation.data[1].alternate_loop_filter_enabled = 1;
    inter.segmentation.data[1].alternate_loop_filter = -5;
    updateVP9PastState(&vp9, &inter);
    assert(!vp9.segmentAbsDelta);
    assert(!vp9.segmentFeatureEnabled[2][0] && vp9.segmentFeatureData[2][0] == 0);
    assert(!vp9.segmentFeatureEnabled[5][2] && !vp9.segmentFeatureEnabled[6][3]);
    assert(vp9.segmentFeatureEnabled[1][1] && vp9.segmentFeatureData[1][1] == -5);

    // Intra-only and error-resilient frames restore the defaults.
    GstVp9FrameHdr resets[2] = {interHeader(), interHeader()};
    resets[0].intra_only = 1;
    resets[1].error_resilient_mode = 1;
    for (int i = 0; i < 2; i++) {
        updateVP9PastState(&vp9, &inter);
        assert(vp9.segmentFeatureEnabled[1][1]);
        updateVP9PastState(&vp9, &resets[i]);
        assert(vp9.refLfDeltas[0] == 1 && vp9.refLfDeltas[1] == 0 &&
               vp9.refLfDeltas[2] == -1 && vp9.refLfDeltas[3] == -1);
        assert(vp9.modeLfDeltas[0] == 0 && vp9.modeLfDeltas[1] == 0);
        assert(!vp9.segmentFeatureEnabled[1][1] && vp9.segmentFeatureData[1][1] == 0);
    }
}

int main(void) {
    testInterFramesKeepCodedDeltas();
    testUpdatesAndResets();
    puts("VP9 loop-filter deltas and segment features persist until updated or reset");
    return 0;
}
