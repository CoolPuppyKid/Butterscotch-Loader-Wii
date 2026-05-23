#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <ogc/audio.h>
#include <asndlib.h>
#include <ogc/lwp_watchdog.h>

#include "runner.h"
#include "runner_gamepad.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "../data_win.h"
#include "wii_file_system.h"
#include "wii_audio_system.h"
#include "grr_renderer.h"
#include "noop_audio_system.h"
#include "wii_log.h"
#include "utils.h"
#include "stb_ds.h"

#define fprintf(stream, ...) WiiLog_fprintf((stream), __VA_ARGS__)

static const char* kDataWinPath = "sd:/butterscotch/DATA.WIN";

static bool g_sdWritable = false;
static uint32_t g_dbgStepMs = 0;
static uint32_t g_dbgDrawMs = 0;
static uint32_t g_dbgCapMs = 0;

static bool getDirectoryName(const char* path, char* outDir, size_t outDirSize)
{
    if (!path || !outDir || outDirSize == 0)
        return false;

    const char* slash = strrchr(path, '/');
    if (!slash) {
        snprintf(outDir, outDirSize, "./");
        return true;
    }

    size_t len = (size_t)(slash - path + 1);
    if (len >= outDirSize)
        len = outDirSize - 1;

    memcpy(outDir, path, len);
    outDir[len] = '\0';
    return true;
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

static void logDirectoryListing(const char* dir)
{
    DIR* pdir = opendir(dir);
    if (!pdir) {
        fprintf(stderr, "opendir('%s') failed\n", dir ? dir : "(null)");
        return;
    }

    fprintf(stderr, "Directory listing for %s\n", dir);
    struct dirent* pent = NULL;
    while ((pent = readdir(pdir)) != NULL) {
        if (strcmp(".", pent->d_name) == 0 || strcmp("..", pent->d_name) == 0)
            continue;

        char path[256];
        struct stat statbuf;
        joinPath(path, sizeof(path), dir, pent->d_name);
        if (stat(path, &statbuf) != 0) {
            fprintf(stderr, "  %s <stat failed>\n", pent->d_name);
            continue;
        }

        if (S_ISDIR(statbuf.st_mode))
            fprintf(stderr, "  %s <dir>\n", pent->d_name);
        else
            fprintf(stderr, "  %s %lld\n", pent->d_name, (long long)statbuf.st_size);
    }
    closedir(pdir);
}

static void openLogFile(const char* baseDir)
{
#ifndef BUTTERSCOTCH_WII_SD_LOGS
    (void)baseDir;
    return;
#endif

    if (!g_sdWritable)
        return;

    char logPath[256];
    joinPath(logPath, sizeof(logPath), baseDir, "butterscotch.log");

    if (WiiLog_open(logPath))
        fprintf(stderr, "=== Butterscotch Wii log opened ===\n");
    else
        g_sdWritable = false;
}

static bool pathIsDirectory(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensureDirectory(const char* path)
{
    if (pathIsDirectory(path))
        return true;
    return mkdir(path, 0777) == 0 || pathIsDirectory(path);
}

static bool probeWriteAccess(const char* baseDir)
{
    if (!ensureDirectory(baseDir))
        return false;

    char probePath[256];
    joinPath(probePath, sizeof(probePath), baseDir, ".write_probe.tmp");

    FILE* f = fopen(probePath, "wb");
    if (!f)
        return false;

    static const char probeText[] = "write probe\n";
    bool ok = fwrite(probeText, 1, strlen(probeText), f) == strlen(probeText);
    if (fclose(f) != 0)
        ok = false;

    if (ok)
        remove(probePath);

    return ok;
}

static void showSdWriteWarning(void)
{
    const char* lines[] = {
        "STORAGE WRITE ACCESS IS UNAVAILABLE",
        "SAVES AND LOGS WILL NOT BE WRITTEN",
        "THE GAME WILL CONTINUE IN 4 SECONDS"
    };
    WiiGX_drawTextScreen("WARNING", lines, 3);

    u64 start = gettime();
    while (SYS_MainLoop() && ticks_to_millisecs(gettime() - start) < 4000) {
        VIDEO_WaitVSync();
    }
}

static uint32_t countLayerInstances(Room* room) {
    if (!room || !room->payloadLoaded || !room->layers) return 0;
    uint32_t total = 0;
    repeat(room->layerCount, i) {
        RoomLayer* layer = &room->layers[i];
        if (layer->type == RoomLayerType_Instances && layer->instancesData != NULL) {
            total += layer->instancesData->instanceCount;
        }
    }
    return total;
}

// ===[ Input ]=============================================================

static void pollWiimote(RunnerKeyboardState* kb, RunnerGamepadState* gp) {
    if (!kb || !gp) return;

    uint32_t held = WPAD_ButtonsHeld(WPAD_CHAN_0);

    struct expansion_t exp;
    memset(&exp, 0, sizeof(exp));
    WPAD_Expansion(WPAD_CHAN_0, &exp);

    float axisX = 0.0f, axisY = 0.0f;
    if (exp.type == WPAD_EXP_NUNCHUK || exp.type == EXP_NUNCHUK) {
        if (exp.nunchuk.js.mag >= 0.25f) {
            float rad = exp.nunchuk.js.ang * ((float)M_PI / 180.0f);
            axisX =  sinf(rad) * exp.nunchuk.js.mag;
            axisY = -cosf(rad) * exp.nunchuk.js.mag;
            if (axisX < -1.0f) axisX = -1.0f;
            if (axisX >  1.0f) axisX =  1.0f;
            if (axisY < -1.0f) axisY = -1.0f;
            if (axisY >  1.0f) axisY =  1.0f;
        }
    }

    bool wantUp    = (held & WPAD_BUTTON_UP)    || axisY < -0.35f;
    bool wantDown  = (held & WPAD_BUTTON_DOWN)  || axisY >  0.35f;
    bool wantLeft  = (held & WPAD_BUTTON_LEFT)  || axisX < -0.35f;
    bool wantRight = (held & WPAD_BUTTON_RIGHT) || axisX >  0.35f;
    bool wantZ     = (held & (WPAD_BUTTON_A | WPAD_BUTTON_2)) != 0;
    bool wantX     = (held & WPAD_BUTTON_1)     != 0;
    bool wantEnter = (held & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) != 0;
    bool wantShift = (held & WPAD_BUTTON_1)     != 0;
    bool wantCtrl  = (held & WPAD_BUTTON_B)     != 0;
    bool wantEsc   = (held & WPAD_BUTTON_HOME)  != 0;
    bool wantBack  = (held & WPAD_BUTTON_MINUS) != 0;

    // Keyboard state
    #define SYNC_KEY(k, down) do { \
        if ((down) && !kb->keyDown[k])  RunnerKeyboard_onKeyDown(kb, k); \
        else if (!(down) && kb->keyDown[k]) RunnerKeyboard_onKeyUp(kb, k); \
    } while(0)

    SYNC_KEY(VK_UP,        wantUp);
    SYNC_KEY(VK_DOWN,      wantDown);
    SYNC_KEY(VK_LEFT,      wantLeft);
    SYNC_KEY(VK_RIGHT,     wantRight);
    SYNC_KEY(VK_ENTER,     wantEnter);
    SYNC_KEY(VK_SHIFT,     wantShift);
    SYNC_KEY(VK_CONTROL,   wantCtrl);
    SYNC_KEY(VK_ESCAPE,    wantEsc);
    SYNC_KEY(VK_BACKSPACE, wantBack);
    SYNC_KEY('Z',          wantZ);
    SYNC_KEY('X',          wantX);

    #undef SYNC_KEY

    // Gamepad state
    GamepadSlot* slot = &gp->slots[0];
    slot->connected = true;
    slot->jid = 0;
    strcpy(slot->description, "Wiimote");
    strcpy(slot->guid, "butterscotch-wiimote");
    gp->connectedCount = 1;

    #define SET_BTN(idx, down) do { \
        slot->buttonDownPrev[idx] = slot->buttonDown[idx]; \
        if ((down)  && !slot->buttonDown[idx]) slot->buttonPressed[idx]  = true; \
        if (!(down) &&  slot->buttonDown[idx]) slot->buttonReleased[idx] = true; \
        slot->buttonDown[idx]  = (down); \
        slot->buttonValue[idx] = (down) ? 1.0f : 0.0f; \
    } while(0)

    SET_BTN(0,  wantZ);
    SET_BTN(1,  wantX);
    SET_BTN(2,  wantCtrl);
    SET_BTN(3,  false);
    SET_BTN(8,  (held & WPAD_BUTTON_MINUS) != 0);
    SET_BTN(9,  (held & WPAD_BUTTON_PLUS)  != 0);
    SET_BTN(12, wantUp);
    SET_BTN(13, wantDown);
    SET_BTN(14, wantLeft);
    SET_BTN(15, wantRight);
    SET_BTN(16, wantEsc);

    #undef SET_BTN

    slot->axisValue[0] = axisX != 0.0f ? axisX : (wantLeft && !wantRight ? -1.0f : (wantRight && !wantLeft ?  1.0f : 0.0f));
    slot->axisValue[1] = axisY != 0.0f ? axisY : (wantUp   && !wantDown  ? -1.0f : (wantDown  && !wantUp   ?  1.0f : 0.0f));
    slot->axisValue[2] = 0.0f;
    slot->axisValue[3] = 0.0f;
}

