#include "grr_renderer.h"

#include "image_decoder.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "utils.h"
#include "wii_log.h"

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define fprintf(stream, ...) WiiLog_fprintf((stream), __VA_ARGS__)

#define GX_MAX_TEXTURE_SIZE 1024
#define GX_SPRITE_TEXTURE_FORMAT GX_TF_RGB5A3
// Keep persistent texture caches conservative for real Wii hardware. Dolphin is
// forgiving here, but the console still has to share MEM1/MEM2 with data.win,
// the VM, audio buffers, libogc, and transient image decode buffers.
#define GX_CROPPED_TEXTURE_BUDGET (8u * 1024u * 1024u)
#define GX_DECODED_TEXTURE_BUDGET (12u * 1024u * 1024u)
#define GX_FULL_TEXTURE_BUDGET (12u * 1024u * 1024u)
#define GX_CROPPED_TEXTURE_MAX_UNUSED_FRAMES 180u
#define GX_FULL_TEXTURE_MAX_UNUSED_FRAMES 180u
#define GX_DECODED_TEXTURE_MAX_UNUSED_FRAMES 90u
#define WII_GX_FIFO_SIZE (256u * 1024u)
#define WII_GX_DEBUG_OVERLAY 1
#define WII_GX_ENABLE_SURFACE_CAPTURE 0

GXRModeObj* g_wiiRmode = NULL;
void* g_wiiXfb[2] = { NULL, NULL };
u32 g_wiiFb = 0;
static void* g_wiiGpFifo = NULL;
static char g_wiiDebugOverlayLine1[96] = { 0 };
static char g_wiiDebugOverlayLine2[96] = { 0 };
static uint32_t g_wiiFrameQuads = 0;
static uint32_t g_wiiFrameSpriteDraws = 0;
static uint32_t g_wiiFrameTextureLoads = 0;
static uint32_t g_wiiLastFrameQuads = 0;
static uint32_t g_wiiLastFrameSpriteDraws = 0;
static uint32_t g_wiiLastFrameTextureLoads = 0;
static uint32_t g_wiiSurfaceTargetLogCount = 0;
static uint32_t g_wiiDownsampleLogCount = 0;

typedef struct {
    Font* font;
    WiiTexture* tex;
    TexturePageItem* fontTpag;  // for regular fonts
    Sprite* spriteFontSprite;   // for sprite fonts, NULL otherwise
} GrrFontState;

static WiiTexture* WiiTexture_create(u32 w, u32 h)
{
    WiiTexture* tex = safeCalloc(1, sizeof(WiiTexture));
    tex->w = w;
    tex->h = h;
    return tex;
}

static void WiiTexture_free(WiiTexture* tex)
{
    if (!tex) return;
    free(tex->data);
    free(tex);
}

void WiiGX_initVideo(void)
{
    VIDEO_Init();
    g_wiiRmode = VIDEO_GetPreferredMode(NULL);
    g_wiiFb = 0;

    g_wiiXfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_wiiRmode));
    g_wiiXfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_wiiRmode));

    console_init(g_wiiXfb[g_wiiFb], 20, 20, g_wiiRmode->fbWidth, g_wiiRmode->xfbHeight, g_wiiRmode->fbWidth * VI_DISPLAY_PIX_SZ);

    VIDEO_Configure(g_wiiRmode);
    VIDEO_SetNextFramebuffer(g_wiiXfb[g_wiiFb]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_wiiRmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    g_wiiFb ^= 1;

    g_wiiGpFifo = memalign(32, WII_GX_FIFO_SIZE);
    memset(g_wiiGpFifo, 0, WII_GX_FIFO_SIZE);
    GX_Init(g_wiiGpFifo, WII_GX_FIFO_SIZE);
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
    GX_InvalidateTexAll();
}

void WiiGX_shutdownVideo(void)
{
    GX_DrawDone();
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    free(g_wiiGpFifo);
    g_wiiGpFifo = NULL;
}

static void grrConfigureGXState(void)
{
    GX_SetViewport(0.0f, 0.0f, (f32)g_wiiRmode->fbWidth, (f32)g_wiiRmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (u32)g_wiiRmode->fbWidth, (u32)g_wiiRmode->efbHeight);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);

    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XY,   GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0,
                   GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordScaleManually(GX_TEXCOORD0, GX_FALSE, 0, 0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetNumTevStages(1);

    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
    f32 yscale = GX_GetYScaleFactor((u16)g_wiiRmode->efbHeight, (u16)g_wiiRmode->xfbHeight);
    u32 xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, (u16)g_wiiRmode->fbWidth, (u16)g_wiiRmode->efbHeight);
    GX_SetDispCopyDst((u16)g_wiiRmode->fbWidth, (u16)xfbHeight);
    GX_SetCopyFilter(g_wiiRmode->aa, g_wiiRmode->sample_pattern, GX_TRUE, g_wiiRmode->vfilter);
    GX_SetFieldMode(g_wiiRmode->field_rendering, (g_wiiRmode->viHeight == 2 * g_wiiRmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetCurrentMtx(GX_PNMTX0);
}

// ===================================================================
// Matrix helpers
// ===================================================================

static void Matrix4f_to_Mtx44(const Matrix4f* src, Mtx44 dst)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            dst[i][j] = src->m[i*4 + j];
}

static void Mtx44_to_Matrix4f(const Mtx44 src, Matrix4f* dst)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            dst->m[i*4 + j] = src[i][j];
}

static int32_t grrRoundToInt(float value)
{
    return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static uint8_t grrAlphaToU8(float alpha)
{
    if (!(alpha > 0.0f)) return 0;
    if (alpha >= 1.0f) return 255;
    return (uint8_t)(alpha * 255.0f + 0.5f);
}

static void grrConstrainTextureDimensions(int32_t srcW, int32_t srcH, int32_t* outW, int32_t* outH)
{
    if (srcW <= GX_MAX_TEXTURE_SIZE && srcH <= GX_MAX_TEXTURE_SIZE) {
        *outW = srcW;
        *outH = srcH;
        return;
    }

    float scaleX = (float)srcW / (float)GX_MAX_TEXTURE_SIZE;
    float scaleY = (float)srcH / (float)GX_MAX_TEXTURE_SIZE;
    float scale = scaleX > scaleY ? scaleX : scaleY;
    int32_t dstW = (int32_t)ceilf((float)srcW / scale);
    int32_t dstH = (int32_t)ceilf((float)srcH / scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;
    if (dstW > GX_MAX_TEXTURE_SIZE) dstW = GX_MAX_TEXTURE_SIZE;
    if (dstH > GX_MAX_TEXTURE_SIZE) dstH = GX_MAX_TEXTURE_SIZE;
    *outW = dstW;
    *outH = dstH;
}

static uint8_t* grrDownsampleRgbaNearest(const uint8_t* src, int32_t srcW, int32_t srcH, int32_t dstW, int32_t dstH)
{
    uint8_t* dst = safeMalloc((size_t)dstW * (size_t)dstH * 4u);
    for (int32_t y = 0; y < dstH; y++) {
        int32_t srcY = (int32_t)(((int64_t)y * srcH) / dstH);
        if (srcY >= srcH) srcY = srcH - 1;
        for (int32_t x = 0; x < dstW; x++) {
            int32_t srcX = (int32_t)(((int64_t)x * srcW) / dstW);
            if (srcX >= srcW) srcX = srcW - 1;
            memcpy(dst + ((size_t)y * (size_t)dstW + (size_t)x) * 4u,
                   src + ((size_t)srcY * (size_t)srcW + (size_t)srcX) * 4u,
                   4u);
        }
    }
    return dst;
}

static void grrUpdateCanvas(GRRRenderer* grr)
{
    if (grr->gameW <= 0 || grr->gameH <= 0 || grr->windowW <= 0 || grr->windowH <= 0) {
        grr->canvasScale = 1.0f;
        grr->canvasX = 0;
        grr->canvasY = 0;
        grr->canvasW = grr->windowW > 0 ? grr->windowW : 1;
        grr->canvasH = grr->windowH > 0 ? grr->windowH : 1;
        return;
    }

    float scaleX = (float)grr->windowW / (float)grr->gameW;
    float scaleY = (float)grr->windowH / (float)grr->gameH;
    grr->canvasScale = scaleX < scaleY ? scaleX : scaleY;
    if (grr->canvasScale <= 0.0f) grr->canvasScale = 1.0f;

    grr->canvasW = grrRoundToInt((float)grr->gameW * grr->canvasScale);
    grr->canvasH = grrRoundToInt((float)grr->gameH * grr->canvasScale);
    if (grr->canvasW <= 0) grr->canvasW = 1;
    if (grr->canvasH <= 0) grr->canvasH = 1;

    grr->canvasX = (grr->windowW - grr->canvasW) / 2;
    grr->canvasY = (grr->windowH - grr->canvasH) / 2;
}

static void grrMapPortToScreen(
    GRRRenderer* grr,
    int32_t portX, int32_t portY, int32_t portW, int32_t portH,
    int32_t* outX, int32_t* outY, int32_t* outW, int32_t* outH)
{
    float scale = grr->canvasScale > 0.0f ? grr->canvasScale : 1.0f;
    int32_t x = grr->canvasX + grrRoundToInt((float)portX * scale);
    int32_t y = grr->canvasY + grrRoundToInt((float)portY * scale);
    int32_t w = grrRoundToInt((float)portW * scale);
    int32_t h = grrRoundToInt((float)portH * scale);

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= grr->windowW) x = grr->windowW > 0 ? grr->windowW - 1 : 0;
    if (y >= grr->windowH) y = grr->windowH > 0 ? grr->windowH - 1 : 0;
    if (x + w > grr->windowW) w = grr->windowW - x;
    if (y + h > grr->windowH) h = grr->windowH - y;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    *outX = x;
    *outY = y;
    *outW = w;
    *outH = h;
}

static bool grrQuadOutsideCull(GRRRenderer* grr, float x0, float y0, float x1, float y1,
    float x2, float y2, float x3, float y3)
{
    if (!grr->cullEnabled) return false;

    float minX = x0, maxX = x0;
    float minY = y0, maxY = y0;
    float xs[3] = { x1, x2, x3 };
    float ys[3] = { y1, y2, y3 };
    for (int i = 0; i < 3; i++) {
        if (xs[i] < minX) minX = xs[i];
        if (xs[i] > maxX) maxX = xs[i];
        if (ys[i] < minY) minY = ys[i];
        if (ys[i] > maxY) maxY = ys[i];
    }

    return maxX < grr->cullX0 || minX > grr->cullX1 ||
           maxY < grr->cullY0 || minY > grr->cullY1;
}

// ===================================================================
// Texture swizzle: linear RGBA -> GX texture formats.
//
// GX RGBA8 layout: 4×4 texel blocks, 64 bytes each.
// First 32 bytes = AR plane (A then R for each texel).
// Next  32 bytes = GB plane (G then B for each texel).
// ===================================================================
static void convertRGBAtoGX_RGBA8(const uint8_t* src, void* dst, uint32_t width, uint32_t height)
{
    uint8_t* dest = (uint8_t*)dst;
    uint32_t bw = (width  + 3) & ~3u;
    uint32_t bh = (height + 3) & ~3u;

    for (uint32_t y = 0; y < bh; y += 4) {
        for (uint32_t x = 0; x < bw; x += 4) {
            uint32_t blockBase = ((y >> 2) * (bw >> 2) + (x >> 2)) * 64;
            for (uint32_t by = 0; by < 4; by++) {
                for (uint32_t bx = 0; bx < 4; bx++) {
                    uint32_t srcX = x + bx;
                    uint32_t srcY = y + by;
                    uint32_t texel = (by * 4 + bx) * 2;

                    uint8_t r = 0, g = 0, b = 0, a = 0;
                    if (srcX < width && srcY < height) {
                        uint32_t si = (srcY * width + srcX) * 4;
                        r = src[si]; g = src[si+1]; b = src[si+2]; a = src[si+3];
                    }
                    dest[blockBase + texel +  0] = a;  // AR plane: A
                    dest[blockBase + texel +  1] = r;  // AR plane: R
                    dest[blockBase + texel + 32] = g;  // GB plane: G
                    dest[blockBase + texel + 33] = b;  // GB plane: B
                }
            }
        }
    }
}

static void convertGX_RGBA8toRGBA(const void* src, uint8_t* dst, uint32_t width, uint32_t height)
{
    const uint8_t* source = (const uint8_t*)src;
    uint32_t bw = (width  + 3) & ~3u;
    uint32_t bh = (height + 3) & ~3u;

    for (uint32_t y = 0; y < bh; y += 4) {
        for (uint32_t x = 0; x < bw; x += 4) {
            uint32_t blockBase = ((y >> 2) * (bw >> 2) + (x >> 2)) * 64;
            for (uint32_t by = 0; by < 4; by++) {
                for (uint32_t bx = 0; bx < 4; bx++) {
                    uint32_t dstX = x + bx;
                    uint32_t dstY = y + by;
                    if (dstX >= width || dstY >= height) continue;
                    uint32_t texel = (by * 4 + bx) * 2;
                    uint32_t di = (dstY * width + dstX) * 4;
                    dst[di + 3] = source[blockBase + texel + 0];
                    dst[di + 0] = source[blockBase + texel + 1];
                    dst[di + 1] = source[blockBase + texel + 32];
                    dst[di + 2] = source[blockBase + texel + 33];
                }
            }
        }
    }
}

static uint16_t packRGB5A3(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a >= 224) {
        return 0x8000
            | ((uint16_t)(r >> 3) << 10)
            | ((uint16_t)(g >> 3) << 5)
            |  (uint16_t)(b >> 3);
    }

    return ((uint16_t)(a >> 5) << 12)
        | ((uint16_t)(r >> 4) << 8)
        | ((uint16_t)(g >> 4) << 4)
        |  (uint16_t)(b >> 4);
}

static void convertRGBAtoGX_RGB5A3(const uint8_t* src, void* dst, uint32_t width, uint32_t height)
{
    uint8_t* dest = (uint8_t*)dst;
    uint32_t bw = (width + 3) & ~3u;
    uint32_t bh = (height + 3) & ~3u;

    for (uint32_t y = 0; y < bh; y += 4) {
        for (uint32_t x = 0; x < bw; x += 4) {
            uint32_t blockBase = ((y >> 2) * (bw >> 2) + (x >> 2)) * 32;
            for (uint32_t by = 0; by < 4; by++) {
                for (uint32_t bx = 0; bx < 4; bx++) {
                    uint32_t srcX = x + bx;
                    uint32_t srcY = y + by;
                    uint8_t r = 0, g = 0, b = 0, a = 0;
                    if (srcX < width && srcY < height) {
                        uint32_t si = (srcY * width + srcX) * 4;
                        r = src[si];
                        g = src[si + 1];
                        b = src[si + 2];
                        a = src[si + 3];
                    }

                    uint16_t packed = packRGB5A3(r, g, b, a);
                    uint32_t di = blockBase + (by * 4 + bx) * 2;
                    dest[di] = (uint8_t)(packed >> 8);
                    dest[di + 1] = (uint8_t)(packed & 0xff);
                }
            }
        }
    }
}

