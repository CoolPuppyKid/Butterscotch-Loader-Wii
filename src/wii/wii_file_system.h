#pragma once

#include "common.h"
#include "../file_system.h"
#include "../json_reader.h"
#include <fat.h>

FileSystem* WiiFileSystem_create(JsonValue* configRoot, const char* gameTitle, bool sdWritable);
FileSystem* WiiFileSystem_createDefault(const char* gameTitle, const char* dataWinPath, bool sdWritable);
void WiiFileSystem_destroy(FileSystem* fs);
