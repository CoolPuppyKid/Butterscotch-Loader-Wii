#pragma once

#include "common.h"
#include "grr_renderer.h"
#include "wii_embedded_data.h"

typedef struct {
    char* title;
    char* rootPath;
    char* dataWinPath;
    char* iconPath;
    char* deviceLabel;
    WiiTexture* iconTexture;
} WiiLaunchEntry;

typedef struct {
    const char* path;
    int voice;
    int16_t* pcm;
    uint32_t pcmBytes;
    int sampleRate;
    int channels;
} WiiLauncherSound;

typedef enum {
    WII_LAUNCHER_SOUND_SELECT = 0,
    WII_LAUNCHER_SOUND_COUNT
} WiiLauncherSoundId;

typedef struct {
    WiiTexture* fallbackGameIcon;
    WiiTexture* btnIcon;
    WiiLauncherSound sounds[WII_LAUNCHER_SOUND_COUNT];
} WiiLauncherData;

bool WiiLauncher_selectGame(WiiLaunchEntry* outEntry);
void WiiLauncher_destroyEntry(WiiLaunchEntry* entry);