static uint32_t grrSpriteTextureSize(int32_t w, int32_t h)
{
    return GX_GetTexBufferSize((u16)w, (u16)h, GX_SPRITE_TEXTURE_FORMAT, GX_FALSE, 0);
}

static bool rgbaIsGrayscale(const uint8_t* pixels, int32_t w, int32_t h)
{
    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; i++) {
        const uint8_t* p = pixels + i * 4;
        if (p[3] == 0) continue;
        int rg = abs((int)p[0] - (int)p[1]);
        int rb = abs((int)p[0] - (int)p[2]);
        int gb = abs((int)p[1] - (int)p[2]);
        if (rg > 3 || rb > 3 || gb > 3) return false;
    }
    return true;
}

static bool rgbaRegionIsGrayscale(const uint8_t* pixels, int32_t pageW, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH)
{
    for (int32_t y = 0; y < srcH; y++) {
        for (int32_t x = 0; x < srcW; x++) {
            const uint8_t* p = pixels + ((size_t)(srcY + y) * (size_t)pageW + (size_t)(srcX + x)) * 4;
            if (p[3] == 0) continue;
            int rg = abs((int)p[0] - (int)p[1]);
            int rb = abs((int)p[0] - (int)p[2]);
            int gb = abs((int)p[1] - (int)p[2]);
            if (rg > 3 || rb > 3 || gb > 3) return false;
        }
    }
    return true;
}

static void convertRGBAtoGX_IA4(const uint8_t* src, void* dst, uint32_t width, uint32_t height)
{
    uint8_t* dest = (uint8_t*)dst;
    uint32_t bw = (width + 7) & ~7u;
    uint32_t bh = (height + 3) & ~3u;

    for (uint32_t y = 0; y < bh; y += 4) {
        for (uint32_t x = 0; x < bw; x += 8) {
            uint32_t blockBase = ((y >> 2) * (bw >> 3) + (x >> 3)) * 32;
            for (uint32_t by = 0; by < 4; by++) {
                for (uint32_t bx = 0; bx < 8; bx++) {
                    uint8_t intensity = 0;
                    uint8_t alpha = 0;
                    uint32_t srcX = x + bx;
                    uint32_t srcY = y + by;
                    if (srcX < width && srcY < height) {
                        uint32_t si = (srcY * width + srcX) * 4;
                        intensity = (uint8_t)(((uint32_t)src[si] + src[si + 1] + src[si + 2]) / 3u);
                        alpha = src[si + 3];
                    }
                    dest[blockBase + by * 8 + bx] = (uint8_t)((intensity & 0xf0) | (alpha >> 4));
                }
            }
        }
    }
}

static uint32_t grrTextureSizeForFormat(int32_t w, int32_t h, u8 format)
{
    return GX_GetTexBufferSize((u16)w, (u16)h, format, GX_FALSE, 0);
}

static u8 grrChooseTextureFormat(const uint8_t* pixels, int32_t w, int32_t h)
{
    return rgbaIsGrayscale(pixels, w, h) ? GX_TF_IA4 : GX_SPRITE_TEXTURE_FORMAT;
}

// ===================================================================
// Internal: load a GXTexObj from a WiiTexture into slot 0
// ===================================================================
static void loadTex(WiiTexture* tex)
{
    GX_SetNumTexGens(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXTexObj obj;
    GX_InitTexObj(&obj, tex->data, tex->w, tex->h, tex->format,
                  GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&obj, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&obj, GX_TEXMAP0);
}

static void loadTexObj(GXTexObj* obj)
{
    GX_SetNumTexGens(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_LoadTexObj(obj, GX_TEXMAP0);
}

static void useColorOnlyTev(void)
{
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

// ===================================================================
// Internal: emit a textured quad using raw GX
// Vertex order: TL, TR, BR, BL  (clockwise, Y-down)
// ===================================================================
static void emitQuad(
    float x0, float y0, float x1, float y1,
    float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    g_wiiFrameQuads++;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
        GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
        GX_Position2f32(x2, y2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
        GX_Position2f32(x3, y3); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
    GX_End();
}

// Gradient-colored quad (different color per vertex)
static void emitQuadGrad(
    float x0, float y0, float x1, float y1,
    float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r0, uint8_t g0, uint8_t b0,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    uint8_t r3, uint8_t g3, uint8_t b3,
    uint8_t a)
{
    g_wiiFrameQuads++;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r0, g0, b0, a); GX_TexCoord2f32(u0, v0);
        GX_Position2f32(x1, y1); GX_Color4u8(r1, g1, b1, a); GX_TexCoord2f32(u1, v0);
        GX_Position2f32(x2, y2); GX_Color4u8(r2, g2, b2, a); GX_TexCoord2f32(u1, v1);
        GX_Position2f32(x3, y3); GX_Color4u8(r3, g3, b3, a); GX_TexCoord2f32(u0, v1);
    GX_End();
}

static void emitLoadingRect(float x0, float y0, float x1, float y1, GXColor color)
{
    g_wiiFrameQuads++;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(color.r, color.g, color.b, color.a); GX_TexCoord2f32(0.5f, 0.5f);
        GX_Position2f32(x1, y0); GX_Color4u8(color.r, color.g, color.b, color.a); GX_TexCoord2f32(0.5f, 0.5f);
        GX_Position2f32(x1, y1); GX_Color4u8(color.r, color.g, color.b, color.a); GX_TexCoord2f32(0.5f, 0.5f);
        GX_Position2f32(x0, y1); GX_Color4u8(color.r, color.g, color.b, color.a); GX_TexCoord2f32(0.5f, 0.5f);
    GX_End();
}

static uint8_t loadingGlyphRow(char c, int row)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14},
    };
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    };

    if (row < 0 || row >= 7) return 0;
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    switch (c) {
        case ' ': return 0;
        case '_': return row == 6 ? 31 : 0;
        case '-': return row == 3 ? 31 : 0;
        case '.': return row == 6 ? 4 : 0;
        case ':': return (row == 2 || row == 4) ? 4 : 0;
        case '/': return (uint8_t)(1 << (4 - row * 5 / 7));
        case '(': return row == 0 || row == 6 ? 2 : 4;
        case ')': return row == 0 || row == 6 ? 8 : 4;
        default: return row == 6 ? 31 : 0;
    }
}

static void drawLoadingText(float x, float y, const char* text, int scale, GXColor color)
{
    if (!text || scale <= 0) return;
    float cx = x;
    for (const char* p = text; *p && cx < (float)g_wiiRmode->fbWidth - 8.0f; p++) {
        char c = *p;
        for (int row = 0; row < 7; row++) {
            uint8_t bits = loadingGlyphRow(c, row);
            for (int col = 0; col < 5; col++) {
                if ((bits & (1u << (4 - col))) == 0) continue;
                float px = cx + (float)(col * scale);
                float py = y + (float)(row * scale);
                emitLoadingRect(px, py, px + (float)scale, py + (float)scale, color);
            }
        }
        cx += (float)(6 * scale);
    }
}

void WiiGX_drawLoadingStatus(int phase, int totalPhases, const char* status)
{
    if (totalPhases <= 0) totalPhases = 1;
    if (phase < 0) phase = 0;
    if (phase > totalPhases) phase = totalPhases;

    grrConfigureGXState();
    useColorOnlyTev();

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)g_wiiRmode->efbHeight, 0.0f, (f32)g_wiiRmode->fbWidth, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    float sw = (float)g_wiiRmode->fbWidth;
    float sh = (float)g_wiiRmode->efbHeight;
    int barW = (int)sw * 2 / 3;
    int barH = 24;
    int barX = ((int)sw - barW) / 2;
    int barY = (int)sh / 2 - barH / 2;
    int filled = (barW * phase) / totalPhases;

    emitLoadingRect(0, 0, sw, sh, (GXColor){17, 17, 17, 255});
    if (status && status[0]) {
        int textScale = 2;
        int maxChars = barW / (6 * textScale);
        int textChars = 0;
        while (status[textChars] && textChars < maxChars) textChars++;
        float textW = (float)(textChars * 6 * textScale);
        drawLoadingText((sw - textW) * 0.5f, (float)(barY - 34), status, textScale, (GXColor){220, 220, 220, 255});
    }
    emitLoadingRect(barX - 2, barY - 2, barX + barW + 2, barY + barH + 2, (GXColor){102, 102, 102, 255});
    emitLoadingRect(barX, barY, barX + barW, barY + barH, (GXColor){17, 17, 17, 255});
    if (filled > 0) {
        emitLoadingRect(barX, barY, barX + filled, barY + barH, (GXColor){173, 68, 204, 255});
    }

    GX_DrawDone();
    GX_CopyDisp(g_wiiXfb[g_wiiFb], GX_TRUE);
    VIDEO_SetNextFramebuffer(g_wiiXfb[g_wiiFb]);
    VIDEO_Flush();
    g_wiiFb ^= 1;
}

void WiiGX_drawLoadingScreen(int phase, int totalPhases)
{
    WiiGX_drawLoadingStatus(phase, totalPhases, NULL);
}

void WiiGX_drawTextScreen(const char* title, const char* const* lines, int lineCount)
{
    grrConfigureGXState();
    useColorOnlyTev();

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)g_wiiRmode->efbHeight, 0.0f, (f32)g_wiiRmode->fbWidth, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    float sw = (float)g_wiiRmode->fbWidth;
    float sh = (float)g_wiiRmode->efbHeight;
    emitLoadingRect(0, 0, sw, sh, (GXColor){17, 17, 17, 255});

    float y = 24.0f;
    if (title && title[0]) {
        drawLoadingText(20.0f, y, title, 2, (GXColor){235, 235, 235, 255});
        y += 28.0f;
    }

    if (lineCount < 0) lineCount = 0;
    for (int i = 0; i < lineCount && i < 18; i++) {
        drawLoadingText(20.0f, y, lines[i], 2, (GXColor){185, 220, 255, 255});
        y += 16.0f;
        if (y > sh - 18.0f) break;
    }

    GX_DrawDone();
    GX_CopyDisp(g_wiiXfb[g_wiiFb], GX_TRUE);
    VIDEO_SetNextFramebuffer(g_wiiXfb[g_wiiFb]);
    VIDEO_Flush();
    g_wiiFb ^= 1;
    VIDEO_WaitVSync();
}

void WiiGX_setDebugOverlay(const char* line1, const char* line2)
{
    snprintf(g_wiiDebugOverlayLine1, sizeof(g_wiiDebugOverlayLine1), "%s", line1 ? line1 : "");
    snprintf(g_wiiDebugOverlayLine2, sizeof(g_wiiDebugOverlayLine2), "%s", line2 ? line2 : "");
}

void WiiGX_getFrameStats(uint32_t* outQuads, uint32_t* outSpriteDraws, uint32_t* outTextureLoads)
{
    if (outQuads) *outQuads = g_wiiLastFrameQuads;
    if (outSpriteDraws) *outSpriteDraws = g_wiiLastFrameSpriteDraws;
    if (outTextureLoads) *outTextureLoads = g_wiiLastFrameTextureLoads;
}

// ===================================================================
// Texture management
// ===================================================================

static void grrEvictDecodedTexture(GRRRenderer* grr, uint32_t pageId)
{
    if (pageId >= grr->textureCount || grr->decodedTexturePixels[pageId] == NULL) return;
    free(grr->decodedTexturePixels[pageId]);
    grr->decodedTexturePixels[pageId] = NULL;
    if (grr->decodedTextureBytes >= grr->decodedTextureSizes[pageId]) {
        grr->decodedTextureBytes -= grr->decodedTextureSizes[pageId];
    } else {
        grr->decodedTextureBytes = 0;
    }
    grr->decodedTextureSizes[pageId] = 0;
    grr->decodedTextureLastUsed[pageId] = 0;
}

static void grrEvictDecodedTexturesUntilBudget(GRRRenderer* grr, uint32_t neededBytes, bool protectCurrentFrame)
{
    while (grr->decodedTextureBytes + neededBytes > GX_DECODED_TEXTURE_BUDGET) {
        uint32_t oldestIndex = UINT32_MAX;
        uint32_t oldestFrame = UINT32_MAX;
        for (uint32_t i = 0; i < grr->textureCount; i++) {
            if (grr->decodedTexturePixels[i] != NULL
                && (!protectCurrentFrame || grr->decodedTextureLastUsed[i] != grr->textureFrame)
                && grr->decodedTextureLastUsed[i] < oldestFrame) {
                oldestFrame = grr->decodedTextureLastUsed[i];
                oldestIndex = i;
            }
        }
        if (oldestIndex == UINT32_MAX) break;
        grrEvictDecodedTexture(grr, oldestIndex);
    }
}

static void grrTrimDecodedTextureCache(GRRRenderer* grr)
{
    grrEvictDecodedTexturesUntilBudget(grr, 0, true);
    uint32_t cutoff = grr->textureFrame > GX_DECODED_TEXTURE_MAX_UNUSED_FRAMES
        ? grr->textureFrame - GX_DECODED_TEXTURE_MAX_UNUSED_FRAMES
        : 0;
    if (cutoff == 0) return;
    for (uint32_t i = 0; i < grr->textureCount; i++) {
        if (grr->decodedTexturePixels[i] != NULL &&
            grr->decodedTextureLastUsed[i] != grr->textureFrame &&
            grr->decodedTextureLastUsed[i] < cutoff) {
            grrEvictDecodedTexture(grr, i);
        }
    }
}

static void grrEvictTexture(GRRRenderer* grr, uint32_t pageId)
{
    if (pageId >= grr->originalTexturePageCount || pageId >= grr->textureCount || grr->textures[pageId] == NULL) return;
    if (grr->textureLastUsed[pageId] == grr->textureFrame) GX_DrawDone();
    WiiTexture_free(grr->textures[pageId]);
    grr->textures[pageId] = NULL;
    if (grr->textureBytes >= grr->textureSizes[pageId]) {
        grr->textureBytes -= grr->textureSizes[pageId];
    } else {
        grr->textureBytes = 0;
    }
    grr->textureSizes[pageId] = 0;
    grr->textureLastUsed[pageId] = 0;
    grr->textureLoaded[pageId] = false;
    grr->loadIndicatorFrames = 18;
}

static void grrEvictTexturesUntilBudget(GRRRenderer* grr, uint32_t neededBytes, bool protectCurrentFrame)
{
    while (grr->textureBytes + neededBytes > GX_FULL_TEXTURE_BUDGET) {
        uint32_t oldestIndex = UINT32_MAX;
        uint32_t oldestFrame = UINT32_MAX;
        for (uint32_t i = 0; i < grr->originalTexturePageCount && i < grr->textureCount; i++) {
            if (grr->textures[i] != NULL
                && (!protectCurrentFrame || grr->textureLastUsed[i] != grr->textureFrame)
                && grr->textureLastUsed[i] < oldestFrame) {
                oldestFrame = grr->textureLastUsed[i];
                oldestIndex = i;
            }
        }
        if (oldestIndex == UINT32_MAX) break;
        grrEvictTexture(grr, oldestIndex);
    }
}

