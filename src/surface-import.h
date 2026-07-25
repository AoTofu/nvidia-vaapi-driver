#ifndef SURFACE_IMPORT_H
#define SURFACE_IMPORT_H

#include <stdbool.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_drmcommon.h>

#define NVD_MAX_IMPORTED_OBJECTS 4
#define NVD_MAX_IMPORTED_PLANES 4
#define NVD_MAX_LEGACY_SURFACE_BUFFERS 1024

typedef struct {
    int fd;
    uint64_t size;
    uint64_t modifier;
} ImportedObject;

typedef struct {
    uint32_t objectIndex;
    uint32_t offset;
    uint32_t pitch;
} ImportedPlane;

typedef struct {
    bool requested;
    bool valid;
    bool legacyPrime;
    uint32_t memoryType;
    uint32_t pixelFormat;
    uint32_t width;
    uint32_t height;
    uint64_t dataSize;
    uint32_t numObjects;
    uint32_t numPlanes;
    const uintptr_t *legacyBuffers;
    uint32_t numLegacyBuffers;
    ImportedObject objects[NVD_MAX_IMPORTED_OBJECTS];
    ImportedPlane planes[NVD_MAX_IMPORTED_PLANES];
} ImportedSurface;

void importedSurfaceInit(ImportedSurface *imported);
void parseSurfaceImportAttributes(const VASurfaceAttrib *attribList,
                                  unsigned int numAttribs,
                                  ImportedSurface *imported);
bool importedSurfaceSelectIndex(const ImportedSurface *imported,
                                uint32_t surfaceIndex,
                                uint32_t numSurfaces,
                                ImportedSurface *selected);

#endif