// ===[ Fatal error ]=======================================================

static void fatalErrorLoop(const char* const* lines, int lineCount)
{
    WiiGX_drawTextScreen("FATAL", lines, lineCount);
    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(1);
        VIDEO_WaitVSync();
    }
    exit(1);
}

static void fatalError(const char* msg)
{
    const char* lines[] = {
        msg,
        "PRESS HOME TO EXIT"
    };
    fprintf(stderr, "FATAL: %s\n", msg);
    fatalErrorLoop(lines, 2);
}

static void loadingStep(int phase, int totalPhases, const char* status)
{
    WiiGX_drawLoadingStatus(phase, totalPhases, status);
    VIDEO_WaitVSync();
    fprintf(stderr, "Loading %d/%d: %s\n", phase, totalPhases, status ? status : "");
}

static void dataWinLogProgressCallback(const char* chunkName, int chunkIndex, int totalChunks, MAYBE_UNUSED DataWin* dataWin, MAYBE_UNUSED void* userData)
{
    if (totalChunks > 0) {
        fprintf(stderr, "DATA.WIN chunk %.4s %d/%d\n", chunkName, chunkIndex + 1, totalChunks);
    } else {
        fprintf(stderr, "DATA.WIN scan %.4s\n", chunkName);
    }
}

// =========================================================================

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    WiiGX_initVideo();
    loadingStep(0, 10, "STARTING");

    loadingStep(1, 10, "INIT FAT");
    if (!fatInitDefault()) {
        fatalError("fatInitDefault() failed - is your SD card inserted?");
    }
    logDirectoryListing(".");

    loadingStep(2, 10, "INIT INPUT");
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC);
    fprintf(stderr, "WPAD initialized\n");