static void grrTrimTextureCache(GRRRenderer* grr)
{
    grrEvictTexturesUntilBudget(grr, 0, true);
    uint32_t cutoff = grr->textureFrame > GX_FULL_TEXTURE_MAX_UNUSED_FRAMES
        ? grr->textureFrame - GX_FULL_TEXTURE_MAX_UNUSED_FRAMES
        : 0;
    if (cutoff == 0) return;
    for (uint32_t i = 0; i < grr->originalTexturePageCount && i < grr->textureCount; i++) {
        if (grr->textures[i] != NULL &&
            grr->textureLastUsed[i] != grr->textureFrame &&
            grr->textureLastUsed[i] < cutoff) {
            grrEvictTexture(grr, i);
        }
    }
}

static bool grrEnsureTextureDecoded(GRRRenderer* grr, uint32_t pageId)
{
    if (grr->decodedTexturePixels[pageId] != NULL) {
        grr->decodedTextureLastUsed[pageId] = grr->textureFrame;
        return true;
    }

    DataWin* dw = grr->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];
    if (!DataWin_loadTextureBlob(dw, pageId)) {
        fprintf(stderr, "GRR: Failed to load TXTR page %u blob (offset=%u size=%u)\n",
                pageId, txtr->blobOffset, txtr->blobSize);
        return false;
    }

    int w, h;
    if (txtr->textureWidth > 0 && txtr->textureHeight > 0) {
        size_t expectedBytes = (size_t)txtr->textureWidth * (size_t)txtr->textureHeight * 4u;
        if (expectedBytes >= 8u * 1024u * 1024u) {
            grrEvictDecodedTexturesUntilBudget(grr, GX_DECODED_TEXTURE_BUDGET + 1u, false);
        }
    }
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h);
    if (!pixels) {
        const uint8_t* b = txtr->blobData;
        fprintf(stderr, "GRR: Failed to decode TXTR page %u (blob=%u bytes magic=%02X %02X %02X %02X meta=%dx%d)\n",
                pageId,
                txtr->blobSize,
                b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[3] : 0,
                txtr->textureWidth,
                txtr->textureHeight);
        DataWin_freeTextureBlob(dw, pageId);
        return false;
    }
    DataWin_freeTextureBlob(dw, pageId);

    uint32_t decodedBytes = (uint32_t)((size_t)w * (size_t)h * 4u);
    grrEvictDecodedTexturesUntilBudget(grr, decodedBytes, true);

    grr->decodedTexturePixels[pageId] = pixels;
    grr->textureWidths[pageId] = w;
    grr->textureHeights[pageId] = h;
    grr->decodedTextureSizes[pageId] = decodedBytes;
    grr->decodedTextureLastUsed[pageId] = grr->textureFrame;
    grr->decodedTextureBytes += decodedBytes;
    grr->loadIndicatorFrames = 18;
    fprintf(stderr, "GRR: Decoded TXTR page %u (%dx%d)\n", pageId, w, h);
    return true;
}

static WiiTexture* grrCreateTextureFromRgba(const uint8_t* pixels, int32_t w, int32_t h)
{
    int32_t uploadW, uploadH;
    grrConstrainTextureDimensions(w, h, &uploadW, &uploadH);

    const uint8_t* uploadPixels = pixels;
    uint8_t* downsampled = NULL;
    if (uploadW != w || uploadH != h) {
        downsampled = grrDownsampleRgbaNearest(pixels, w, h, uploadW, uploadH);
        uploadPixels = downsampled;
        if (g_wiiDownsampleLogCount < 32) {
            fprintf(stderr, "GRR: Downsampled texture upload %dx%d -> %dx%d\n", w, h, uploadW, uploadH);
            g_wiiDownsampleLogCount++;
        }
    }

    u8 format = grrChooseTextureFormat(uploadPixels, uploadW, uploadH);
    uint32_t texSize = grrTextureSizeForFormat(uploadW, uploadH, format);
    void* texData = memalign(32, texSize);
    if (!texData) {
        free(downsampled);
        return NULL;
    }

    if (format == GX_TF_IA4) {
        convertRGBAtoGX_IA4(uploadPixels, texData, (uint32_t)uploadW, (uint32_t)uploadH);
    } else if (format == GX_TF_RGBA8) {
        convertRGBAtoGX_RGBA8(uploadPixels, texData, (uint32_t)uploadW, (uint32_t)uploadH);
    } else {
        convertRGBAtoGX_RGB5A3(uploadPixels, texData, (uint32_t)uploadW, (uint32_t)uploadH);
    }
    free(downsampled);
    DCFlushRange(texData, texSize);
    GX_InvalidateTexAll();

    // We use WiiTexture purely as a container struct — no GX draw calls.
    WiiTexture* tex = WiiTexture_create((u32)uploadW, (u32)uploadH);
    if (!tex) { free(texData); return NULL; }

    free(tex->data);
    tex->data   = texData;
    tex->w      = (u32)uploadW;
    tex->h      = (u32)uploadH;
    tex->format = format;
    return tex;
}

static bool grrEnsureTextureLoaded(GRRRenderer* grr, uint32_t pageId)
{
    if (grr->textureLoaded[pageId]) {
        if (grr->textures[pageId] != NULL) {
            grr->textureLastUsed[pageId] = grr->textureFrame;
            return true;
        }
        if (grr->textureOversized[pageId]) return true;
        if (pageId >= grr->originalTexturePageCount) return false;
    }
    if (!grrEnsureTextureDecoded(grr, pageId)) return false;

    int32_t w = grr->textureWidths[pageId];
    int32_t h = grr->textureHeights[pageId];
    if (w > GX_MAX_TEXTURE_SIZE || h > GX_MAX_TEXTURE_SIZE) {
        grr->textureLoaded[pageId] = true;
        grr->textureOversized[pageId] = true;
        fprintf(stderr, "GRR: TXTR page %u is %dx%d, using cropped texture fallback\n", pageId, w, h);
        return true;
    }

    int32_t uploadW, uploadH;
    grrConstrainTextureDimensions(w, h, &uploadW, &uploadH);
    u8 format = grrChooseTextureFormat(grr->decodedTexturePixels[pageId], w, h);
    uint32_t texSize = grrTextureSizeForFormat(uploadW, uploadH, format);
    grrEvictTexturesUntilBudget(grr, texSize, true);
    WiiTexture* tex = grrCreateTextureFromRgba(grr->decodedTexturePixels[pageId], w, h);
    if (!tex) {
        grrEvictTexturesUntilBudget(grr, texSize, false);
        tex = grrCreateTextureFromRgba(grr->decodedTexturePixels[pageId], w, h);
    }
    if (!tex) return false;
    grr->textures[pageId] = tex;
    grr->textureSizes[pageId] = texSize;
    grr->textureLastUsed[pageId] = grr->textureFrame;
    grr->textureBytes += texSize;
    grr->textureLoaded[pageId] = true;
    grr->loadIndicatorFrames = 18;
    g_wiiFrameTextureLoads++;
    fprintf(stderr, "GRR: Loaded TXTR page %u (%dx%d, %u bytes, cache %u/%u)\n",
            pageId, w, h, texSize, grr->textureBytes, GX_FULL_TEXTURE_BUDGET);
    return true;
}

static WiiTexture* grrCreateCroppedTexture(GRRRenderer* grr, uint32_t pageId, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH)
{
    if (!grrEnsureTextureDecoded(grr, pageId)) return NULL;
    int32_t pageW = grr->textureWidths[pageId];
    int32_t pageH = grr->textureHeights[pageId];
    if (srcX < 0 || srcY < 0 || srcW <= 0 || srcH <= 0) return NULL;
    if (srcX + srcW > pageW || srcY + srcH > pageH) return NULL;

    uint8_t* cropped = safeMalloc((size_t)srcW * (size_t)srcH * 4);
    uint8_t* source = grr->decodedTexturePixels[pageId];
    for (int32_t row = 0; row < srcH; row++) {
        memcpy(cropped + (size_t)row * (size_t)srcW * 4,
               source + ((size_t)(srcY + row) * (size_t)pageW + (size_t)srcX) * 4,
               (size_t)srcW * 4);
    }

    WiiTexture* tex = grrCreateTextureFromRgba(cropped, srcW, srcH);
    free(cropped);
    return tex;
}

static void grrEvictCroppedTexture(GRRRenderer* grr, uint32_t index)
{
    if (index >= grr->tpagTextureCount || grr->tpagTextures[index] == NULL) return;
    if (grr->tpagTextureLastUsed[index] == grr->textureFrame) GX_DrawDone();
    WiiTexture_free(grr->tpagTextures[index]);
    grr->tpagTextures[index] = NULL;
    if (grr->tpagTextureBytes >= grr->tpagTextureSizes[index]) {
        grr->tpagTextureBytes -= grr->tpagTextureSizes[index];
    } else {
        grr->tpagTextureBytes = 0;
    }
    grr->tpagTextureSizes[index] = 0;
    grr->tpagTextureLastUsed[index] = 0;
}

static void grrEvictCroppedTexturesUntilBudget(GRRRenderer* grr, uint32_t neededBytes, bool protectCurrentFrame)
{
    while (grr->tpagTextureBytes + neededBytes > GX_CROPPED_TEXTURE_BUDGET) {
        uint32_t oldestIndex = UINT32_MAX;
        uint32_t oldestFrame = UINT32_MAX;
        for (uint32_t i = 0; i < grr->tpagTextureCount; i++) {
            if (grr->tpagTextures[i] != NULL
                && (!protectCurrentFrame || grr->tpagTextureLastUsed[i] != grr->textureFrame)
                && grr->tpagTextureLastUsed[i] < oldestFrame) {
                oldestFrame = grr->tpagTextureLastUsed[i];
                oldestIndex = i;
            }
        }
        if (oldestIndex == UINT32_MAX) break;
        grrEvictCroppedTexture(grr, oldestIndex);
    }
}

static void grrTrimCroppedTextureCache(GRRRenderer* grr)
{
    grrEvictCroppedTexturesUntilBudget(grr, 0, true);
    uint32_t cutoff = grr->textureFrame > GX_CROPPED_TEXTURE_MAX_UNUSED_FRAMES
        ? grr->textureFrame - GX_CROPPED_TEXTURE_MAX_UNUSED_FRAMES
        : 0;
    if (cutoff == 0) return;
    for (uint32_t i = 0; i < grr->tpagTextureCount; i++) {
        if (grr->tpagTextures[i] != NULL &&
            grr->tpagTextureLastUsed[i] != grr->textureFrame &&
            grr->tpagTextureLastUsed[i] < cutoff) {
            grrEvictCroppedTexture(grr, i);
        }
    }
}

static bool grrEnsureTpagTextureSlots(GRRRenderer* grr, uint32_t tpagIndex)
{
    if (tpagIndex < grr->tpagTextureCount) return true;

    uint32_t oldCount = grr->tpagTextureCount;
    uint32_t newCount = tpagIndex + 1;
    grr->tpagTextures = safeRealloc(grr->tpagTextures, newCount * sizeof(WiiTexture*));
    grr->tpagTextureSizes = safeRealloc(grr->tpagTextureSizes, newCount * sizeof(uint32_t));
    grr->tpagTextureLastUsed = safeRealloc(grr->tpagTextureLastUsed, newCount * sizeof(uint32_t));
    memset(grr->tpagTextures + oldCount, 0, (newCount - oldCount) * sizeof(WiiTexture*));
    memset(grr->tpagTextureSizes + oldCount, 0, (newCount - oldCount) * sizeof(uint32_t));
    memset(grr->tpagTextureLastUsed + oldCount, 0, (newCount - oldCount) * sizeof(uint32_t));
    grr->tpagTextureCount = newCount;
    return true;
}

static WiiTexture* grrResolveTpagTexture(GRRRenderer* grr, int32_t tpagIndex, TexturePageItem* tpag, float* outU0, float* outV0, float* outU1, float* outV1)
{
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= grr->textureCount) return NULL;
    if (!grrEnsureTextureLoaded(grr, (uint32_t)pageId)) return NULL;

    int32_t pageW = grr->textureWidths[pageId];
    int32_t pageH = grr->textureHeights[pageId];
    if (pageW <= GX_MAX_TEXTURE_SIZE && pageH <= GX_MAX_TEXTURE_SIZE) {
        WiiTexture* tex = grr->textures[pageId];
        if (!tex) return NULL;
        *outU0 = (float)tpag->sourceX / (float)pageW;
        *outV0 = (float)tpag->sourceY / (float)pageH;
        *outU1 = (float)(tpag->sourceX + tpag->sourceWidth) / (float)pageW;
        *outV1 = (float)(tpag->sourceY + tpag->sourceHeight) / (float)pageH;
        return tex;
    }

    if (!grrEnsureTpagTextureSlots(grr, (uint32_t)tpagIndex)) return NULL;
    if (grr->tpagTextures[tpagIndex]) {
        grr->tpagTextureLastUsed[tpagIndex] = grr->textureFrame;
        *outU0 = 0.0f;
        *outV0 = 0.0f;
        *outU1 = 1.0f;
        *outV1 = 1.0f;
        return grr->tpagTextures[tpagIndex];
    }
    if (grr->tpagTextures[tpagIndex]) grrEvictCroppedTexture(grr, (uint32_t)tpagIndex);

    if (!grrEnsureTextureDecoded(grr, (uint32_t)pageId)) return NULL;
    pageW = grr->textureWidths[pageId];
    pageH = grr->textureHeights[pageId];

    if (!grr->tpagTextures[tpagIndex]) {
        int32_t uploadW, uploadH;
        grrConstrainTextureDimensions(tpag->sourceWidth, tpag->sourceHeight, &uploadW, &uploadH);
        u8 neededFormat = rgbaRegionIsGrayscale(
            grr->decodedTexturePixels[pageId],
            pageW,
            tpag->sourceX,
            tpag->sourceY,
            tpag->sourceWidth,
            tpag->sourceHeight) ? GX_TF_IA4 : GX_SPRITE_TEXTURE_FORMAT;
        uint32_t neededBytes = grrTextureSizeForFormat(uploadW, uploadH, neededFormat);
        grrEvictCroppedTexturesUntilBudget(grr, neededBytes, true);
        grr->tpagTextures[tpagIndex] = grrCreateCroppedTexture(grr, (uint32_t)pageId, tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight);
        grrEvictDecodedTexture(grr, (uint32_t)pageId);
        if (!grr->tpagTextures[tpagIndex]) return NULL;
        grr->tpagTextureSizes[tpagIndex] = grrTextureSizeForFormat(
            (int32_t)grr->tpagTextures[tpagIndex]->w,
            (int32_t)grr->tpagTextures[tpagIndex]->h,
            grr->tpagTextures[tpagIndex]->format);
        grr->tpagTextureBytes += grr->tpagTextureSizes[tpagIndex];
    }
    grr->tpagTextureLastUsed[tpagIndex] = grr->textureFrame;
    grrEvictDecodedTexture(grr, (uint32_t)pageId);

    *outU0 = 0.0f;
    *outV0 = 0.0f;
    *outU1 = 1.0f;
    *outV1 = 1.0f;
    return grr->tpagTextures[tpagIndex];
}

