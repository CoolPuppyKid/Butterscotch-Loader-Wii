#pragma once

#include "common.h"
#include "renderer.h"
#include "data_win.h"

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#define GX_MAX_QUADS 4096

typedef struct {
    void* data;
    u32 w;
    u32 h;
    u8 format;
} WiiTexture;

extern GXRModeObj* g_wiiRmode;
extern void* g_wiiXfb[2];
extern u32 g_wiiFb;

void WiiGX_initVideo(void);
void WiiGX_shutdownVideo(void);
void WiiGX_drawLoadingScreen(int phase, int totalPhases);
void WiiGX_drawLoadingStatus(int phase, int totalPhases, const char* status);
void WiiGX_drawTextScreen(const char* title, const char* const* lines, int lineCount);
void WiiGX_beginFrame2D(void);
void WiiGX_presentFrame(void);
void WiiGX_useColorOnly(void);
void WiiGX_useTexture(WiiTexture* tex);
void WiiGX_drawRect(float x0, float y0, float x1, float y1, GXColor color);
void WiiGX_drawTextureRect(WiiTexture* tex, float x, float y, float w, float h, GXColor color);
void WiiGX_drawBitmapText(float x, float y, const char* text, int scale, GXColor color);
void WiiGX_drawBitmapTextAlpha(float x, float y, const char* text, int scale, uint8_t r, uint8_t g, uint8_t b, float alpha);
void WiiGX_measureBitmapText(const char* text, int scale, float* outWidth, float* outHeight);
WiiTexture* WiiGX_createTextureFromRgba(const uint8_t* pixels, int32_t w, int32_t h);
void WiiGX_destroyTexture(WiiTexture* tex);
void WiiGX_setDebugOverlay(const char* line1, const char* line2);
void WiiGX_getFrameStats(uint32_t* outQuads, uint32_t* outSpriteDraws, uint32_t* outTextureLoads);
void GRRRenderer_preloadStartupAssets(Renderer* renderer, DataWin* dataWin);
void GRRRenderer_preloadRoomAssets(Renderer* renderer, DataWin* dataWin, int32_t roomIndex);

typedef struct {
    Renderer base;

    // Textures
    WiiTexture** textures;     // replaces glTextures + widths + heights + loaded
    uint8_t** decodedTexturePixels;
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t* decodedTextureSizes;
    uint32_t* decodedTextureLastUsed;
    uint32_t decodedTextureBytes;
    uint32_t* textureSizes;
    uint32_t* textureLastUsed;
    uint32_t textureBytes;
    bool* textureLoaded;
    bool* textureOversized;
    uint32_t textureCount;
    WiiTexture** tpagTextures;
    uint32_t* tpagTextureSizes;
    uint32_t* tpagTextureLastUsed;
    uint32_t tpagTextureCount;
    uint32_t tpagTextureBytes;
    uint32_t textureFrame;
    uint32_t loadIndicatorFrames;
    WiiTexture* whiteTexture;

    // Alpha test / fog state (same flags, drive GX calls on change)
    bool alphaTestEnable;
    float alphaTestRef;
    bool fogEnable;
    uint32_t fogColor;

    // Frame sizing
    int32_t windowW, windowH;
    int32_t gameW, gameH;
    float canvasScale;
    int32_t canvasX, canvasY;
    int32_t canvasW, canvasH;
    float cullX0, cullY0;
    float cullX1, cullY1;
    bool cullEnabled;

    // Surfaces (raw GX has no FBO support)
    GXTexObj* surfaceTextures;
    void** surfaceBuffers;   // MEM2 backing store for each surface
    int32_t* surfaceWidth;
    int32_t* surfaceHeight;
    uint32_t surfaceCount;
    uint32_t surfaceCapacity;
    int32_t surfaceStack[16];

    // Original data.win counts
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    // fps
    u8 FPS;
} GRRRenderer;

static u8 GRRRenderer_CalculateFrameRate(void)
{
    static u8 frameCount = 0;
    static u32 lastTime;
    static u8 FPS = 0;
    const u32 currentTime = ticks_to_millisecs(gettime());

    frameCount++;
    if(currentTime - lastTime > 1000) {
        lastTime = currentTime;
        FPS = frameCount;
        frameCount = 0;
    }
    return FPS;
} 

Renderer* GRRRenderer_create(void);