#ifdef BUTTERSCOTCH_WII_ENABLE_AUDIO
    loadingStep(3, 10, "AUDIO INIT");
    AUDIO_Init(NULL);
    loadingStep(3, 10, "ASND INIT");
    ASND_Init();
    loadingStep(3, 10, "ASND START");
    ASND_Pause(0);
    fprintf(stderr, "Audio initialized\n");
#else
    loadingStep(3, 10, "AUDIO DISABLED");
    fprintf(stderr, "Audio disabled for this Wii build\n");
#endif

    loadingStep(4, 10, "OPEN DATA.WIN");
    FILE* dataWinProbe = fopen(kDataWinPath, "rb");
    if (!dataWinProbe) {
        fatalError("DATA.WIN could not be opened at sd:/butterscotch/DATA.WIN");
    }
    fclose(dataWinProbe);

    char dataWinDir[256];
    getDirectoryName(kDataWinPath, dataWinDir, sizeof(dataWinDir));
    logDirectoryListing(dataWinDir);

    char saveDir[256];
    joinPath(saveDir, sizeof(saveDir), dataWinDir, "saves");
    g_sdWritable = probeWriteAccess(dataWinDir);
    if (g_sdWritable) {
        ensureDirectory(saveDir);
    } else {
        showSdWriteWarning();
    }

    openLogFile(dataWinDir);
    fprintf(stderr, "FAT initialized (%s)\n", g_sdWritable ? "read/write" : "read-only");
    fprintf(stderr, "Loading %s\n", kDataWinPath);

    loadingStep(5, 10, "PARSE DATA.WIN");

    DataWin* dataWin = DataWin_parse(kDataWinPath,
        (DataWinParserOptions){
            .parseGen8=true,.parseOptn=true,.parseLang=true,.parseExtn=false,
            .parseSond=true,.parseAgrp=true,.parseSprt=true,.parseBgnd=true,
            .parsePath=true,.parseScpt=true,.parseGlob=true,.parseShdr=true,
            .parseFont=true,.parseTmln=true,.parseObjt=true,.parseRoom=true,
            .parseTpag=true,.parseCode=true,.parseVari=true,.parseFunc=true,
            .parseStrg=true,.parseTxtr=true,.parseAudo=false,
            .skipLoadingPreciseMasksForNonPreciseSprites=true,
            .lazyLoadRooms=false,.lazyLoadTextureData=true,.eagerlyLoadedRooms=NULL,
            .progressCallback=dataWinLogProgressCallback,.progressCallbackUserData=NULL,
        });
    if (!dataWin) fatalError("Failed to parse DATA.WIN. Check butterscotch.log.");
    fprintf(stderr, "DATA.WIN loaded (%u textures, %u sprites)\n",
            dataWin->txtr.count, dataWin->sprt.count);
    loadingStep(6, 10, "CREATE FILE SYSTEM");

    FileSystem* fileSystem = WiiFileSystem_createDefault(dataWin->gen8.displayName, kDataWinPath, g_sdWritable);
    loadingStep(7, 10, "CREATE VM");

    VMContext* vm = VM_create(dataWin);
    if (!vm) fatalError("VM_create failed.");

    loadingStep(8, 10, "CREATE RENDERER");
    Renderer* renderer = (Renderer*) GRRRenderer_create();
    if (!renderer) fatalError("GRRRenderer_create failed.");

    loadingStep(9, 10, "CREATE AUDIO SYSTEM");