static bool grrMarkIndex(bool* seen, uint32_t count, int32_t index)
{
    if (index < 0 || (uint32_t)index >= count) return false;
    if (seen[index]) return false;
    seen[index] = true;
    return true;
}

static void grrCollectRoomPreloadAssets(DataWin* dw, Room* room, bool* sprites, bool* backgrounds, uint32_t* total)
{
    repeat(room->gameObjectCount, i) {
        RoomGameObject* roomObj = &room->gameObjects[i];
        if (roomObj->objectDefinition >= 0 && (uint32_t)roomObj->objectDefinition < dw->objt.count) {
            if (grrMarkIndex(sprites, dw->sprt.count, dw->objt.objects[roomObj->objectDefinition].spriteId)) (*total)++;
        }
    }

    repeat(8, i) {
        RoomBackground* bg = &room->backgrounds[i];
        if (bg->enabled && grrMarkIndex(backgrounds, dw->bgnd.count, bg->backgroundDefinition)) (*total)++;
    }

    repeat(room->tileCount, i) {
        RoomTile* tile = &room->tiles[i];
        if (grrMarkIndex(backgrounds, dw->bgnd.count, tile->backgroundDefinition)) (*total)++;
    }

    repeat(room->layerCount, i) {
        RoomLayer* layer = &room->layers[i];
        if (layer->type == RoomLayerType_Assets && layer->assetsData != NULL) {
            RoomLayerAssetsData* assets = layer->assetsData;
            repeat(assets->spriteCount, j) {
                if (grrMarkIndex(sprites, dw->sprt.count, assets->sprites[j].spriteIndex)) (*total)++;
            }
            repeat(assets->legacyTileCount, j) {
                if (grrMarkIndex(backgrounds, dw->bgnd.count, assets->legacyTiles[j].backgroundDefinition)) (*total)++;
            }
        } else if (layer->type == RoomLayerType_Background && layer->backgroundData != NULL) {
            if (grrMarkIndex(sprites, dw->sprt.count, layer->backgroundData->spriteIndex)) (*total)++;
        } else if (layer->type == RoomLayerType_Tiles && layer->tilesData != NULL) {
            if (grrMarkIndex(backgrounds, dw->bgnd.count, layer->tilesData->backgroundIndex)) (*total)++;
        }
    }
}

static void grrPreloadTpag(GRRRenderer* grr, DataWin* dw, int32_t tpagIndex)
{
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float u0, v0, u1, v1;
    grrResolveTpagTexture(grr, tpagIndex, tpag, &u0, &v0, &u1, &v1);
}

static void grrPreloadSpriteFrames(GRRRenderer* grr, DataWin* dw, int32_t spriteIndex)
{
    if (spriteIndex < 0 || (uint32_t)spriteIndex >= dw->sprt.count) return;
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    repeat(sprite->textureCount, i) {
        grrPreloadTpag(grr, dw, sprite->tpagIndices[i]);
    }
}

static void grrPreloadBackgroundTexture(GRRRenderer* grr, DataWin* dw, int32_t backgroundIndex)
{
    if (backgroundIndex < 0 || (uint32_t)backgroundIndex >= dw->bgnd.count) return;
    grrPreloadTpag(grr, dw, dw->bgnd.backgrounds[backgroundIndex].tpagIndex);
}

static void grrPreloadFontTexture(GRRRenderer* grr, DataWin* dw, int32_t fontIndex)
{
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;
    Font* font = &dw->font.fonts[fontIndex];
    if (font->isSpriteFont) {
        grrPreloadSpriteFrames(grr, dw, font->spriteIndex);
    } else {
        if (font->tpagIndex < 0 || (uint32_t)font->tpagIndex >= dw->tpag.count) return;
        grrPreloadTpag(grr, dw, font->tpagIndex);
    }
}

static void grrMakeStatus(char* out, size_t outSize, const char* prefix, const char* name)
{
    if (!out || outSize == 0) return;
    if (!name || !name[0]) name = "(unnamed)";
    snprintf(out, outSize, "%s %s", prefix, name);
}

void GRRRenderer_preloadRoomAssets(Renderer* renderer, DataWin* dataWin, int32_t roomIndex)
{
    if (!renderer || !dataWin || roomIndex < 0 || (uint32_t)roomIndex >= dataWin->room.count) return;
    GRRRenderer* grr = (GRRRenderer*)renderer;
    Room* room = &dataWin->room.rooms[roomIndex];
    if (!room->payloadLoaded) DataWin_loadRoomPayload(dataWin, roomIndex);

    bool* sprites = safeCalloc(dataWin->sprt.count ? dataWin->sprt.count : 1, sizeof(bool));
    bool* backgrounds = safeCalloc(dataWin->bgnd.count ? dataWin->bgnd.count : 1, sizeof(bool));
    uint32_t total = 1;
    grrCollectRoomPreloadAssets(dataWin, room, sprites, backgrounds, &total);

    uint32_t step = 0;
    char status[128];
    grrMakeStatus(status, sizeof(status), "ROOM", room->name);
    WiiGX_drawLoadingStatus((int)step++, (int)total, status);

    repeat(dataWin->sprt.count, i) {
        if (!sprites[i]) continue;
        grrMakeStatus(status, sizeof(status), "SPRITE", dataWin->sprt.sprites[i].name);
        WiiGX_drawLoadingStatus((int)step, (int)total, status);
        grrPreloadSpriteFrames(grr, dataWin, (int32_t)i);
        step++;
    }

    repeat(dataWin->bgnd.count, i) {
        if (!backgrounds[i]) continue;
        grrMakeStatus(status, sizeof(status), "BACKGROUND", dataWin->bgnd.backgrounds[i].name);
        WiiGX_drawLoadingStatus((int)step, (int)total, status);
        grrPreloadBackgroundTexture(grr, dataWin, (int32_t)i);
        step++;
    }

    grrTrimTextureCache(grr);
    grrTrimCroppedTextureCache(grr);
    grrTrimDecodedTextureCache(grr);
    WiiGX_drawLoadingStatus((int)total, (int)total, "READY");
    for (int i = 0; i < 2; i++) VIDEO_WaitVSync();
    fprintf(stderr, "GRR: Preloaded room assets for %s (%u steps, %u full bytes, %u cropped bytes, %u decoded bytes)\n",
            room->name ? room->name : "(unnamed)", total, grr->textureBytes, grr->tpagTextureBytes, grr->decodedTextureBytes);
    free(sprites);
    free(backgrounds);
}

void GRRRenderer_preloadStartupAssets(Renderer* renderer, DataWin* dataWin)
{
    if (!renderer || !dataWin) return;
    GRRRenderer* grr = (GRRRenderer*)renderer;
    uint32_t total = 1 + dataWin->font.count;

    fprintf(stderr, "GRR: Startup preload is font-only; sprites/backgrounds stream on demand\n");
    uint32_t step = 0;
    WiiGX_drawLoadingStatus((int)step++, (int)total, "PRELOAD STARTUP");
    char status[128];
    repeat(dataWin->font.count, i) {
        grrMakeStatus(status, sizeof(status), "FONT", dataWin->font.fonts[i].name);
        WiiGX_drawLoadingStatus((int)step, (int)total, status);
        grrPreloadFontTexture(grr, dataWin, (int32_t)i);
        step++;
    }

    grrTrimCroppedTextureCache(grr);
    grrTrimDecodedTextureCache(grr);
    WiiGX_drawLoadingStatus((int)total, (int)total, "STARTUP READY");
    for (int i = 0; i < 6; i++) VIDEO_WaitVSync();
    fprintf(stderr, "GRR: Startup preload finished (%u steps, %u cropped bytes, %u decoded bytes)\n",
            total, grr->tpagTextureBytes, grr->decodedTextureBytes);
}

static uint32_t findOrAllocTexturePageSlot(GRRRenderer* grr)
{
    for (uint32_t i = grr->originalTexturePageCount; i < grr->textureCount; i++) {
        if (grr->textures[i] == NULL) return i;
    }
    uint32_t newPageId = grr->textureCount++;
    grr->textures = safeRealloc(grr->textures, grr->textureCount * sizeof(WiiTexture*));
    grr->decodedTexturePixels = safeRealloc(grr->decodedTexturePixels, grr->textureCount * sizeof(uint8_t*));
    grr->textureWidths = safeRealloc(grr->textureWidths, grr->textureCount * sizeof(int32_t));
    grr->textureHeights = safeRealloc(grr->textureHeights, grr->textureCount * sizeof(int32_t));
    grr->decodedTextureSizes = safeRealloc(grr->decodedTextureSizes, grr->textureCount * sizeof(uint32_t));
    grr->decodedTextureLastUsed = safeRealloc(grr->decodedTextureLastUsed, grr->textureCount * sizeof(uint32_t));
    grr->textureSizes = safeRealloc(grr->textureSizes, grr->textureCount * sizeof(uint32_t));
    grr->textureLastUsed = safeRealloc(grr->textureLastUsed, grr->textureCount * sizeof(uint32_t));
    grr->textureLoaded = safeRealloc(grr->textureLoaded, grr->textureCount * sizeof(bool));
    grr->textureOversized = safeRealloc(grr->textureOversized, grr->textureCount * sizeof(bool));
    grr->textures[newPageId] = NULL;
    grr->decodedTexturePixels[newPageId] = NULL;
    grr->textureWidths[newPageId] = 0;
    grr->textureHeights[newPageId] = 0;
    grr->decodedTextureSizes[newPageId] = 0;
    grr->decodedTextureLastUsed[newPageId] = 0;
    grr->textureSizes[newPageId] = 0;
    grr->textureLastUsed[newPageId] = 0;
    grr->textureLoaded[newPageId] = true;
    grr->textureOversized[newPageId] = false;
    return newPageId;
}

