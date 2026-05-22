#include "wii_file_system.h"
#include "../json_reader.h"
#include "../utils.h"
#include "wii_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <fat.h>
#include "stb_ds.h"

#define fprintf(stream, ...) WiiLog_fprintf((stream), __VA_ARGS__)

typedef struct {
    char* key;
    char** value;
} WiiFileMapping;

typedef struct {
    FileSystem base;
    char* gameTitle;
    char* basePath;
    char* savePath;
    WiiFileMapping* mappings; // stb_ds string hashmap: filename -> array of paths
    bool sdWritable;
} WiiFileSystem;

static bool hasDevicePrefix(const char* path) {
    return path != NULL && strstr(path, ":/") != NULL;
}

static char* withTrailingSlash(const char* path) {
    if (!path || !*path) return safeStrdup("./");

    size_t len = strlen(path);
    if (path[len - 1] == '/') return safeStrdup(path);

    char* result = safeMalloc(len + 2);
    memcpy(result, path, len);
    result[len] = '/';
    result[len + 1] = '\0';
    return result;
}

static char* pathDirectoryName(const char* path) {
    if (!path || !*path) return safeStrdup("./");

    const char* slash = strrchr(path, '/');
    if (!slash) return safeStrdup("./");

    size_t len = (size_t)(slash - path + 1);
    char* result = safeMalloc(len + 1);
    memcpy(result, path, len);
    result[len] = '\0';
    return result;
}

static char* joinPath(const char* basePath, const char* relativePath) {
    if (!relativePath || !*relativePath) return safeStrdup(basePath);

    size_t baseLen = strlen(basePath);
    size_t relLen = strlen(relativePath);
    char* result = safeMalloc(baseLen + relLen + 1);
    memcpy(result, basePath, baseLen);
    memcpy(result + baseLen, relativePath, relLen);
    result[baseLen + relLen] = '\0';
    return result;
}

static char* expandBasePrefix(WiiFileSystem* wfs, const char* path) {
    if (hasDevicePrefix(path)) {
        return safeStrdup(path);
    }
    return joinPath(wfs->basePath, path ? path : "");
}

static void ensureParentDirectory(const char* path) {
    char buf[256];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return;

    memcpy(buf, path, len + 1);
    char* lastSlash = strrchr(buf, '/');
    if (!lastSlash) return;

    *lastSlash = '\0';
    for (char* p = buf; *p != '\0'; p++) {
        if (*p != '/') continue;
        if (p > buf && p[-1] == ':') continue;

        *p = '\0';
        if (*buf) mkdir(buf, 0777);
        *p = '/';
    }
    mkdir(buf, 0777);
}

static bool getReadableFileSize(const char* path, long* outSize) {
    struct stat st;
    if (stat(path, &st) != 0)
        return false;

    if (S_ISDIR(st.st_mode))
        return false;

    if (st.st_size < 0)
        return false;

    *outSize = (long) st.st_size;
    return true;
}

static bool readBinaryPath(const char* path, uint8_t** outData, int32_t* outSize) {
    long size = 0;
    if (!getReadableFileSize(path, &size))
        return false;

    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t* buf = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(buf, 1, (size_t) size, f);
    fclose(f);

    if (bytesRead != (size_t) size) {
        free(buf);
        return false;
    }

    *outData = buf;
    *outSize = (int32_t) bytesRead;
    return true;
}

static char* wiiResolvePath(FileSystem* fs, const char* relativePath) {
    WiiFileSystem* wfs = (WiiFileSystem*) fs;
    ptrdiff_t idx = shgeti(wfs->mappings, relativePath);
    if (idx < 0) return expandBasePrefix(wfs, relativePath);
    char** arr = wfs->mappings[idx].value;
    if (arrlen(arr) > 0) return safeStrdup(arr[0]);
    return expandBasePrefix(wfs, relativePath);
}