#ifdef BUTTERSCOTCH_WII_ENABLE_AUDIO
    AudioSystem* audioSystem = (AudioSystem*) WiiAudioSystem_create();
#else
    AudioSystem* audioSystem = (AudioSystem*) NoopAudioSystem_create();
#endif
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);

    loadingStep(9, 10, "INIT RUNNER");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);
    loadingStep(9, 10, "PRELOAD ASSETS");
    GRRRenderer_preloadStartupAssets(renderer, dataWin);
    loadingStep(9, 10, "INIT FIRST ROOM");
    Runner_initFirstRoom(runner);
    if (!runner->currentRoom) fatalError("Failed to load first room. Check butterscotch.log.");
    fprintf(stderr, "First room: %s\n", runner->currentRoom->name ? runner->currentRoom->name : "(unnamed)");
    loadingStep(10, 10, "START ROOM");

    int32_t gameW   = (int32_t) dataWin->gen8.defaultWindowWidth;
    int32_t gameH   = (int32_t) dataWin->gen8.defaultWindowHeight;
    int32_t screenW = g_wiiRmode->fbWidth;
    int32_t screenH = g_wiiRmode->efbHeight;
    fprintf(stderr, "Game %dx%d  Screen %dx%d\n", gameW, gameH, screenW, screenH);

    // ===[ Main Loop ]===
    while (!runner->shouldExit && SYS_MainLoop()) {
        u64 frameStart = gettime();

        WPAD_ScanPads();
        RunnerGamepad_beginFrame(runner->gamepads);
        pollWiimote(runner->keyboard, runner->gamepads);

        uint32_t roomSpeed = runner->currentRoom ? runner->currentRoom->speed : 0;

        u64 stepStart = gettime();
        Runner_step(runner);
        g_dbgStepMs = ticks_to_millisecs(gettime() - stepStart);

        u64 drawStart = gettime();
        float displayScaleX = 1.0f;
        float displayScaleY = 1.0f;
        Runner_computeViewDisplayScale(runner, gameW, gameH, &displayScaleX, &displayScaleY);
        Runner_beginFrame(runner, gameW, gameH, screenW, screenH);
        Runner_drawViews(runner, gameW, gameH, displayScaleX, displayScaleY, false);
        if (runner->pendingRoom == -1) {
            renderer->vtable->endFrameEnd(renderer);
        }
        Runner_handlePendingRoomChange(runner);
        g_dbgDrawMs = ticks_to_millisecs(gettime() - drawStart);

        if ((runner->frameCount % 60) == 0) {
            fprintf(stderr, "frame=%d room=%s step=%ums draw=%ums cap=%ums\n",
                    runner->frameCount,
                    runner->currentRoom && runner->currentRoom->name ? runner->currentRoom->name : "?",
                    g_dbgStepMs, g_dbgDrawMs, g_dbgCapMs);
        }

        float dt = roomSpeed > 0 ? 1.0f / (float)roomSpeed : 0.0f;
        runner->audioSystem->vtable->update(runner->audioSystem, dt);

        g_dbgCapMs = 0;
        if (roomSpeed > 0) {
            u64 capStart = gettime();
            u64 targetTicks = secs_to_ticks(1) / roomSpeed;
            while (gettime() - frameStart < targetTicks) usleep(1000);
            g_dbgCapMs = ticks_to_millisecs(gettime() - capStart);
        }
    }

    fprintf(stderr, "Exiting\n");
    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);
    WiiGX_shutdownVideo();
    WiiLog_close();
    return 0;
}