static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount)
{
    for (uint32_t i = originalTpagCount; i < dw->tpag.count; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

// ===================================================================
// grrDrawSprite
// Replaces GX_DrawImg with raw GX quads.
// Handles scale, rotation around origin point.
// ===================================================================
static void grrDrawSprite(
    Renderer* renderer,
    int32_t tpagIndex,
    float x, float y,
    float originX, float originY,
    float xscale, float yscale,
    float angleDeg,
    uint32_t color, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    // The four corners in local space (origin-relative)
    float lx0 = (float)tpag->targetX * xscale - originX * xscale;
    float ly0 = (float)tpag->targetY * yscale - originY * yscale;
    float lx1 = lx0 + (float)tpag->sourceWidth * xscale;
    float ly1 = ly0 + (float)tpag->sourceHeight * yscale;

    // Rotate and translate to world space
    float rad  = -angleDeg * ((float)M_PI / 180.0f);
    float cosA = cosf(rad);
    float sinA = sinf(rad);

#define ROTATE_POINT(lx, ly, wx, wy) \
    wx = cosA*(lx) - sinA*(ly) + x; \
    wy = sinA*(lx) + cosA*(ly) + y;

    float wx0, wy0, wx1, wy1, wx2, wy2, wx3, wy3;
    ROTATE_POINT(lx0, ly0, wx0, wy0);
    ROTATE_POINT(lx1, ly0, wx1, wy1);
    ROTATE_POINT(lx1, ly1, wx2, wy2);
    ROTATE_POINT(lx0, ly1, wx3, wy3);
#undef ROTATE_POINT

    if (grrQuadOutsideCull(grr, wx0, wy0, wx1, wy1, wx2, wy2, wx3, wy3)) return;

    float u0, v0, u1, v1;
    WiiTexture* tex = grrResolveTpagTexture(grr, tpagIndex, tpag, &u0, &v0, &u1, &v1);
    if (!tex) return;
    g_wiiFrameSpriteDraws++;

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    loadTex(tex);
    emitQuad(wx0, wy0, wx1, wy1, wx2, wy2, wx3, wy3,
             u0, v0, u1, v1, r, g, b, a);
}

// ===================================================================
// grrDrawSpritePart
// ===================================================================
static void grrDrawSpritePart(
    Renderer* renderer,
    int32_t tpagIndex,
    int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
    float x, float y,
    float xscale, float yscale,
    float angleDeg, float pivotX, float pivotY,
    uint32_t color, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    if (angleDeg == 0.0f) {
        cx0 = x;                         cy0 = y;
        cx1 = x + (float)srcW * xscale;  cy1 = y;
        cx2 = cx1;                        cy2 = y + (float)srcH * yscale;
        cx3 = x;                          cy3 = cy2;
    } else {
        float rad  = -angleDeg * ((float)M_PI / 180.0f);
        float cosA = cosf(rad), sinA = sinf(rad);
        float qx[4] = { x, x + (float)srcW*xscale, x + (float)srcW*xscale, x };
        float qy[4] = { y, y,                        y + (float)srcH*yscale,  y + (float)srcH*yscale };
        float* cxs[4] = { &cx0, &cx1, &cx2, &cx3 };
        float* cys[4] = { &cy0, &cy1, &cy2, &cy3 };
        for (int i = 0; i < 4; i++) {
            float dx = qx[i] - pivotX, dy = qy[i] - pivotY;
            *cxs[i] = cosA*dx - sinA*dy + pivotX;
            *cys[i] = sinA*dx + cosA*dy + pivotY;
        }
    }

    if (grrQuadOutsideCull(grr, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3)) return;

    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= grr->textureCount) return;
    if (!grrEnsureTextureLoaded(grr, (uint32_t)pageId)) return;
    WiiTexture* tex = grr->textures[pageId];

    float u0, v0, u1, v1;
    if (grr->textureWidths[pageId] > GX_MAX_TEXTURE_SIZE || grr->textureHeights[pageId] > GX_MAX_TEXTURE_SIZE) {
        tex = grrResolveTpagTexture(grr, tpagIndex, tpag, &u0, &v0, &u1, &v1);
        if (!tex) return;
        u0 = (float)srcOffX / (float)tpag->sourceWidth;
        v0 = (float)srcOffY / (float)tpag->sourceHeight;
        u1 = (float)(srcOffX + srcW) / (float)tpag->sourceWidth;
        v1 = (float)(srcOffY + srcH) / (float)tpag->sourceHeight;
    } else {
        float texW = (float)tex->w;
        float texH = (float)tex->h;
        u0 = (float)(tpag->sourceX + srcOffX) / texW;
        v0 = (float)(tpag->sourceY + srcOffY) / texH;
        u1 = (float)(tpag->sourceX + srcOffX + srcW) / texW;
        v1 = (float)(tpag->sourceY + srcOffY + srcH) / texH;
    }

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    loadTex(tex);
    g_wiiFrameSpriteDraws++;
    emitQuad(cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3,
             u0, v0, u1, v1, r, g, b, a);
}

// ===================================================================
// grrDrawSpritePos
// ===================================================================
static void grrDrawSpritePos(
    Renderer* renderer, int32_t tpagIndex,
    float x1, float y1, float x2, float y2,
    float x3, float y3, float x4, float y4, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    if (grrQuadOutsideCull(grr, x1, y1, x2, y2, x3, y3, x4, y4)) return;

    float u0, v0, u1, v1;
    WiiTexture* tex = grrResolveTpagTexture(grr, tpagIndex, tpag, &u0, &v0, &u1, &v1);
    if (!tex) return;
    g_wiiFrameSpriteDraws++;

    uint8_t a = grrAlphaToU8(alpha);

    loadTex(tex);
    emitQuad(x1, y1, x2, y2, x3, y3, x4, y4,
             u0, v0, u1, v1, 255, 255, 255, a);
}

// ===================================================================
// grrDrawRectangle  — pure raw GX, no GX
// ===================================================================
static void grrDrawLineColor(
    Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color1, uint32_t color2, float alpha);

static void grrDrawRectangle(
    Renderer* renderer,
    float x1, float y1, float x2, float y2,
    uint32_t color, float alpha, bool outline)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    useColorOnlyTev();

    if (!outline) {
        emitQuad(x1, y1, x2 + 1.0f, y1, x2 + 1.0f, y2 + 1.0f, x1, y2 + 1.0f,
                 0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
    } else {
        // Top
        emitQuad(x1, y1,   x2+1, y1,   x2+1, y1+1, x1, y1+1, 0.5f,0.5f,0.5f,0.5f, r,g,b,a);
        // Bottom
        emitQuad(x1, y2,   x2+1, y2,   x2+1, y2+1, x1, y2+1, 0.5f,0.5f,0.5f,0.5f, r,g,b,a);
        // Left
        emitQuad(x1,   y1+1, x1+1, y1+1, x1+1, y2, x1, y2,   0.5f,0.5f,0.5f,0.5f, r,g,b,a);
        // Right
        emitQuad(x2,   y1+1, x2+1, y1+1, x2+1, y2, x2, y2,   0.5f,0.5f,0.5f,0.5f, r,g,b,a);
    }
}

static void grrDrawRectangleColor(
    Renderer* renderer,
    float x1, float y1, float x2, float y2,
    uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
    float alpha, bool outline)
{
    if (outline) {
        grrDrawLineColor(renderer, x1, y1, x2, y1, 1.0f, color1, color2, alpha);
        grrDrawLineColor(renderer, x2, y1, x2, y2, 1.0f, color2, color3, alpha);
        grrDrawLineColor(renderer, x2, y2, x1, y2, 1.0f, color3, color4, alpha);
        grrDrawLineColor(renderer, x1, y2, x1, y1, 1.0f, color4, color1, alpha);
        return;
    }

    uint8_t a = grrAlphaToU8(alpha);
    useColorOnlyTev();
    emitQuadGrad(
        x1, y1, x2 + 1.0f, y1, x2 + 1.0f, y2 + 1.0f, x1, y2 + 1.0f,
        0.5f, 0.5f, 0.5f, 0.5f,
        BGR_R(color1), BGR_G(color1), BGR_B(color1),
        BGR_R(color2), BGR_G(color2), BGR_B(color2),
        BGR_R(color3), BGR_G(color3), BGR_B(color3),
        BGR_R(color4), BGR_G(color4), BGR_B(color4),
        a);
}

// ===================================================================
// grrDrawLine  — pure raw GX, no GX
// ===================================================================
static void grrDrawLine(
    Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    useColorOnlyTev();

    if (width <= 1.0f) {
        width = 1.0f;
    }

    {
        float dx = x2 - x1, dy = y2 - y1;
        float len = sqrtf(dx*dx + dy*dy);
        if (len == 0.0f) {
            emitQuad(x1, y1, x1 + width, y1, x1 + width, y1 + width, x1, y1 + width,
                     0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
            return;
        }
        float px = (-dy / len) * (width * 0.5f);
        float py = ( dx / len) * (width * 0.5f);
        emitQuad(x1+px, y1+py, x1-px, y1-py, x2-px, y2-py, x2+px, y2+py,
                 0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
    }
}

// ===================================================================
// grrDrawLineColor
// ===================================================================
static void grrDrawLineColor(
    Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color1, uint32_t color2, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.0001f) return;

    float px = (-dy / len) * (width * 0.5f);
    float py = ( dx / len) * (width * 0.5f);

    uint8_t r1 = BGR_R(color1), g1 = BGR_G(color1), b1 = BGR_B(color1);
    uint8_t r2 = BGR_R(color2), g2 = BGR_G(color2), b2 = BGR_B(color2);
    uint8_t a  = grrAlphaToU8(alpha);

    useColorOnlyTev();
    emitQuadGrad(
        x1+px, y1+py,  x1-px, y1-py,  x2-px, y2-py,  x2+px, y2+py,
        0.5f, 0.5f, 0.5f, 0.5f,
        r1, g1, b1,   r1, g1, b1,   r2, g2, b2,   r2, g2, b2,
        a);
}

// ===================================================================
// grrDrawTriangle
// ===================================================================
static void grrDrawTriangle(
    Renderer* renderer,
    float x1, float y1, float x2, float y2, float x3, float y3,
    bool outline)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    uint8_t r = BGR_R(renderer->drawColor), g = BGR_G(renderer->drawColor), b = BGR_B(renderer->drawColor);
    uint8_t a = grrAlphaToU8(renderer->drawAlpha);

    if (outline) {
        grrDrawLine(renderer, x1, y1, x2, y2, 1.0f, renderer->drawColor, renderer->drawAlpha);
        grrDrawLine(renderer, x2, y2, x3, y3, 1.0f, renderer->drawColor, renderer->drawAlpha);
        grrDrawLine(renderer, x3, y3, x1, y1, 1.0f, renderer->drawColor, renderer->drawAlpha);
    } else {
        useColorOnlyTev();
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
            GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.5f, 0.5f);
            GX_Position2f32(x2, y2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.5f, 0.5f);
            GX_Position2f32(x3, y3); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.5f, 0.5f);
        GX_End();
    }
}

// ===================================================================
// Font helpers
// ===================================================================

static bool grrResolveFontState(GRRRenderer* grr, DataWin* dw, Font* font, GrrFontState* out)
{
    out->font = font;
    out->spriteFontSprite = NULL;
    out->fontTpag = NULL;
    out->tex = NULL;

    if (font->isSpriteFont) {
        if (font->spriteIndex < 0 || (uint32_t)font->spriteIndex >= dw->sprt.count) return false;
        out->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
        return true;
    } else {
        if (font->tpagIndex < 0 || (uint32_t)font->tpagIndex >= dw->tpag.count) return false;
        TexturePageItem* tpag = &dw->tpag.items[font->tpagIndex];
        int16_t pageId = tpag->texturePageId;
        if (pageId < 0 || (uint32_t)pageId >= grr->textureCount) return false;
        if (!grrEnsureTextureLoaded(grr, (uint32_t)pageId)) return false;
        out->fontTpag = tpag;
        if (grr->textureOversized[pageId]) return true;
        out->tex = grr->textures[pageId];
        return out->tex != NULL;
    }
}

static bool grrResolveGlyph(
    GRRRenderer* grr, DataWin* dw, GrrFontState* state, FontGlyph* glyph,
    float cursorX, float cursorY,
    WiiTexture** outTex,
    float* u0, float* v0, float* u1, float* v1,
    float* localX0, float* localY0)
{
    Font* font = state->font;

    if (font->isSpriteFont && state->spriteFontSprite != NULL) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t)(glyph - font->glyphs);
        if (glyphIndex < 0 || glyphIndex >= (int32_t)sprite->textureCount) return false;
        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (tpagIdx < 0) return false;
        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        WiiTexture* tex = grrResolveTpagTexture(grr, tpagIdx, glyphTpag, u0, v0, u1, v1);
        if (!tex) return false;
        *outTex = tex;
        *localX0 = cursorX + (float)(int16_t)glyph->offset;
        *localY0 = cursorY + (float)((int32_t)glyphTpag->targetY - sprite->originY);
    } else {
        int16_t pageId = state->fontTpag->texturePageId;
        if (pageId < 0 || (uint32_t)pageId >= grr->textureCount) return false;
        if (grr->textureOversized[pageId]) {
            WiiTexture* tex = grrResolveTpagTexture(grr, state->font->tpagIndex, state->fontTpag, u0, v0, u1, v1);
            if (!tex) return false;
            *outTex = tex;
            float baseW = (float)state->fontTpag->sourceWidth;
            float baseH = (float)state->fontTpag->sourceHeight;
            if (baseW <= 0.0f || baseH <= 0.0f) return false;
            *u0 = (float)glyph->sourceX / baseW;
            *v0 = (float)glyph->sourceY / baseH;
            *u1 = (float)(glyph->sourceX + glyph->sourceWidth) / baseW;
            *v1 = (float)(glyph->sourceY + glyph->sourceHeight) / baseH;
        } else {
            if (!state->tex) return false;
            *outTex = state->tex;
            float pageW = (float)grr->textureWidths[pageId];
            float pageH = (float)grr->textureHeights[pageId];
            if (pageW <= 0.0f || pageH <= 0.0f) return false;
            *u0 = (float)(state->fontTpag->sourceX + glyph->sourceX) / pageW;
            *v0 = (float)(state->fontTpag->sourceY + glyph->sourceY) / pageH;
            *u1 = (float)(state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / pageW;
            *v1 = (float)(state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / pageH;
        }
        *localX0 = cursorX + (float)(int16_t)glyph->offset;
        *localY0 = cursorY;
    }
    return true;
}

// ===================================================================
// grrDrawText
// ===================================================================
static void grrDrawText(
    Renderer* renderer,
    const char* text, float x, float y,
    float xscale, float yscale, float angleDeg, float lineSeparation)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;
    Font* font = &dw->font.fonts[fontIndex];

    GrrFontState fontState;
    if (!grrResolveFontState(grr, dw, font, &fontState)) return;

    uint8_t r = BGR_R(renderer->drawColor), g = BGR_G(renderer->drawColor), b = BGR_B(renderer->drawColor);
    uint8_t a = grrAlphaToU8(renderer->drawAlpha);

    int32_t textLen = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation)
        ? TextUtils_lineStride(font)
        : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = lineLen > 0;
        if (hasCh) ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = pos < lineLen;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != NULL && glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                WiiTexture* glyphTex;
                float u0, v0, u1, v1, lx0, ly0;
                if (grrResolveGlyph(grr, dw, &fontState, glyph, cursorX, cursorY,
                                    &glyphTex, &u0, &v0, &u1, &v1, &lx0, &ly0)) {
                    float lx1 = lx0 + (float)glyph->sourceWidth;
                    float ly1 = ly0 + (float)glyph->sourceHeight;
                    float px0, py0, px1, py1, px2, py2, px3, py3;
                    Matrix4f_transformPoint(&transform, lx0, ly0, &px0, &py0);
                    Matrix4f_transformPoint(&transform, lx1, ly0, &px1, &py1);
                    Matrix4f_transformPoint(&transform, lx1, ly1, &px2, &py2);
                    Matrix4f_transformPoint(&transform, lx0, ly1, &px3, &py3);
                    loadTex(glyphTex);
                    emitQuad(px0, py0, px1, py1, px2, py2, px3, py3,
                             u0, v0, u1, v1, r, g, b, a);

                    if (hasNext)
                        cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }
            if (glyph) cursorX += glyph->shift;
            ch = nextCh; hasCh = hasNext;
        }

        cursorY += lineStride;
        lineStart = (lineEnd < textLen) ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }
}

// ===================================================================
// grrDrawTextColor
// ===================================================================
static void grrDrawTextColor(
    Renderer* renderer,
    const char* text, float x, float y,
    float xscale, float yscale, float angleDeg,
    int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha, float lineSeparation)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || (uint32_t)fontIndex >= dw->font.count) return;
    Font* font = &dw->font.fonts[fontIndex];

    GrrFontState fontState;
    if (!grrResolveFontState(grr, dw, font, &fontState)) return;

    int32_t textLen = (int32_t)strlen(text);
    if (textLen == 0) return;

    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation)
        ? TextUtils_lineStride(font)
        : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    uint8_t a = grrAlphaToU8(alpha);
    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset, gradX = 0.0f;
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = lineLen > 0;
        if (hasCh) ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = pos < lineLen;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != NULL) {
                float advance = (float)glyph->shift;
                float lf = (lineWidth > 0.0f) ? (gradX / lineWidth) : 0.0f;
                float rf = (lineWidth > 0.0f) ? ((gradX + advance) / lineWidth) : 1.0f;
                int32_t c0 = Color_lerp(_c1, _c2, lf);
                int32_t c1 = Color_lerp(_c1, _c2, rf);
                int32_t c2 = Color_lerp(_c4, _c3, rf);
                int32_t c3 = Color_lerp(_c4, _c3, lf);

                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    WiiTexture* glyphTex;
                    float u0, v0, u1, v1, lx0, ly0;
                    if (grrResolveGlyph(grr, dw, &fontState, glyph, cursorX, cursorY,
                                        &glyphTex, &u0, &v0, &u1, &v1, &lx0, &ly0)) {
                        float lx1 = lx0 + (float)glyph->sourceWidth;
                        float ly1 = ly0 + (float)glyph->sourceHeight;
                        float px0, py0, px1, py1, px2, py2, px3, py3;
                        Matrix4f_transformPoint(&transform, lx0, ly0, &px0, &py0);
                        Matrix4f_transformPoint(&transform, lx1, ly0, &px1, &py1);
                        Matrix4f_transformPoint(&transform, lx1, ly1, &px2, &py2);
                        Matrix4f_transformPoint(&transform, lx0, ly1, &px3, &py3);
                        loadTex(glyphTex);
                        emitQuadGrad(px0, py0, px1, py1, px2, py2, px3, py3,
                                     u0, v0, u1, v1,
                                     BGR_R(c0), BGR_G(c0), BGR_B(c0),
                                     BGR_R(c1), BGR_G(c1), BGR_B(c1),
                                     BGR_R(c2), BGR_G(c2), BGR_B(c2),
                                     BGR_R(c3), BGR_G(c3), BGR_B(c3),
                                     a);
                        if (hasNext) {
                            float kern = TextUtils_getKerningOffset(glyph, nextCh);
                            cursorX += kern; gradX += kern;
                        }
                    }
                }
                cursorX += glyph->shift; gradX += glyph->shift;
            }
            ch = nextCh; hasCh = hasNext;
        }

        cursorY += lineStride;
        lineStart = (lineEnd < textLen) ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }
}

// ===================================================================
// Surface drawing
// ===================================================================