static bool wiiFileExists(FileSystem* fs, const char* relativePath) {
    WiiFileSystem* wfs = (WiiFileSystem*) fs;
    ptrdiff_t idx = shgeti(wfs->mappings, relativePath);
    if (idx < 0) {
        char* fallback = expandBasePrefix(wfs, relativePath);
        long size = 0;
        bool exists = getReadableFileSize(fallback, &size);
        free(fallback);
        return exists;
    }
    char** arr = wfs->mappings[idx].value;
    for (int i = 0; i < arrlen(arr); i++) {
        long size = 0;
        if (getReadableFileSize(arr[i], &size)) return true;
    }
    return false;
}

static bool wiiReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    WiiFileSystem* wfs = (WiiFileSystem*) fs;
    ptrdiff_t idx = shgeti(wfs->mappings, relativePath);
    if (idx < 0) {
        char* fallback = expandBasePrefix(wfs, relativePath);
        bool ok = readBinaryPath(fallback, outData, outSize);
        free(fallback);
        return ok;
    }
    char** arr = wfs->mappings[idx].value;
    for (int i = 0; i < arrlen(arr); i++) {
        if (readBinaryPath(arr[i], outData, outSize)) return true;
    }
    return false;
}

static char* wiiReadFileText(FileSystem* fs, const char* relativePath) {
    uint8_t* data = NULL;
    int32_t size = 0;
    if (!wiiReadFileBinary(fs, relativePath, &data, &size)) return NULL;
    char* text = safeMalloc((size_t) size + 1);
    memcpy(text, data, (size_t) size);
    text[size] = '\0';
    free(data);
    return text;
}

static bool wiiWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    WiiFileSystem* wfs = (WiiFileSystem*) fs;
    if (!wfs->sdWritable) {
        fprintf(stderr, "WiiFileSystem: storage is read-only, skipping write to %s\n", relativePath);
        return false;
    }

    ptrdiff_t idx = shgeti(wfs->mappings, relativePath);
    const char* path = NULL;
    char* fallback = NULL;
    if (idx >= 0 && arrlen(wfs->mappings[idx].value) > 0) {
        path = wfs->mappings[idx].value[0];
    } else {
        fallback = joinPath(wfs->savePath, relativePath);
        path = fallback;
    }
    ensureParentDirectory(path);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "WiiFileSystem: failed to write %s\n", path);
        free(fallback);
        return false;
    }
    bool ok = fwrite(data, 1, (size_t) size, f) == (size_t) size;
    if (fclose(f) != 0)
        ok = false;
    free(fallback);
    return ok;
}

static bool wiiWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    return wiiWriteFileBinary(fs, relativePath, (const uint8_t*) contents, (int32_t) strlen(contents));
}

static bool wiiDeleteFile(FileSystem* fs, const char* relativePath) {
    WiiFileSystem* wfs = (WiiFileSystem*) fs;
    if (!wfs->sdWritable) {
        fprintf(stderr, "WiiFileSystem: storage is read-only, skipping delete of %s\n", relativePath);
        return false;
    }

    ptrdiff_t idx = shgeti(wfs->mappings, relativePath);
    if (idx < 0) return false;
    char** arr = wfs->mappings[idx].value;
    bool any = false;
    for (int i = 0; i < arrlen(arr); i++) {
        if (remove(arr[i]) == 0) any = true;
    }
    return any;
}

static FileSystemVtable wiiFileSystemVtable = {
    .resolvePath     = wiiResolvePath,
    .fileExists      = wiiFileExists,
    .readFileText    = wiiReadFileText,
    .writeFileText   = wiiWriteFileText,
    .deleteFile      = wiiDeleteFile,
    .readFileBinary  = wiiReadFileBinary,
    .writeFileBinary = wiiWriteFileBinary,
};

