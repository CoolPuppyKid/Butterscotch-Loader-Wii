#include "wii_embedded_data.h"

#include "../utils.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char* normalizeEmbeddedPath(const char* path)
{
    if (!path) return NULL;
    if (strncmp(path, "data/", 5) == 0) return path + 5;
    if (strncmp(path, "./data/", 7) == 0) return path + 7;
    return path;
}

const WiiEmbeddedDataEntryPublic* WiiEmbeddedData_find(const char* path)
{
    const char* normalized = normalizeEmbeddedPath(path);
    if (!normalized || !normalized[0]) return NULL;

    for (size_t i = 0; i < g_wiiEmbeddedDataEntryCount; i++) {
        if (strcmp(g_wiiEmbeddedDataEntries[i].path, normalized) == 0) {
            return &g_wiiEmbeddedDataEntries[i];
        }
    }
    return NULL;
}

bool WiiEmbeddedData_get(const char* path, const uint8_t** outData, size_t* outSize)
{
    const WiiEmbeddedDataEntryPublic* entry = WiiEmbeddedData_find(path);
    if (!entry) return false;
    if (outData) *outData = entry->data;
    if (outSize) *outSize = entry->size;
    return true;
}

bool WiiEmbeddedData_readBinary(const char* path, uint8_t** outData, int32_t* outSize)
{
    const uint8_t* data = NULL;
    size_t size = 0;
    if (!outData || !outSize) return false;
    if (!WiiEmbeddedData_get(path, &data, &size)) return false;

    if (size > (size_t) INT32_MAX) return false;
    uint8_t* copy = safeMalloc(size + 1u);
    memcpy(copy, data, size);
    copy[size] = 0;
    *outData = copy;
    *outSize = (int32_t) size;
    return true;
}

char* WiiEmbeddedData_readText(const char* path)
{
    const uint8_t* data = NULL;
    size_t size = 0;
    if (!WiiEmbeddedData_get(path, &data, &size)) return NULL;

    char* text = safeMalloc(size + 1u);
    memcpy(text, data, size);
    text[size] = '\0';
    return text;
}