static void grrDrawSurface(
    Renderer* renderer, int32_t surfaceID,
    int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
    float x, float y, float xscale, float yscale, float angleDeg,
    uint32_t color, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return;
    if (surfaceID < 0 || (uint32_t)surfaceID >= grr->surfaceCount) return;
    if (!grr->surfaceBuffers[surfaceID]) return;

    int32_t texW = grr->surfaceWidth[surfaceID];
    int32_t texH = grr->surfaceHeight[surfaceID];
    if (srcWidth <= 0) srcWidth = texW;
    if (srcHeight <= 0) srcHeight = texH;
    if (srcLeft < 0) {
        srcWidth += srcLeft;
        x -= (float)srcLeft * xscale;
        srcLeft = 0;
    }
    if (srcTop < 0) {
        srcHeight += srcTop;
        y -= (float)srcTop * yscale;
        srcTop = 0;
    }
    if (srcLeft + srcWidth > texW) srcWidth = texW - srcLeft;
    if (srcTop + srcHeight > texH) srcHeight = texH - srcTop;
    if (srcWidth <= 0 || srcHeight <= 0) return;

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    float x0 = x;
    float y0 = y;
    float x1 = x + (float)srcWidth * xscale;
    float y1 = y;
    float x2 = x1;
    float y2 = y + (float)srcHeight * yscale;
    float x3 = x;
    float y3 = y2;

    if (angleDeg != 0.0f) {
        float rad = -angleDeg * ((float)M_PI / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);
        float px[4] = { x0, x1, x2, x3 };
        float py[4] = { y0, y1, y2, y3 };
        float* ox[4] = { &x0, &x1, &x2, &x3 };
        float* oy[4] = { &y0, &y1, &y2, &y3 };
        for (int i = 0; i < 4; i++) {
            float dx = px[i] - x;
            float dy = py[i] - y;
            *ox[i] = cosA * dx - sinA * dy + x;
            *oy[i] = sinA * dx + cosA * dy + y;
        }
    }

    float u0 = (float)srcLeft / (float)texW;
    float v0 = (float)srcTop / (float)texH;
    float u1 = (float)(srcLeft + srcWidth) / (float)texW;
    float v1 = (float)(srcTop + srcHeight) / (float)texH;

    loadTexObj(&grr->surfaceTextures[surfaceID]);
    emitQuad(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

static void grrDrawSurfaceStretched(
    Renderer* renderer, int32_t surfaceID,
    float x, float y, float width, float height)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= grr->surfaceCount) return;
    if (!grr->surfaceBuffers[surfaceID]) return;

    loadTexObj(&grr->surfaceTextures[surfaceID]);
    emitQuad(x, y, x+width, y, x+width, y+height, x, y+height,
             0.0f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255);
}

static void grrDrawSurfacePart(
    Renderer* renderer, int32_t surfaceID,
    int32_t x, int32_t y,
    int32_t left, int32_t top, int32_t width, int32_t height,
    float xscale, float yscale, uint32_t color, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceID < 0 || (uint32_t)surfaceID >= grr->surfaceCount) return;
    if (!grr->surfaceBuffers[surfaceID]) return;

    int32_t texW = grr->surfaceWidth[surfaceID];
    int32_t texH = grr->surfaceHeight[surfaceID];
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    float u0 = (float)left / texW,          v0 = (float)top / texH;
    float u1 = (float)(left+width) / texW,  v1 = (float)(top+height) / texH;

    float x0 = (float)x,                    y0 = (float)y;
    float x1 = x0 + width*xscale,           y1 = y0;
    float x2 = x1,                          y2 = y0 + height*yscale;
    float x3 = x0,                          y3 = y2;

    loadTexObj(&grr->surfaceTextures[surfaceID]);
    emitQuad(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, r, g, b, a);
}

// ===================================================================
// grrClearScreen  — replaces GX_FillScreen with a raw GX quad
// Uses screen-space coordinates, resets projection before grrBeginView
// takes over.
// ===================================================================
static void grrClearScreen(Renderer* renderer, uint32_t color, float alpha)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = grrAlphaToU8(alpha);

    float sw = (float)g_wiiRmode->fbWidth;
    float sh = (float)g_wiiRmode->efbHeight;

    Mtx44 proj;
    guOrtho(proj, 0.0f, sh, 0.0f, sw, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

    Mtx mv; guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    useColorOnlyTev();
    emitQuad(0, 0, sw, 0, sw, sh, 0, sh,
             0.5f, 0.5f, 0.5f, 0.5f, r, g, b, a);
    // grrBeginView/grrBeginGUI will reload the correct projection next.
}

// ===================================================================
// Surface management
// ===================================================================

static int32_t grrCreateSurface(Renderer* renderer, int32_t width, int32_t height)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (width <= 0 || height <= 0) return -1;

    if (grr->surfaceCount >= grr->surfaceCapacity) {
        uint32_t oldCapacity = grr->surfaceCapacity;
        uint32_t newCapacity = oldCapacity == 0 ? 4 : oldCapacity * 2;
        grr->surfaceTextures = safeRealloc(grr->surfaceTextures, newCapacity * sizeof(GXTexObj));
        grr->surfaceBuffers = safeRealloc(grr->surfaceBuffers, newCapacity * sizeof(void*));
        grr->surfaceWidth = safeRealloc(grr->surfaceWidth, newCapacity * sizeof(int32_t));
        grr->surfaceHeight = safeRealloc(grr->surfaceHeight, newCapacity * sizeof(int32_t));

        memset(grr->surfaceTextures + oldCapacity, 0, (newCapacity - oldCapacity) * sizeof(GXTexObj));
        memset(grr->surfaceBuffers + oldCapacity, 0, (newCapacity - oldCapacity) * sizeof(void*));
        memset(grr->surfaceWidth + oldCapacity, 0, (newCapacity - oldCapacity) * sizeof(int32_t));
        memset(grr->surfaceHeight + oldCapacity, 0, (newCapacity - oldCapacity) * sizeof(int32_t));
        grr->surfaceCapacity = newCapacity;
    }

    uint32_t id = grr->surfaceCount++;

    grr->surfaceWidth[id]  = width;
    grr->surfaceHeight[id] = height;

    uint32_t size = GX_GetTexBufferSize(width, height, GX_TF_RGBA8, GX_FALSE, 0);
    void* buffer = memalign(32, size);
    if (!buffer) {
        grr->surfaceWidth[id] = 0;
        grr->surfaceHeight[id] = 0;
        grr->surfaceCount--;
        return -1;
    }

    memset(buffer, 0, size);
    DCFlushRange(buffer, size);

    GX_InitTexObj(&grr->surfaceTextures[id], buffer, width, height,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&grr->surfaceTextures[id], GX_LINEAR, GX_LINEAR);
    grr->surfaceBuffers[id] = buffer;
    fprintf(stderr, "GRR: Created surface %u (%dx%d)\n", id, width, height);
    return (int32_t)id;
}

static void grrSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) {
        if (renderer->runner != NULL) {
            if (width > 0) renderer->runner->applicationWidth = width;
            if (height > 0) renderer->runner->applicationHeight = height;
        }
        return;
    }
    if (surfaceID < 0 || (uint32_t)surfaceID >= grr->surfaceCount) return;
    if (width <= 0 || height <= 0) return;
    if (grr->surfaceWidth[surfaceID] == width && grr->surfaceHeight[surfaceID] == height) return;

    free(grr->surfaceBuffers[surfaceID]);
    uint32_t texSize = GX_GetTexBufferSize(width, height, GX_TF_RGBA8, GX_FALSE, 0);
    void* buf = memalign(32, texSize);
    if (!buf) { grr->surfaceBuffers[surfaceID] = NULL; return; }

    memset(buf, 0, texSize);
    DCFlushRange(buf, texSize);

    GX_InitTexObj(&grr->surfaceTextures[surfaceID], buf, width, height,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&grr->surfaceTextures[surfaceID], GX_LINEAR, GX_LINEAR);
    grr->surfaceBuffers[surfaceID] = buf;
    grr->surfaceWidth[surfaceID]   = width;
    grr->surfaceHeight[surfaceID]  = height;
    fprintf(stderr, "GRR: Resized surface %d to %dx%d\n", surfaceID, width, height);
}

static void grrSurfaceFree(Renderer* renderer, int32_t surfaceID)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return;
    if (surfaceID < 0 || (uint32_t)surfaceID >= grr->surfaceCount) return;
    free(grr->surfaceBuffers[surfaceID]);
    grr->surfaceBuffers[surfaceID] = NULL;
    grr->surfaceWidth[surfaceID]   = 0;
    grr->surfaceHeight[surfaceID]  = 0;
    memset(&grr->surfaceTextures[surfaceID], 0, sizeof(GXTexObj));
    fprintf(stderr, "GRR: Freed surface %d\n", surfaceID);
}

static bool grrSurfaceExists(Renderer* renderer, int32_t surfaceId)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId == APPLICATION_SURFACE_ID) return true;
    return surfaceId >= 0
        && (uint32_t)surfaceId < grr->surfaceCount
        && grr->surfaceBuffers[surfaceId] != NULL;
}

static bool grrSurfaceGetPixels(Renderer* renderer, int32_t surfaceId, uint8_t* outRGBA)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId == APPLICATION_SURFACE_ID) return false;
    if (surfaceId < 0 || (uint32_t)surfaceId >= grr->surfaceCount) return false;
    if (!grr->surfaceBuffers[surfaceId]) return false;
    int32_t w = grr->surfaceWidth[surfaceId], h = grr->surfaceHeight[surfaceId];
    if (w <= 0 || h <= 0) return false;
    convertGX_RGBA8toRGBA(grr->surfaceBuffers[surfaceId], outRGBA, (uint32_t)w, (uint32_t)h);
    return true;
}

static void grrCaptureEfbToSurface(GRRRenderer* grr, int32_t surfaceId)
{
    if (surfaceId < 0 || (uint32_t)surfaceId >= grr->surfaceCount) return;
    if (!grr->surfaceBuffers[surfaceId]) return;

    int32_t w = grr->surfaceWidth[surfaceId];
    int32_t h = grr->surfaceHeight[surfaceId];
    if (w <= 0 || h <= 0) return;
    if (w > (int32_t)g_wiiRmode->fbWidth || h > (int32_t)g_wiiRmode->efbHeight) {
        fprintf(stderr, "GRR: Surface %d (%dx%d) too large for EFB copy (%ux%u)\n",
                surfaceId, w, h, g_wiiRmode->fbWidth, g_wiiRmode->efbHeight);
        return;
    }

    uint32_t texSize = GX_GetTexBufferSize((u16)w, (u16)h, GX_TF_RGBA8, GX_FALSE, 0);
    GX_DrawDone();
    GX_SetTexCopySrc(0, 0, (u16)w, (u16)h);
    GX_SetTexCopyDst((u16)w, (u16)h, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(grr->surfaceBuffers[surfaceId], GX_FALSE);
    GX_PixModeSync();
    GX_DrawDone();
    DCInvalidateRange(grr->surfaceBuffers[surfaceId], texSize);
    GX_InitTexObj(&grr->surfaceTextures[surfaceId], grr->surfaceBuffers[surfaceId], (u16)w, (u16)h,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&grr->surfaceTextures[surfaceId], GX_NEAR, GX_NEAR);
    GX_InvalidateTexAll();
}

static void grrSurfaceCopy(
    Renderer* renderer,
    int32_t DestID, int32_t DestX, int32_t DestY,
    int32_t SrcID,  int32_t SrcX,  int32_t SrcY,
    int32_t SrcW,   int32_t SrcH,  bool part)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (SrcID == APPLICATION_SURFACE_ID || DestID == APPLICATION_SURFACE_ID) return;
    if (SrcID  < 0 || (uint32_t)SrcID  >= grr->surfaceCount) return;
    if (DestID < 0 || (uint32_t)DestID >= grr->surfaceCount) return;

    int32_t srcW = grr->surfaceWidth[SrcID];
    int32_t srcH = grr->surfaceHeight[SrcID];
    int32_t dstW = grr->surfaceWidth[DestID];

    if (!part) {
        // Full copy
        SrcX = SrcY = DestX = DestY = 0;
        SrcW = srcW; SrcH = srcH;
    }

    if (SrcX < 0) { SrcW += SrcX; SrcX = 0; }
    if (SrcY < 0) { SrcH += SrcY; SrcY = 0; }
    if (SrcW <= 0 || SrcH <= 0) return;

    int32_t dstH = grr->surfaceHeight[DestID];
    if (DestX < 0) { SrcX -= DestX; SrcW += DestX; DestX = 0; }
    if (DestY < 0) { SrcY -= DestY; SrcH += DestY; DestY = 0; }
    if (SrcX < 0) { SrcW += SrcX; DestX -= SrcX; SrcX = 0; }
    if (SrcY < 0) { SrcH += SrcY; DestY -= SrcY; SrcY = 0; }
    if (SrcW <= 0 || SrcH <= 0) return;

    uint8_t* srcLinear = safeMalloc((size_t)srcW * (size_t)srcH * 4);
    uint8_t* dstLinear = safeMalloc((size_t)dstW * (size_t)dstH * 4);
    convertGX_RGBA8toRGBA(grr->surfaceBuffers[SrcID], srcLinear, (uint32_t)srcW, (uint32_t)srcH);
    convertGX_RGBA8toRGBA(grr->surfaceBuffers[DestID], dstLinear, (uint32_t)dstW, (uint32_t)dstH);

    for (int32_t row = 0; row < SrcH; row++) {
        if (SrcY + row >= srcH) break;
        if (DestY + row >= dstH) break;
        uint8_t* srcRow = srcLinear + ((SrcY + row) * srcW + SrcX) * 4;
        uint8_t* dstRow = dstLinear + ((DestY + row) * dstW + DestX) * 4;
        int32_t copyW = SrcW;
        if (DestX + copyW > dstW) copyW = dstW - DestX;
        if (SrcX + copyW > srcW) copyW = srcW - SrcX;
        if (copyW <= 0) break;
        memcpy(dstRow, srcRow, (size_t)copyW * 4);
    }

    uint32_t dstSize = GX_GetTexBufferSize((u16)dstW, (u16)dstH, GX_TF_RGBA8, GX_FALSE, 0);
    convertRGBAtoGX_RGBA8(dstLinear, grr->surfaceBuffers[DestID], (uint32_t)dstW, (uint32_t)dstH);
    DCFlushRange(grr->surfaceBuffers[DestID], dstSize);
    GX_InvalidateTexAll();
    free(srcLinear);
    free(dstLinear);
}

static float grrGetSurfaceWidth(Renderer* renderer, int32_t surfaceId)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId == APPLICATION_SURFACE_ID) {
        if (renderer->runner != NULL && renderer->runner->applicationWidth > 0)
            return (float)renderer->runner->applicationWidth;
        return (float)(grr->gameW > 0 ? grr->gameW : g_wiiRmode->fbWidth);
    }
    if (surfaceId < 0 || (uint32_t)surfaceId >= grr->surfaceCount) return 0.0f;
    return (float)grr->surfaceWidth[surfaceId];
}

