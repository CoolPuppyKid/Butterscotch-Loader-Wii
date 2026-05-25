#pragma once

#include "common.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* path;
    const uint8_t* data;
    size_t size;
} WiiEmbeddedDataEntryPublic;

extern const WiiEmbeddedDataEntryPublic g_wiiEmbeddedDataEntries[];
extern const size_t g_wiiEmbeddedDataEntryCount;

const WiiEmbeddedDataEntryPublic* WiiEmbeddedData_find(const char* path);
bool WiiEmbeddedData_get(const char* path, const uint8_t** outData, size_t* outSize);
bool WiiEmbeddedData_readBinary(const char* path, uint8_t** outData, int32_t* outSize);
char* WiiEmbeddedData_readText(const char* path);
