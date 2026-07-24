#include "surface-import.h"

#include <drm_fourcc.h>
#include <limits.h>
#include <string.h>

void importedSurfaceInit(ImportedSurface *imported) {
    memset(imported, 0, sizeof(*imported));
    for (uint32_t i = 0; i < NVD_MAX_IMPORTED_OBJECTS; i++) {
        imported->objects[i].fd = -1;
        imported->objects[i].modifier = DRM_FORMAT_MOD_INVALID;
    }
}

static bool addObjectSize(ImportedSurface *imported, uint64_t size) {
    if (UINT64_MAX - imported->dataSize < size) {
        return false;
    }
    imported->dataSize += size;
    return true;
}

static void parsePrimeDescriptor(const VASurfaceAttribExternalBuffers *ext,
                                 ImportedSurface *imported) {
    imported->pixelFormat = ext->pixel_format != 0 ? ext->pixel_format : imported->pixelFormat;
    imported->width = ext->width;
    imported->height = ext->height;
    imported->dataSize = ext->data_size;
    imported->numPlanes = ext->num_planes;
    imported->numObjects = 1;
    imported->legacyPrime = true;
    imported->legacyBuffers = ext->buffers;
    imported->numLegacyBuffers = ext->num_buffers;
    if (imported->numPlanes == 0 || imported->numPlanes > NVD_MAX_IMPORTED_PLANES ||
        imported->numLegacyBuffers == 0 || ext->buffers == NULL ||
        (ext->flags & VA_SURFACE_EXTBUF_DESC_ENABLE_TILING) != 0) {
        return;
    }

    for (uint32_t i = 0; i < imported->numLegacyBuffers; i++) {
        if (ext->buffers[i] > INT_MAX) {
            return;
        }
    }
    imported->objects[0].fd = (int) ext->buffers[0];
    imported->objects[0].size = ext->data_size;
    // Old PRIME supports one pitched object per surface. Tiled descriptors
    // were rejected above because they cannot carry a DRM modifier.
    imported->objects[0].modifier = DRM_FORMAT_MOD_LINEAR;
    for (uint32_t i = 0; i < imported->numPlanes; i++) {
        imported->planes[i].objectIndex = 0;
        imported->planes[i].offset = ext->offsets[i];
        imported->planes[i].pitch = ext->pitches[i];
    }
    imported->valid = true;
}

static void parsePrime2Descriptor(const VADRMPRIMESurfaceDescriptor *desc,
                                  ImportedSurface *imported) {
    imported->pixelFormat = desc->fourcc;
    imported->width = desc->width;
    imported->height = desc->height;
    imported->numObjects = desc->num_objects;
    if (desc->num_layers == 0 || desc->num_layers > 4 ||
        imported->numObjects == 0 || imported->numObjects > NVD_MAX_IMPORTED_OBJECTS) {
        return;
    }

    for (uint32_t i = 0; i < imported->numObjects; i++) {
        if (desc->objects[i].fd < 0 || !addObjectSize(imported, desc->objects[i].size)) {
            return;
        }
        imported->objects[i].fd = desc->objects[i].fd;
        imported->objects[i].size = desc->objects[i].size;
        imported->objects[i].modifier = desc->objects[i].drm_format_modifier;
    }

    uint32_t planeIndex = 0;
    for (uint32_t layer = 0; layer < desc->num_layers; layer++) {
        if (desc->layers[layer].num_planes == 0 || desc->layers[layer].num_planes > 4) {
            return;
        }
        for (uint32_t plane = 0; plane < desc->layers[layer].num_planes; plane++) {
            if (planeIndex >= NVD_MAX_IMPORTED_PLANES ||
                desc->layers[layer].object_index[plane] >= imported->numObjects) {
                return;
            }
            imported->planes[planeIndex].objectIndex = desc->layers[layer].object_index[plane];
            imported->planes[planeIndex].offset = desc->layers[layer].offset[plane];
            imported->planes[planeIndex].pitch = desc->layers[layer].pitch[plane];
            planeIndex++;
        }
    }
    imported->numPlanes = planeIndex;
    imported->valid = planeIndex > 0;
}

void parseSurfaceImportAttributes(const VASurfaceAttrib *attribList,
                                  unsigned int numAttribs,
                                  ImportedSurface *imported) {
    importedSurfaceInit(imported);
    const void *externalDescriptor = NULL;

    for (unsigned int i = 0; i < numAttribs; i++) {
        switch (attribList[i].type) {
        case VASurfaceAttribMemoryType:
            imported->memoryType = (uint32_t) attribList[i].value.value.i;
            break;
        case VASurfaceAttribExternalBufferDescriptor:
            externalDescriptor = attribList[i].value.value.p;
            break;
        case VASurfaceAttribPixelFormat:
            imported->pixelFormat = (uint32_t) attribList[i].value.value.i;
            break;
        default:
            break;
        }
    }

    if (externalDescriptor == NULL) {
        return;
    }
    imported->requested = true;
    if ((imported->memoryType & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) != 0) {
        parsePrime2Descriptor(externalDescriptor, imported);
    } else if ((imported->memoryType & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) != 0) {
        parsePrimeDescriptor(externalDescriptor, imported);
    }
}

bool importedSurfaceSelectIndex(const ImportedSurface *imported,
                                uint32_t surfaceIndex,
                                uint32_t numSurfaces,
                                ImportedSurface *selected) {
    if (imported == NULL || selected == NULL || !imported->valid ||
        surfaceIndex >= numSurfaces) {
        return false;
    }
    *selected = *imported;
    if (!imported->legacyPrime) {
        return numSurfaces == 1;
    }
    if (imported->numLegacyBuffers != numSurfaces ||
        imported->legacyBuffers[surfaceIndex] > INT_MAX) {
        return false;
    }
    selected->objects[0].fd = (int) imported->legacyBuffers[surfaceIndex];
    return true;
}