static float grrGetSurfaceHeight(Renderer* renderer, int32_t surfaceId)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId == APPLICATION_SURFACE_ID) {
        if (renderer->runner != NULL && renderer->runner->applicationHeight > 0)
            return (float)renderer->runner->applicationHeight;
        return (float)(grr->gameH > 0 ? grr->gameH : g_wiiRmode->efbHeight);
    }
    if (surfaceId < 0 || (uint32_t)surfaceId >= grr->surfaceCount) return 0.0f;
    return (float)grr->surfaceHeight[surfaceId];
}

// ===================================================================
// Sprite-from-surface
// ===================================================================
static int32_t grrCreateSpriteFromSurface(
    Renderer* renderer, int32_t surfaceID,
    int32_t x, int32_t y, int32_t w, int32_t h,
    bool removeback, bool smooth,
    int32_t xorig, int32_t yorig)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (w <= 0 || h <= 0) return -1;
    if (surfaceID < 0 || (uint32_t)surfaceID >= grr->surfaceCount) return -1;

    int32_t srcW = grr->surfaceWidth[surfaceID];
    int32_t srcH = grr->surfaceHeight[surfaceID];
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > srcW) w = srcW - x;
    if (y + h > srcH) h = srcH - y;
    if (w <= 0 || h <= 0) return -1;

    uint32_t texSize = GX_GetTexBufferSize(w, h, GX_TF_RGBA8, GX_FALSE, 0);
    void* texData = memalign(32, texSize);
    if (!texData) return -1;

    uint8_t* srcPixels = (uint8_t*)grr->surfaceBuffers[surfaceID];

    uint8_t* fullLinear = (uint8_t*)malloc((size_t)srcW * (size_t)srcH * 4);
    if (!fullLinear) { free(texData); return -1; }
    convertGX_RGBA8toRGBA(srcPixels, fullLinear, (uint32_t)srcW, (uint32_t)srcH);

    uint8_t* linear = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
    if (!linear) { free(fullLinear); free(texData); return -1; }
    for (int32_t row = 0; row < h; row++) {
        memcpy(linear + (size_t)row * (size_t)w * 4,
               fullLinear + ((size_t)(y + row) * (size_t)srcW + (size_t)x) * 4,
               (size_t)w * 4);
    }
    convertRGBAtoGX_RGBA8(linear, texData, (uint32_t)w, (uint32_t)h);
    free(linear);
    free(fullLinear);

    DCFlushRange(texData, texSize);

    // Use WiiTexture as a plain container (no draw calls).
    WiiTexture* newTex = WiiTexture_create((u32)w, (u32)h);
    if (!newTex) { free(texData); return -1; }
    free(newTex->data);
    newTex->data = texData; newTex->w = (u32)w; newTex->h = (u32)h;
    newTex->format = GX_TF_RGBA8;
    DCFlushRange(newTex->data, texSize);

    uint32_t pageId = findOrAllocTexturePageSlot(grr);
    grr->textures[pageId] = newTex;
    grr->textureWidths[pageId] = w;
    grr->textureHeights[pageId] = h;
    grr->textureSizes[pageId] = texSize;
    grr->textureLastUsed[pageId] = grr->textureFrame;
    grr->textureLoaded[pageId] = true;
    grr->textureOversized[pageId] = false;

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, grr->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = tpag->targetX = tpag->sourceY = tpag->targetY = 0;
    tpag->sourceWidth  = tpag->targetWidth  = tpag->boundingWidth  = (uint16_t)w;
    tpag->sourceHeight = tpag->targetHeight = tpag->boundingHeight = (uint16_t)h;
    tpag->texturePageId = (int16_t)pageId;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, grr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t)w; sprite->height = (uint32_t)h;
    sprite->originX = xorig; sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = safeMalloc(sizeof(int32_t));
    if (sprite->tpagIndices) sprite->tpagIndices[0] = (int32_t)tpagIndex;
    sprite->maskCount = 0; sprite->masks = NULL;

    fprintf(stderr, "GRR: Created dynamic sprite %u (%dx%d) from surface %d\n",
            spriteIndex, w, h, surfaceID);
    return (int32_t)spriteIndex;
}

static void grrDeleteSprite(Renderer* renderer, int32_t spriteIndex)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (spriteIndex < 0 || (uint32_t)spriteIndex >= dw->sprt.count) return;
    if ((uint32_t)spriteIndex < grr->originalSpriteCount) {
        fprintf(stderr, "GRR: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t)tpagIdx >= grr->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && (uint32_t)pageId < grr->textureCount && grr->textures[pageId]) {
                WiiTexture_free(grr->textures[pageId]);
                grr->textures[pageId] = NULL;
            }
            tpag->texturePageId = -1;
        }
    }

    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
    fprintf(stderr, "GRR: Deleted sprite %d\n", spriteIndex);
}

// ===================================================================
// Surface target stack
// ===================================================================

static int32_t findSurfaceStackSlot(GRRRenderer* grr) {
    for (int32_t i = 0; i < 16; i++) if (grr->surfaceStack[i] == -1) return i;
    return -1;
}
static void removeSurfaceStackSlot(GRRRenderer* grr) {
    for (int32_t i = 15; i >= 0; i--) if (grr->surfaceStack[i] != -1) { grr->surfaceStack[i] = -1; return; }
}
static int32_t findSurfaceStackTop(GRRRenderer* grr) {
    for (int32_t i = 15; i >= 0; i--) if (grr->surfaceStack[i] != -1) return i;
    return -1;
}

static bool grrBindRenderTarget(Renderer* renderer, int32_t surfaceId)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId >= 0) {
        if ((uint32_t)surfaceId >= grr->surfaceCount || !grr->surfaceBuffers[surfaceId]) return false;
        grr->base.CPortX = grr->base.CPortY = 0;
        grr->base.CPortW = grr->surfaceWidth[surfaceId];
        grr->base.CPortH = grr->surfaceHeight[surfaceId];

        Mtx44 proj;
        guOrtho(proj, 0.0f, (f32)grr->surfaceHeight[surfaceId],
                      0.0f, (f32)grr->surfaceWidth[surfaceId], -1.0f, 1.0f);
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
        GX_SetViewport(0, 0, (f32)grr->surfaceWidth[surfaceId], (f32)grr->surfaceHeight[surfaceId], 0, 1);
        GX_SetScissor(0, 0, grr->surfaceWidth[surfaceId], grr->surfaceHeight[surfaceId]);
    } else {
        if (renderer->PreviousViewMatrix.m[0] != 0.0f) {
            Mtx44 proj;
            Matrix4f_to_Mtx44(&renderer->PreviousViewMatrix, proj);
            GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
        }
        GX_SetViewport((f32)grr->base.CPortX, (f32)grr->base.CPortY,
                       (f32)grr->base.CPortW, (f32)grr->base.CPortH, 0, 1);
        GX_SetScissor(grr->base.CPortX, grr->base.CPortY,
                      grr->base.CPortW, grr->base.CPortH);
    }
    return true;
}

static bool grrSetSurfaceTarget(Renderer* renderer, int32_t surfaceId)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId >= 0 && ((uint32_t)surfaceId >= grr->surfaceCount || !grr->surfaceBuffers[surfaceId])) return false;
    int32_t slot = findSurfaceStackSlot(grr);
    if (slot == -1) return false;
    grr->surfaceStack[slot] = surfaceId;
#if !WII_GX_ENABLE_SURFACE_CAPTURE
    if (g_wiiSurfaceTargetLogCount < 8) {
        fprintf(stderr, "GRR: Surface target %d requested; passing through to EFB on Wii\n", surfaceId);
        g_wiiSurfaceTargetLogCount++;
    }
    return true;
#else
    return grrBindRenderTarget(renderer, surfaceId);
#endif
}

static bool grrResetSurfaceTarget(Renderer* renderer)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    int32_t oldTop = findSurfaceStackTop(grr);
#if WII_GX_ENABLE_SURFACE_CAPTURE
    if (oldTop != -1) grrCaptureEfbToSurface(grr, grr->surfaceStack[oldTop]);
#endif
    removeSurfaceStackSlot(grr);
    int32_t top = findSurfaceStackTop(grr);
#if !WII_GX_ENABLE_SURFACE_CAPTURE
    if (top == -1 && renderer->PreviousViewMatrix.m[0] != 0.0f) {
        Mtx44 proj;
        Matrix4f_to_Mtx44(&renderer->PreviousViewMatrix, proj);
        GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    }
    return true;
#else
    return grrBindRenderTarget(renderer, top != -1 ? grr->surfaceStack[top] : -1);
#endif
}

static bool grrSetRenderTarget(Renderer* renderer, int32_t surfaceId)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (surfaceId == APPLICATION_SURFACE_ID) {
#if !WII_GX_ENABLE_SURFACE_CAPTURE
        return true;
#else
        return grrBindRenderTarget(renderer, -1);
#endif
    }

    if (surfaceId < 0 || (uint32_t)surfaceId >= grr->surfaceCount || !grr->surfaceBuffers[surfaceId])
        return false;

#if !WII_GX_ENABLE_SURFACE_CAPTURE
    if (g_wiiSurfaceTargetLogCount < 8) {
        fprintf(stderr, "GRR: Surface target %d requested; passing through to EFB on Wii\n", surfaceId);
        g_wiiSurfaceTargetLogCount++;
    }
    return true;
#else
    return grrBindRenderTarget(renderer, surfaceId);
#endif
}

static int32_t grrEnsureApplicationSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height)
{
    return APPLICATION_SURFACE_ID;
}

// ===================================================================
// Frame management
// ===================================================================

static void grrDrawLoadIndicator(GRRRenderer* grr)
{
    if (grr->loadIndicatorFrames == 0) return;

    grrConfigureGXState();
    useColorOnlyTev();

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)g_wiiRmode->efbHeight, 0.0f, (f32)g_wiiRmode->fbWidth, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    float usage = GX_FULL_TEXTURE_BUDGET > 0 ? (float)grr->textureBytes / (float)GX_FULL_TEXTURE_BUDGET : 0.0f;
    if (usage > 1.0f) usage = 1.0f;
    float x = 8.0f;
    float y = 8.0f;
    float w = 64.0f;
    float h = 7.0f;
    emitLoadingRect(x - 1.0f, y - 1.0f, x + w + 1.0f, y + h + 1.0f, (GXColor){20, 20, 20, 220});
    emitLoadingRect(x, y, x + w, y + h, (GXColor){55, 55, 55, 220});
    emitLoadingRect(x, y, x + w * usage, y + h, (GXColor){130, 210, 80, 240});

    grr->loadIndicatorFrames--;
}

#if WII_GX_DEBUG_OVERLAY
static void grrDrawDebugOverlay(void)
{
    if (!g_wiiDebugOverlayLine1[0] && !g_wiiDebugOverlayLine2[0]) return;

    grrConfigureGXState();
    useColorOnlyTev();

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)g_wiiRmode->efbHeight, 0.0f, (f32)g_wiiRmode->fbWidth, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    float sw = (float)g_wiiRmode->fbWidth;
    float y = (float)g_wiiRmode->efbHeight - 34.0f;
    emitLoadingRect(6.0f, y - 4.0f, sw - 6.0f, y + 28.0f, (GXColor){0, 0, 0, 180});
    drawLoadingText(10.0f, y, g_wiiDebugOverlayLine1, 2, (GXColor){230, 230, 230, 255});
    drawLoadingText(10.0f, y + 14.0f, g_wiiDebugOverlayLine2, 2, (GXColor){170, 220, 255, 255});
}
#endif

static void grrBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH,
                           int32_t windowW, int32_t windowH)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    grr->gameW = gameW;
    grr->gameH = gameH;
    grr->windowW = windowW;
    grr->windowH = windowH;
    grrUpdateCanvas(grr);
    grr->textureFrame++;
    g_wiiFrameQuads = 0;
    g_wiiFrameSpriteDraws = 0;
    g_wiiFrameTextureLoads = 0;
    grr->base.CPortX = grr->canvasX;
    grr->base.CPortY = grr->canvasY;
    grr->base.CPortW = grr->canvasW;
    grr->base.CPortH = grr->canvasH;
    grr->cullEnabled = false;

    // Re-assert the full GX pipeline state every frame.
    // This is cheap and makes us immune to any external code (e.g. the
    // GX-based loading screen) having touched these registers.
    grrConfigureGXState();
}

static void grrEndFrameInit(MAYBE_UNUSED Renderer* renderer)
{
}

static void grrEndFrameEnd(Renderer* renderer)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    g_wiiLastFrameQuads = g_wiiFrameQuads;
    g_wiiLastFrameSpriteDraws = g_wiiFrameSpriteDraws;
    g_wiiLastFrameTextureLoads = g_wiiFrameTextureLoads;

    grrDrawLoadIndicator(grr);
#if WII_GX_DEBUG_OVERLAY
    grrDrawDebugOverlay();
#endif

    // Flush GX command buffer, copy EFB→XFB, flip.
    // We do this ourselves instead of calling GX_Render() so that
    // GX never touches the FIFO again after grrInit.
    GX_DrawDone();
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(g_wiiXfb[g_wiiFb], GX_TRUE);
    VIDEO_SetNextFramebuffer(g_wiiXfb[g_wiiFb]);
    VIDEO_Flush();
    g_wiiFb ^= 1;
    VIDEO_WaitVSync();

    grr->FPS = GRRRenderer_CalculateFrameRate();
    grrTrimTextureCache(grr);
    grrTrimCroppedTextureCache(grr);
    grrTrimDecodedTextureCache(grr);
}

static void grrBeginView(
    Renderer* renderer,
    int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
    int32_t portX, int32_t portY, int32_t portW, int32_t portH,
    float viewAngle)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    int32_t screenPortX, screenPortY, screenPortW, screenPortH;
    grrMapPortToScreen(grr, portX, portY, portW, portH,
                       &screenPortX, &screenPortY, &screenPortW, &screenPortH);

    GX_SetViewport((f32)screenPortX, (f32)screenPortY, (f32)screenPortW, (f32)screenPortH, 0.0f, 1.0f);
    GX_SetScissor((u32)screenPortX, (u32)screenPortY, (u32)screenPortW, (u32)screenPortH);

    Mtx44 proj;
    guOrtho(proj,
        (f32)viewY, (f32)(viewY + viewH),
        (f32)viewX, (f32)(viewX + viewW),
        -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx44_to_Matrix4f(proj, &renderer->PreviousViewMatrix);
    grr->cullX0 = (float)viewX;
    grr->cullY0 = (float)viewY;
    grr->cullX1 = (float)(viewX + viewW);
    grr->cullY1 = (float)(viewY + viewH);
    grr->cullEnabled = (viewAngle == 0.0f);

    Mtx modelView;
    if (viewAngle != 0.0f) {
        float cx = (float)viewX + (float)viewW / 2.0f;
        float cy = (float)viewY + (float)viewH / 2.0f;
        Mtx t1, t2, rot, tmp;
        guMtxTrans(t1, cx, cy, 0.0f);
        guMtxRotDeg(rot, 'Z', -viewAngle);
        guMtxTrans(t2, -cx, -cy, 0.0f);
        guMtxConcat(t1, rot, tmp);
        guMtxConcat(tmp, t2, modelView);
    } else {
        guMtxIdentity(modelView);
    }
    GX_LoadPosMtxImm(modelView, GX_PNMTX0);

    renderer->CPortX = screenPortX; renderer->CPortY = screenPortY;
    renderer->CPortW = screenPortW; renderer->CPortH = screenPortH;
}

