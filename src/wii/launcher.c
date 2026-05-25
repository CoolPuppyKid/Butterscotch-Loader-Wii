#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <wiiuse/wpad.h>
#include <gccore.h>
#include <ogc/cache.h>
#include <asndlib.h>

#include "../image/image_decoder.h"
#include "stb_ds.h"
#include "utils.h"

#ifdef IS_BIG_ENDIAN
#define STB_VORBIS_BIG_ENDIAN
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

static WiiLauncherData g_launcherData = {0};

static WiiLauncherSound* getLauncherSound(WiiLauncherSoundId soundId)
{
    if (soundId < 0 || soundId >= WII_LAUNCHER_SOUND_COUNT) return NULL;
    return &g_launcherData.sounds[soundId];
}

static bool pathIsDirectory(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool pathIsRegularFile(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void joinPath(char* outPath, size_t outPathSize, const char* dir, const char* name)
{
    if (!outPath || outPathSize == 0)
        return;

    outPath[0] = '\0';
    size_t used = 0;
    const char* parts[] = {
        dir ? dir : "./",
        NULL,
        name ? name : ""
    };
    size_t len = dir ? strlen(dir) : 0;
    parts[1] = (len > 0 && dir[len - 1] == '/') ? "" : "/";

    for (int i = 0; i < 3 && used + 1 < outPathSize; i++) {
        size_t partLen = strlen(parts[i]);
        size_t copyLen = partLen;
        size_t available = outPathSize - used - 1;
        if (copyLen > available)
            copyLen = available;
        memcpy(outPath + used, parts[i], copyLen);
        used += copyLen;
        outPath[used] = '\0';
    }
}

static char* duplicateStringRange(const char* src, size_t len)
{
    char* copy = safeMalloc(len + 1);
    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

static char* pathBaseNameDup(const char* path)
{
    if (!path || !path[0]) return safeStrdup("GAME");
    const char* end = path + strlen(path);
    while (end > path && end[-1] == '/') end--;
    const char* start = end;
    while (start > path && start[-1] != '/') start--;
    return duplicateStringRange(start, (size_t)(end - start));
}

static int launchEntryCompare(const void* a, const void* b)
{
    const WiiLaunchEntry* lhs = (const WiiLaunchEntry*)a;
    const WiiLaunchEntry* rhs = (const WiiLaunchEntry*)b;
    return strcasecmp(lhs->title ? lhs->title : "", rhs->title ? rhs->title : "");
}

static void freeLaunchEntries(WiiLaunchEntry* entries)
{
    for (int i = 0; i < arrlen(entries); i++) {
        WiiLauncher_destroyEntry(&entries[i]);
    }
    arrfree(entries);
}

static void cleanupLauncherUi(void)
{
    WiiGX_destroyTexture(g_launcherData.fallbackGameIcon);
    WiiGX_destroyTexture(g_launcherData.btnIcon);
    g_launcherData.fallbackGameIcon = NULL;
    g_launcherData.btnIcon = NULL;
    for (int i = 0; i < WII_LAUNCHER_SOUND_COUNT; i++) {
#ifdef BUTTERSCOTCH_WII_ENABLE_AUDIO
        ASND_StopVoice(g_launcherData.sounds[i].voice);
#endif
        free(g_launcherData.sounds[i].pcm);
        g_launcherData.sounds[i].pcm = NULL;
        g_launcherData.sounds[i].pcmBytes = 0;
        g_launcherData.sounds[i].sampleRate = 0;
        g_launcherData.sounds[i].channels = 0;
    }
}

void WiiLauncher_destroyEntry(WiiLaunchEntry* entry)
{
    if (!entry) return;
    free(entry->title);
    free(entry->rootPath);
    free(entry->dataWinPath);
    free(entry->iconPath);
    free(entry->deviceLabel);
    WiiGX_destroyTexture(entry->iconTexture);
    memset(entry, 0, sizeof(*entry));
}

static WiiTexture* loadLauncherIcon(const char* path)
{
    if (!path) return NULL;

    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t* blob = safeMalloc((size_t) size);
    size_t bytesRead = fread(blob, 1, (size_t) size, f);
    fclose(f);
    if (bytesRead != (size_t) size) {
        free(blob);
        return NULL;
    }

    int width = 0;
    int height = 0;
    uint8_t* rgba = ImageDecoder_decodeToRgba(blob, (size_t) size, false, &width, &height);
    free(blob);
    if (!rgba) return NULL;

    WiiTexture* tex = WiiGX_createTextureFromRgba(rgba, width, height);
    free(rgba);
    return tex;
}

static WiiTexture* loadEmbeddedLauncherIcon(const char* path)
{
    if (!path) return NULL;

    uint8_t* data = NULL;
    size_t size = 0;
    if (!WiiEmbeddedData_readBinary(path, &data, &size))
    {
        return NULL;
    }

    int width = 0;
    int height = 0;
    uint8_t* rgba = ImageDecoder_decodeToRgba(data, (size_t) size, false, &width, &height);
    free(data);
    if (!rgba) return NULL;

    WiiTexture* tex = WiiGX_createTextureFromRgba(rgba, width, height);
    free(rgba);
    return tex;
}

static int launcherVoiceFormatForChannels(int channels)
{
    return channels > 1 ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
}

static bool loadEmbeddedLauncherSound(WiiLauncherSoundId soundId, const char* path, int voice)
{
#ifndef BUTTERSCOTCH_WII_ENABLE_AUDIO
    (void)soundId;
    (void)path;
    (void)voice;
    return false;
#else
    WiiLauncherSound* sound = getLauncherSound(soundId);
    if (!sound || !path) return false;

    uint8_t* data = NULL;
    size_t size = 0;
    if (!WiiEmbeddedData_readBinary(path, &data, &size))
        return false;

    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, (int)size, &error, NULL);
    if (!vorbis) {
        free(data);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    uint32_t frameCount = stb_vorbis_stream_length_in_samples(vorbis);
    if (frameCount == 0 || info.channels < 1 || info.channels > 2) {
        stb_vorbis_close(vorbis);
        free(data);
        return false;
    }

    size_t pcmSamples = (size_t)frameCount * (size_t)info.channels;
    int16_t* pcm = safeMemalign(32, pcmSamples * sizeof(int16_t));
    size_t decodedSamples = 0;
    while (decodedSamples < pcmSamples) {
        int got = stb_vorbis_get_samples_short_interleaved(
            vorbis,
            info.channels,
            pcm + decodedSamples,
            (int)(pcmSamples - decodedSamples)
        );
        if (got <= 0)
            break;
        decodedSamples += (size_t)got * (size_t)info.channels;
    }

    stb_vorbis_close(vorbis);
    free(data);

    if (decodedSamples == 0) {
        free(pcm);
        return false;
    }

    sound->path = path;
    sound->voice = voice;
    sound->pcm = pcm;
    sound->pcmBytes = (uint32_t)(decodedSamples * sizeof(int16_t));
    sound->sampleRate = (int)info.sample_rate;
    sound->channels = info.channels;
    DCFlushRange(sound->pcm, sound->pcmBytes);
    return true;
#endif
}

static void playLauncherSound(WiiLauncherSoundId soundId)
{
#ifdef BUTTERSCOTCH_WII_ENABLE_AUDIO
    WiiLauncherSound* sound = getLauncherSound(soundId);
    if (!sound || !sound->pcm || sound->pcmBytes == 0 || sound->sampleRate <= 0)
        return;
    ASND_StopVoice(sound->voice);
    DCFlushRange(sound->pcm, sound->pcmBytes);
    ASND_SetVoice(
        sound->voice,
        launcherVoiceFormatForChannels(sound->channels),
        sound->sampleRate,
        0,
        sound->pcm,
        sound->pcmBytes,
        255,
        255,
        NULL
    );
#endif
}

static void addLaunchEntry(WiiLaunchEntry** entries, const char* rootPath, const char* deviceLabel)
{
    char dataWinPath[256];
    joinPath(dataWinPath, sizeof(dataWinPath), rootPath, "DATA.WIN");
    if (!pathIsRegularFile(dataWinPath)) return;

    char iconPath[256];
    joinPath(iconPath, sizeof(iconPath), rootPath, "icon.png");

    for (int i = 0; i < arrlen(*entries); i++) {
        if (strcmp((*entries)[i].dataWinPath, dataWinPath) == 0) return;
    }

    WiiLaunchEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.rootPath = safeStrdup(rootPath);
    entry.dataWinPath = safeStrdup(dataWinPath);
    entry.iconPath = pathIsRegularFile(iconPath) ? safeStrdup(iconPath) : NULL;
    entry.deviceLabel = safeStrdup(deviceLabel);
    entry.title = pathBaseNameDup(rootPath);
    entry.iconTexture = loadLauncherIcon(entry.iconPath);
    arrput(*entries, entry);
}

static void scanLaunchRoots(WiiLaunchEntry** entries, const char* devicePrefix, const char* deviceLabel)
{
    char basePath[256];
    snprintf(basePath, sizeof(basePath), "%s/butterscotch", devicePrefix);
    if (!pathIsDirectory(basePath)) return;

    addLaunchEntry(entries, basePath, deviceLabel);

    DIR* dir = opendir(basePath);
    if (!dir) return;

    struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char childPath[256];
        joinPath(childPath, sizeof(childPath), basePath, ent->d_name);
        if (!pathIsDirectory(childPath))
            continue;

        addLaunchEntry(entries, childPath, deviceLabel);
    }

    closedir(dir);
}

static void drawLauncherCard(float x, float y, float w, float h, bool selected, WiiLaunchEntry* entry)
{
    GXColor border = selected ? (GXColor){0, 220, 255, 255} : (GXColor){110, 110, 120, 255};
    GXColor fill = (GXColor){18, 18, 22, 255};
    GXColor text = selected ? (GXColor){255, 255, 255, 255} : (GXColor){90, 90, 210, 255};

    WiiGX_useColorOnly();
    WiiGX_drawRect(x - 3.0f, y - 3.0f, x + w + 3.0f, y + h + 3.0f, border);
    WiiGX_drawRect(x, y, x + w, y + h, fill);

    if (entry && entry->iconTexture) {
        WiiGX_drawTextureRect(entry->iconTexture, x, y, w, h, (GXColor){255, 255, 255, 255});
    } else {
        if (g_launcherData.fallbackGameIcon) {
            WiiGX_drawTextureRect(g_launcherData.fallbackGameIcon, x, y, w, h, (GXColor){255, 255, 255, 255});
        } else {
            WiiGX_useColorOnly();
            WiiGX_drawRect(x + 10.0f, y + 10.0f, x + w - 10.0f, y + h - 10.0f, (GXColor){28, 28, 34, 255});
        }
    }

    if (entry && entry->title) {
        WiiGX_drawBitmapText(x, y - 30.0f, entry->title, 2, text);
    }
    if (entry && entry->deviceLabel) {
        WiiGX_drawBitmapText(x, y - 15.0f, entry->deviceLabel, 1, (GXColor){145, 190, 220, 255});
    }
}

static void drawLauncherScreen(WiiLaunchEntry* entries, float visualSelected)
{
    float scrW = (float) g_wiiRmode->fbWidth;
    float scrH = (float) g_wiiRmode->efbHeight;
    float centerX = scrW * 0.5f;
    float cardW = 193.0f;
    float cardH = 286.0f;
    float gap = 18.0f;
    float centerY = scrH * 0.5f - 12.0f;

    WiiGX_beginFrame2D();
    WiiGX_useColorOnly();
    WiiGX_drawBitmapText(20.0f, 18.0f, "BUTTERSCOTCH", 2, (GXColor){240, 240, 245, 255});
    WiiGX_drawBitmapText(20.0f, 40.0f, "LEFT/RIGHT TO CHOOSE | A TO LAUNCH | HOME TO EXIT", 1, (GXColor){140, 160, 180, 255});


    int baseIndex = (int) floorf(visualSelected);
    for (int index = baseIndex - 1; index <= baseIndex + 2; index++) {
        if (index < 0 || index >= arrlen(entries)) continue;

        float rel = (float) index - visualSelected;
        if (rel < -1.75f || rel > 1.75f) continue;

        float x = centerX - cardW * 0.5f + rel * (cardW + gap);
        float y = centerY - cardH * 0.5f;
        float scaleT = fabsf(rel);
        if (scaleT > 1.0f) scaleT = 1.0f;
        float scale = 1.0f - 0.1f * scaleT;
        float drawW = cardW * scale;
        float drawH = cardH * scale;
        float drawX = x + (cardW - drawW) * 0.5f;
        float drawY = y + (cardH - drawH) * 0.5f;
        drawLauncherCard(drawX, drawY, drawW, drawH, fabsf(rel) < 0.35f, &entries[index]);
    }

    // not done yet
    //float btnW = 230.0f;
    //float btnH = 60.0f;
    //float btnX = centerX - btnW * 0.5f;
    //float btnY = scrH - 80.0f;
    //WiiGX_drawTextureRect(g_launcherData.btnIcon, btnX, btnY, btnW, btnH, (GXColor){255, 255, 255, 255});
    //float textW = 0.0f;
    //float textH = 0.0f;
    //WiiGX_measureBitmapText("LAUNCH", 3, &textW, &textH);
    //WiiGX_drawBitmapText(centerX - textW / 2.0f, scrH - 65.0f, "LAUNCH", 3, (GXColor){16, 18, 22, 255});

    WiiGX_presentFrame();
}

bool WiiLauncher_selectGame(WiiLaunchEntry* outEntry)
{
    if (!outEntry) return false;
    g_launcherData.fallbackGameIcon = loadEmbeddedLauncherIcon("ui/fallback_game.png");
    g_launcherData.btnIcon = loadEmbeddedLauncherIcon("ui/btn.png");
    loadEmbeddedLauncherSound(WII_LAUNCHER_SOUND_SELECT, "sfx/select.ogg", 15);

    WiiLaunchEntry* entries = NULL;
    scanLaunchRoots(&entries, "sd:", "SD");
    scanLaunchRoots(&entries, "usb:", "USB");
    if (arrlen(entries) > 1) {
        qsort(entries, (size_t) arrlen(entries), sizeof(entries[0]), launchEntryCompare);
    }
    if (arrlen(entries) <= 0) {
        freeLaunchEntries(entries);
        cleanupLauncherUi();
        return false;
    }

    int selected = 0;
    float visualSelected = 0.0f;
    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        uint32_t down = WPAD_ButtonsDown(WPAD_CHAN_0);
        if (down & WPAD_BUTTON_HOME) {
            freeLaunchEntries(entries);
            cleanupLauncherUi();
            exit(0);
        }
        int previousSelected = selected;
        if ((down & WPAD_BUTTON_LEFT) && selected > 0)
            selected--;
        if ((down & WPAD_BUTTON_RIGHT) && selected + 1 < arrlen(entries))
            selected++;
        if (selected != previousSelected)
            playLauncherSound(WII_LAUNCHER_SOUND_SELECT);
        if (down & (WPAD_BUTTON_A | WPAD_BUTTON_2 | WPAD_BUTTON_PLUS)) {
            *outEntry = entries[selected];
            arrdel(entries, selected);
            freeLaunchEntries(entries);
            cleanupLauncherUi();
            return true;
        }

        float targetSelected = (float) selected;
        visualSelected += (targetSelected - visualSelected) * 0.22f;
        if (fabsf(targetSelected - visualSelected) < 0.001f) {
            visualSelected = targetSelected;
        }

        drawLauncherScreen(entries, visualSelected);
    }

    freeLaunchEntries(entries);
    cleanupLauncherUi();
    return false;
}