static WiiFileSystem* wiiFileSystemAlloc(const char* gameTitle, const char* basePath, bool sdWritable) {
    WiiFileSystem* wfs = safeCalloc(1, sizeof(WiiFileSystem));
    wfs->base.vtable = &wiiFileSystemVtable;
    wfs->gameTitle = safeStrdup(gameTitle ? gameTitle : "");
    wfs->basePath = withTrailingSlash(basePath);
    wfs->savePath = joinPath(wfs->basePath, "saves/");
    wfs->sdWritable = sdWritable;
    wfs->mappings = NULL;
    sh_new_strdup(wfs->mappings);
    fprintf(stderr, "WiiFileSystem: base='%s' saves='%s'\n", wfs->basePath, wfs->savePath);
    return wfs;
}

static void wiiFileSystemAddMapping(WiiFileSystem* wfs, const char* gameFileName, const char* path) {
    char** resolvedPaths = NULL;
    char* resolved = expandBasePrefix(wfs, path);
    arrput(resolvedPaths, resolved);
    shput(wfs->mappings, gameFileName, resolvedPaths);
    fprintf(stderr, "WiiFileSystem: '%s' -> '%s'\n", gameFileName, resolved);
}

FileSystem* WiiFileSystem_create(JsonValue* configRoot, const char* gameTitle, const char* basePath, bool sdWritable) {
    JsonValue* fileSystemObj = JsonReader_getObject(configRoot, "fileSystem");
    require(fileSystemObj != NULL && JsonReader_isObject(fileSystemObj));

    WiiFileSystem* wfs = wiiFileSystemAlloc(gameTitle, basePath ? basePath : "./", sdWritable);

    int entryCount = JsonReader_objectLength(fileSystemObj);
    repeat(entryCount, i) {
        const char* gameFileName = JsonReader_getObjectKey(fileSystemObj, i);
        JsonValue* pathArray = JsonReader_getObjectValue(fileSystemObj, i);

        require(JsonReader_isArray(pathArray));

        char** resolvedPaths = NULL;
        int pathCount = JsonReader_arrayLength(pathArray);
        repeat(pathCount, j) {
            JsonValue* pathElement = JsonReader_getArrayElement(pathArray, j);
            require(JsonReader_isString(pathElement));

            const char* rawPath = JsonReader_getString(pathElement);
            char* resolved = expandBasePrefix(wfs, rawPath);
            arrput(resolvedPaths, resolved);
            fprintf(stderr, "WiiFileSystem: '%s' -> '%s'\n", gameFileName, resolved);
        }

        shput(wfs->mappings, gameFileName, resolvedPaths);
    }

    fprintf(stderr, "WiiFileSystem: Loaded %d file mappings\n", (int) shlen(wfs->mappings));
    return (FileSystem*) wfs;
}

FileSystem* WiiFileSystem_createDefault(const char* gameTitle, const char* dataWinPath, bool sdWritable) {
    char* basePath = pathDirectoryName(dataWinPath);
    WiiFileSystem* wfs = wiiFileSystemAlloc(gameTitle, basePath, sdWritable);
    free(basePath);
    const char* mappedDataWinPath = dataWinPath ? dataWinPath : "DATA.WIN";
    if (!hasDevicePrefix(mappedDataWinPath)) {
        const char* slash = strrchr(mappedDataWinPath, '/');
        if (slash) mappedDataWinPath = slash + 1;
    }
    wiiFileSystemAddMapping(wfs, "DATA.WIN", mappedDataWinPath);
    fprintf(stderr, "WiiFileSystem: Loaded %d file mappings\n", (int) shlen(wfs->mappings));
    return (FileSystem*) wfs;
}

void WiiFileSystem_destroy(FileSystem* fs) {
    WiiFileSystem* wfs = (WiiFileSystem*) fs;
    free(wfs->gameTitle);
    free(wfs->basePath);
    free(wfs->savePath);

    int mappingCount = shlen(wfs->mappings);
    repeat(mappingCount, i) {
        char** paths = wfs->mappings[i].value;
        int pathCount = arrlen(paths);
        repeat(pathCount, j) {
            free(paths[j]);
        }
        arrfree(paths);
    }
    shfree(wfs->mappings);
    free(wfs);
}