static void grrBeginGUI(
    Renderer* renderer,
    int32_t guiW, int32_t guiH,
    int32_t portX, int32_t portY, int32_t portW, int32_t portH)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    int32_t screenPortX, screenPortY, screenPortW, screenPortH;
    grrMapPortToScreen(grr, portX, portY, portW, portH,
                       &screenPortX, &screenPortY, &screenPortW, &screenPortH);

    GX_SetViewport((f32)screenPortX, (f32)screenPortY, (f32)screenPortW, (f32)screenPortH, 0.0f, 1.0f);
    GX_SetScissor((u32)screenPortX, (u32)screenPortY, (u32)screenPortW, (u32)screenPortH);

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)guiH, 0.0f, (f32)guiW, -1.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

    Mtx mv; guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    grr->cullX0 = 0.0f;
    grr->cullY0 = 0.0f;
    grr->cullX1 = (float)guiW;
    grr->cullY1 = (float)guiH;
    grr->cullEnabled = true;

    renderer->CPortX = screenPortX; renderer->CPortY = screenPortY;
    renderer->CPortW = screenPortW; renderer->CPortH = screenPortH;
}

static void grrEndView(Renderer* renderer) {
    GRRRenderer* grr = (GRRRenderer*)renderer;
    grr->cullEnabled = false;
    GX_SetScissor(0, 0, (u32)g_wiiRmode->fbWidth, (u32)g_wiiRmode->efbHeight);
}

static void grrEndGUI(Renderer* renderer) {
    GRRRenderer* grr = (GRRRenderer*)renderer;
    grr->cullEnabled = false;
    GX_SetScissor(0, 0, (u32)g_wiiRmode->fbWidth, (u32)g_wiiRmode->efbHeight);
}

static void grrFlush(Renderer* renderer) { /* no-op */ }

// ===================================================================
// Blend / GPU state
// ===================================================================

static u8 grrBlendFactorToGX(int32_t factor)
{
    switch (factor) {
        case bm_zero:            return GX_BL_ZERO;
        case bm_one:             return GX_BL_ONE;
        case bm_src_color:       return GX_BL_SRCCLR;
        case bm_inv_src_color:   return GX_BL_INVSRCCLR;
        case bm_src_alpha:       return GX_BL_SRCALPHA;
        case bm_inv_src_alpha:   return GX_BL_INVSRCALPHA;
        case bm_dest_alpha:      return GX_BL_ONE;       // Wii EFB is RGB8 here, so treat dest alpha as opaque.
        case bm_inv_dest_alpha:  return GX_BL_ZERO;
        case bm_dest_color:      return GX_BL_DSTCLR;
        case bm_inv_dest_color:  return GX_BL_INVDSTCLR;
        case bm_src_alpha_sat:   return GX_BL_SRCALPHA;
        default:                 return GX_BL_ONE;
    }
}

static void grrGpuSetBlendMode(Renderer* renderer, int32_t mode)
{
    switch (mode) {
        case bm_normal:           GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
        case bm_add:              GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_ONE,         GX_LO_CLEAR); break;
        case bm_subtract:         GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_SRCALPHA, GX_BL_ONE,         GX_LO_CLEAR); break;
        case bm_reverse_subtract: GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE,      GX_BL_SRCALPHA,    GX_LO_CLEAR); break;
        case bm_min:              GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
        case bm_max:              GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
        default:                  GX_SetBlendMode(GX_BM_BLEND,    GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR); break;
    }
}

static void grrGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor)
{
    GX_SetBlendMode(GX_BM_BLEND, grrBlendFactorToGX(sfactor), grrBlendFactorToGX(dfactor), GX_LO_CLEAR);
}

static void grrGpuSetBlendEnable(Renderer* renderer, bool enable)
{
    if (enable) GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    else {
        // Keeping alpha blending active avoids GX writing transparent texels as black boxes
        // when GML temporarily disables blending around text or UI sprites.
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    }
}

static bool grrGpuGetBlendEnable(MAYBE_UNUSED Renderer* renderer)
{
    return true;
}

static void grrGpuSetAlphaTestEnable(Renderer* renderer, bool enable)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    grr->alphaTestEnable = enable;
    if (enable) GX_SetAlphaCompare(GX_GREATER, grrAlphaToU8(grr->alphaTestRef), GX_AOP_AND, GX_ALWAYS, 0);
    else        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

static void grrGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    grr->alphaTestRef = ref / 255.0f;
    if (grr->alphaTestEnable)
        GX_SetAlphaCompare(GX_GREATER, ref, GX_AOP_AND, GX_ALWAYS, 0);
}

static void grrGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha)
{
    GX_SetColorUpdate(red || green || blue);
    GX_SetAlphaUpdate(alpha);
}

static void grrGpuGetColorWriteEnable(MAYBE_UNUSED Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha)
{
    if (red) *red = true;
    if (green) *green = true;
    if (blue) *blue = true;
    if (alpha) *alpha = true;
}

static void grrGpuSetFog(Renderer* renderer, bool enable, uint32_t color)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    grr->fogEnable = enable;
    grr->fogColor  = color;
    if (!enable) {
        GX_SetFog(GX_FOG_NONE, 0, 0, 0, 0, (GXColor){0,0,0,255});
        return;
    }
    GXColor fc = { BGR_R(color), BGR_G(color), BGR_B(color), 255 };
    GX_SetFog(GX_FOG_LIN, 0.0f, 1000.0f, 0.0f, 1.0f, fc);
}

// ===================================================================
// Init / Destroy
// ===================================================================

static void grrInit(Renderer* renderer, DataWin* dataWin)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    renderer->dataWin = dataWin;

    if (grr->whiteTexture) {
        WiiTexture_free(grr->whiteTexture);
        grr->whiteTexture = NULL;
    }
    if (grr->textures) {
        for (uint32_t i = 0; i < grr->textureCount; i++) {
            if (grr->textures[i]) WiiTexture_free(grr->textures[i]);
        }
        free(grr->textures);
        grr->textures = NULL;
    }
    if (grr->tpagTextures) {
        for (uint32_t i = 0; i < grr->tpagTextureCount; i++) {
            if (grr->tpagTextures[i]) WiiTexture_free(grr->tpagTextures[i]);
        }
        free(grr->tpagTextures);
        free(grr->tpagTextureSizes);
        free(grr->tpagTextureLastUsed);
        grr->tpagTextures = NULL;
        grr->tpagTextureSizes = NULL;
        grr->tpagTextureLastUsed = NULL;
        grr->tpagTextureCount = 0;
        grr->tpagTextureBytes = 0;
    }
    if (grr->decodedTexturePixels) {
        for (uint32_t i = 0; i < grr->textureCount; i++) free(grr->decodedTexturePixels[i]);
        free(grr->decodedTexturePixels);
        grr->decodedTexturePixels = NULL;
    }
    free(grr->textureWidths);
    free(grr->textureHeights);
    free(grr->decodedTextureSizes);
    free(grr->decodedTextureLastUsed);
    free(grr->textureSizes);
    free(grr->textureLastUsed);
    free(grr->textureLoaded);
    free(grr->textureOversized);
    grr->textureWidths = NULL;
    grr->textureHeights = NULL;
    grr->decodedTextureSizes = NULL;
    grr->decodedTextureLastUsed = NULL;
    grr->decodedTextureBytes = 0;
    grr->textureSizes = NULL;
    grr->textureLastUsed = NULL;
    grr->textureBytes = 0;
    grr->textureLoaded = NULL;
    grr->textureOversized = NULL;

    grrConfigureGXState();

    // White 4×4 texture for solid-color primitives.
    // (WiiTexture_create is only used as a malloc wrapper here —
    //  no GX draw calls ever use this texture.)
    uint8_t whitePixels[4 * 4 * 4];
    memset(whitePixels, 0xFF, sizeof(whitePixels));
    grr->whiteTexture = grrCreateTextureFromRgba(whitePixels, 4, 4);

    grr->textureCount             = dataWin->txtr.count;
    grr->originalTexturePageCount = dataWin->txtr.count;
    grr->originalTpagCount        = dataWin->tpag.count;
    grr->originalSpriteCount      = dataWin->sprt.count;
    grr->textures = safeCalloc(grr->textureCount, sizeof(WiiTexture*));
    grr->decodedTexturePixels = safeCalloc(grr->textureCount, sizeof(uint8_t*));
    grr->textureWidths = safeCalloc(grr->textureCount, sizeof(int32_t));
    grr->textureHeights = safeCalloc(grr->textureCount, sizeof(int32_t));
    grr->decodedTextureSizes = safeCalloc(grr->textureCount, sizeof(uint32_t));
    grr->decodedTextureLastUsed = safeCalloc(grr->textureCount, sizeof(uint32_t));
    grr->decodedTextureBytes = 0;
    grr->textureSizes = safeCalloc(grr->textureCount, sizeof(uint32_t));
    grr->textureLastUsed = safeCalloc(grr->textureCount, sizeof(uint32_t));
    grr->textureBytes = 0;
    grr->textureLoaded = safeCalloc(grr->textureCount, sizeof(bool));
    grr->textureOversized = safeCalloc(grr->textureCount, sizeof(bool));
    grr->tpagTextureCount = dataWin->tpag.count;
    grr->tpagTextures = safeCalloc(grr->tpagTextureCount, sizeof(WiiTexture*));
    grr->tpagTextureSizes = safeCalloc(grr->tpagTextureCount, sizeof(uint32_t));
    grr->tpagTextureLastUsed = safeCalloc(grr->tpagTextureCount, sizeof(uint32_t));
    grr->tpagTextureBytes = 0;
    grr->textureFrame = 1;

    memset(grr->surfaceStack, -1, sizeof(grr->surfaceStack));

    fprintf(stderr, "GRR: Renderer initialized (%u texture pages)\n", grr->textureCount);
}

static void grrDestroy(Renderer* renderer)
{
    GRRRenderer* grr = (GRRRenderer*)renderer;
    if (grr->whiteTexture) WiiTexture_free(grr->whiteTexture);
    for (uint32_t i = 0; i < grr->textureCount; i++) {
        if (grr->textures[i]) WiiTexture_free(grr->textures[i]);
        free(grr->decodedTexturePixels[i]);
    }
    for (uint32_t i = 0; i < grr->tpagTextureCount; i++) {
        if (grr->tpagTextures[i]) WiiTexture_free(grr->tpagTextures[i]);
    }
    for (uint32_t i = 0; i < grr->surfaceCount; i++) {
        free(grr->surfaceBuffers[i]);
    }
    free(grr->textures);
    free(grr->decodedTexturePixels);
    free(grr->textureWidths);
    free(grr->textureHeights);
    free(grr->decodedTextureSizes);
    free(grr->decodedTextureLastUsed);
    free(grr->textureSizes);
    free(grr->textureLastUsed);
    free(grr->textureLoaded);
    free(grr->textureOversized);
    free(grr->tpagTextures);
    free(grr->tpagTextureSizes);
    free(grr->tpagTextureLastUsed);
    free(grr->surfaceTextures);
    free(grr->surfaceBuffers);
    free(grr->surfaceWidth);
    free(grr->surfaceHeight);
    free(renderer);
}

// ===================================================================
// Vtable + constructor
// ===================================================================

static RendererVtable grrVtable = {
    .init                = grrInit,
    .destroy             = grrDestroy,
    .beginFrame          = grrBeginFrame,
    .endFrameInit        = grrEndFrameInit,
    .endFrameEnd         = grrEndFrameEnd,
    .drawSprite          = grrDrawSprite,
    .drawSpritePart      = grrDrawSpritePart,
    .drawSpritePos       = grrDrawSpritePos,
    .drawRectangle       = grrDrawRectangle,
    .drawRectangleColor  = grrDrawRectangleColor,
    .drawLine            = grrDrawLine,
    .drawLineColor       = grrDrawLineColor,
    .drawTriangle        = grrDrawTriangle,
    .drawText            = grrDrawText,
    .drawTextColor       = grrDrawTextColor,
    .drawSurface         = grrDrawSurface,
    .createSpriteFromSurface = grrCreateSpriteFromSurface,
    .createSurface       = grrCreateSurface,
    .surfaceExists       = grrSurfaceExists,
    .setRenderTarget     = grrSetRenderTarget,
    .ensureApplicationSurface = grrEnsureApplicationSurface,
    .deleteSprite        = grrDeleteSprite,
    .clearScreen         = grrClearScreen,
    .beginView           = grrBeginView,
    .beginGUI            = grrBeginGUI,
    .endView             = grrEndView,
    .endGUI              = grrEndGUI,
    .flush               = grrFlush,
    .gpuSetBlendMode     = grrGpuSetBlendMode,
    .gpuSetBlendModeExt  = grrGpuSetBlendModeExt,
    .gpuSetBlendEnable   = grrGpuSetBlendEnable,
    .gpuGetBlendEnable   = grrGpuGetBlendEnable,
    .gpuSetAlphaTestEnable = grrGpuSetAlphaTestEnable,
    .gpuSetAlphaTestRef  = grrGpuSetAlphaTestRef,
    .gpuSetColorWriteEnable = grrGpuSetColorWriteEnable,
    .gpuGetColorWriteEnable = grrGpuGetColorWriteEnable,
    .gpuSetFog           = grrGpuSetFog,
    .surfaceGetPixels    = grrSurfaceGetPixels,
    .surfaceCopy         = grrSurfaceCopy,
    .getSurfaceWidth     = grrGetSurfaceWidth,
    .getSurfaceHeight    = grrGetSurfaceHeight,
    .surfaceResize       = grrSurfaceResize,
    .surfaceFree         = grrSurfaceFree,
};

Renderer* GRRRenderer_create()
{
    GRRRenderer* grr = safeCalloc(1, sizeof(GRRRenderer));
    grr->base.vtable        = &grrVtable;
    grr->base.drawColor     = 0xFFFFFF;
    grr->base.drawAlpha     = 1.0f;
    grr->base.drawFont      = -1;
    grr->base.drawHalign    = 0;
    grr->base.drawValign    = 0;
    grr->base.circlePrecision = 24;
    memset(grr->surfaceStack, -1, sizeof(grr->surfaceStack));
    return (Renderer*)grr;
}
