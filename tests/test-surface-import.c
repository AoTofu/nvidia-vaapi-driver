#include "surface-import.h"

#include <assert.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static VASurfaceAttrib descriptorAttribute(void *descriptor) {
    return (VASurfaceAttrib) {
        .type = VASurfaceAttribExternalBufferDescriptor,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypePointer, .value.p = descriptor },
    };
}

static VASurfaceAttrib memoryTypeAttribute(uint32_t memoryType) {
    return (VASurfaceAttrib) {
        .type = VASurfaceAttribMemoryType,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypeInteger, .value.i = (int32_t) memoryType },
    };
}

static void testPrime2SingleObject(void) {
    VADRMPRIMESurfaceDescriptor descriptor = { 0 };
    descriptor.fourcc = VA_FOURCC_NV12;
    descriptor.width = 1920;
    descriptor.height = 1080;
    descriptor.num_objects = 1;
    descriptor.objects[0].fd = 10;
    descriptor.objects[0].size = 3133440;
    descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    descriptor.num_layers = 1;
    descriptor.layers[0].drm_format = DRM_FORMAT_NV12;
    descriptor.layers[0].num_planes = 2;
    descriptor.layers[0].object_index[0] = 0;
    descriptor.layers[0].object_index[1] = 0;
    descriptor.layers[0].offset[0] = 0;
    descriptor.layers[0].offset[1] = 2088960;
    descriptor.layers[0].pitch[0] = 1920;
    descriptor.layers[0].pitch[1] = 1920;

    VASurfaceAttrib attributes[] = {
        memoryTypeAttribute(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2),
        descriptorAttribute(&descriptor),
    };
    ImportedSurface imported;
    parseSurfaceImportAttributes(attributes, 2, &imported);
    assert(imported.requested && imported.valid);
    assert(imported.numObjects == 1 && imported.numPlanes == 2);
    assert(imported.objects[0].fd == 10);
    assert(imported.objects[0].size == 3133440);
    assert(imported.objects[0].modifier == DRM_FORMAT_MOD_LINEAR);
    assert(imported.planes[1].objectIndex == 0);
    assert(imported.planes[1].offset == 2088960);
    assert(imported.planes[1].pitch == 1920);
}

static void testPrime2MultipleObjects(void) {
    const uint64_t tiledModifier = 0x0300000000000012ULL;
    VADRMPRIMESurfaceDescriptor descriptor = { 0 };
    descriptor.fourcc = VA_FOURCC_P010;
    descriptor.width = 3840;
    descriptor.height = 2160;
    descriptor.num_objects = 2;
    descriptor.objects[0].fd = 21;
    descriptor.objects[0].size = 16711680;
    descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    descriptor.objects[1].fd = 22;
    descriptor.objects[1].size = 8355840;
    descriptor.objects[1].drm_format_modifier = tiledModifier;
    descriptor.num_layers = 2;
    descriptor.layers[0].num_planes = 1;
    descriptor.layers[0].object_index[0] = 0;
    descriptor.layers[0].pitch[0] = 7680;
    descriptor.layers[1].num_planes = 1;
    descriptor.layers[1].object_index[0] = 1;
    descriptor.layers[1].pitch[0] = 7680;

    VASurfaceAttrib attributes[] = {
        memoryTypeAttribute(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2),
        descriptorAttribute(&descriptor),
    };
    ImportedSurface imported;
    parseSurfaceImportAttributes(attributes, 2, &imported);
    assert(imported.valid);
    assert(imported.dataSize == 25067520);
    assert(imported.objects[1].fd == 22);
    assert(imported.objects[1].modifier == tiledModifier);
    assert(imported.planes[0].objectIndex == 0);
    assert(imported.planes[1].objectIndex == 1);
}

static void testInvalidPrime2ObjectIndex(void) {
    VADRMPRIMESurfaceDescriptor descriptor = { 0 };
    descriptor.fourcc = VA_FOURCC_NV12;
    descriptor.width = 64;
    descriptor.height = 64;
    descriptor.num_objects = 1;
    descriptor.objects[0].fd = 30;
    descriptor.objects[0].size = 6144;
    descriptor.num_layers = 1;
    descriptor.layers[0].num_planes = 2;
    descriptor.layers[0].object_index[0] = 0;
    descriptor.layers[0].object_index[1] = 1;

    VASurfaceAttrib attributes[] = {
        memoryTypeAttribute(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2),
        descriptorAttribute(&descriptor),
    };
    ImportedSurface imported;
    parseSurfaceImportAttributes(attributes, 2, &imported);
    assert(imported.requested && !imported.valid);
}

static void testLegacyPrimeSurfaceBuffers(void) {
    unsigned long buffers[] = { 40, 41 };
    VASurfaceAttribExternalBuffers descriptor = {
        .pixel_format = VA_FOURCC_NV12,
        .width = 128,
        .height = 64,
        .data_size = 12288,
        .num_planes = 2,
        .pitches = { 128, 128 },
        .offsets = { 0, 0 },
        .buffers = buffers,
        .num_buffers = 2,
    };
    VASurfaceAttrib attributes[] = {
        memoryTypeAttribute(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME),
        descriptorAttribute(&descriptor),
    };
    ImportedSurface imported;
    parseSurfaceImportAttributes(attributes, 2, &imported);
    assert(imported.valid);
    assert(imported.legacyPrime);
    assert(imported.numObjects == 1);
    assert(imported.numLegacyBuffers == 2);
    assert(imported.objects[0].modifier == DRM_FORMAT_MOD_LINEAR);
    assert(imported.planes[0].objectIndex == 0);
    assert(imported.planes[1].objectIndex == 0);

    ImportedSurface selected;
    assert(importedSurfaceSelectIndex(&imported, 1, 2, &selected));
    assert(selected.objects[0].fd == 41);
    assert(!importedSurfaceSelectIndex(&imported, 0, 1, &selected));
}

static void testLegacyPrimeRejectsUnknownTiling(void) {
    uintptr_t buffer = 50;
    VASurfaceAttribExternalBuffers descriptor = {
        .pixel_format = VA_FOURCC_NV12,
        .width = 64,
        .height = 64,
        .data_size = 6144,
        .num_planes = 2,
        .pitches = { 64, 64 },
        .offsets = { 0, 4096 },
        .buffers = &buffer,
        .num_buffers = 1,
        .flags = VA_SURFACE_EXTBUF_DESC_ENABLE_TILING,
    };
    VASurfaceAttrib attributes[] = {
        memoryTypeAttribute(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME),
        descriptorAttribute(&descriptor),
    };
    ImportedSurface imported;
    parseSurfaceImportAttributes(attributes, 2, &imported);
    assert(imported.requested && !imported.valid);
}

int main(void) {
    testPrime2SingleObject();
    testPrime2MultipleObjects();
    testInvalidPrime2ObjectIndex();
    testLegacyPrimeSurfaceBuffers();
    testLegacyPrimeRejectsUnknownTiling();
    puts("surface import tests passed");
    return 0;
}
