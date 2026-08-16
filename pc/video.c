// Minimal GBA PPU: text BGs, affine BGs, 1D sprites, windows. No blend/mosaic.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "global.h"

#define FB_W 240
#define FB_H 160

static const u8 sSprW[3][4] = {
    { 8, 16, 32, 64 },
    { 16, 32, 32, 64 },
    { 8,  8, 16, 32 },
};
static const u8 sSprH[3][4] = {
    { 8, 16, 32, 64 },
    { 8,  8, 16, 32 },
    { 16, 32, 32, 64 },
};

static u16 sLine[FB_W];
static u8 sRgb[FB_H][FB_W][3];
static s32 sAffX[2], sAffY[2]; // latched BG2/BG3 refs, 24.8

static u16 Pal(u32 i)
{
    return ((u16 *)PLTT)[i & 0x1FF];
}

static void Put(int x, int palIndex, u32 palBase)
{
    if (x < 0 || x >= FB_W || palIndex == 0)
        return;
    sLine[x] = Pal(palBase + palIndex);
}

static u16 *MapEntry(u16 cnt, int tx, int ty)
{
    u32 base = BG_SCREEN_ADDR((cnt >> 8) & 31);
    int size = cnt >> 14;
    int screen = 0;

    if (size == 1)
        screen = (tx >> 5) & 1;
    else if (size == 2)
        screen = (ty >> 5) & 1;
    else if (size == 3)
        screen = ((ty >> 5) & 1) * 2 + ((tx >> 5) & 1);

    return (u16 *)(base + screen * 0x800 + ((ty & 31) * 32 + (tx & 31)) * 2);
}

// BG 8bpp tiles are 64 bytes. OBJ tile numbers are always 32-byte units.
static int TilePix(u32 charBase, int tile, int px, int py, int bpp8, int obj)
{
    u32 off = bpp8 ? (tile * (obj ? 32 : 64) + py * 8 + px)
                   : (tile * 32 + py * 4 + (px >> 1));
    u32 addr = charBase + off;

    if (addr < VRAM || addr >= VRAM + VRAM_SIZE)
        return 0;

    if (bpp8)
        return *(u8 *)addr;

    u8 b = *(u8 *)addr;
    return (px & 1) ? (b >> 4) : (b & 0xF);
}

static int InWin(int x, int y, u16 h, u16 v)
{
    int l = h >> 8, r = h & 0xFF, t = v >> 8, b = v & 0xFF;
    return x >= l && x < r && y >= t && y < b;
}

// Bits: BG0-3, OBJ. No windows → everything draws.
static int WinAllows(int x, int y, int bit)
{
    u16 dispcnt = REG_DISPCNT;

    if (!(dispcnt & (DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON)))
        return 1;

    if ((dispcnt & DISPCNT_WIN0_ON) && InWin(x, y, REG_WIN0H, REG_WIN0V))
        return (REG_WININ >> bit) & 1;

    if ((dispcnt & DISPCNT_WIN1_ON) && InWin(x, y, REG_WIN1H, REG_WIN1V))
        return (REG_WININ >> (8 + bit)) & 1;

    return (REG_WINOUT >> bit) & 1;
}

static void DrawTextBG(int bg, int y)
{
    u16 cnt = *(vu16 *)(REG_ADDR_BG0CNT + bg * 2);
    u16 hofs = *(vu16 *)(REG_ADDR_BG0HOFS + bg * 4) & 0x1FF;
    u16 vofs = *(vu16 *)(REG_ADDR_BG0VOFS + bg * 4) & 0x1FF;
    int bpp8 = cnt & BGCNT_256COLOR;
    u32 charBase = BG_CHAR_ADDR((cnt >> 2) & 3);
    int mapY = y + vofs;

    for (int x = 0; x < FB_W; x++)
    {
        if (!WinAllows(x, y, bg))
            continue;

        int mapX = x + hofs;
        u16 entry = *MapEntry(cnt, mapX >> 3, mapY >> 3);
        int tile = entry & 0x3FF;
        int px = mapX & 7;
        int py = mapY & 7;

        if (entry & 0x400)
            px = 7 - px;
        if (entry & 0x800)
            py = 7 - py;

        int idx = TilePix(charBase, tile, px, py, bpp8, 0);
        Put(x, idx, bpp8 ? 0 : ((entry >> 12) & 0xF) * 16);
    }
}

static s32 Aff28(u32 v)
{
    return ((s32)(v << 4)) >> 4;
}

static int Wrap(int v, int size)
{
    v %= size;
    return v < 0 ? v + size : v;
}

static void DrawAffineBG(int bg, int y)
{
    u16 cnt = *(vu16 *)(REG_ADDR_BG0CNT + bg * 2);
    u32 paBase = (bg == 2) ? REG_ADDR_BG2PA : REG_ADDR_BG3PA;
    s16 pa = *(vs16 *)(paBase);
    s16 pc = *(vs16 *)(paBase + 4);
    int slot = bg - 2;
    s32 fx = sAffX[slot];
    s32 fy = sAffY[slot];
    int tiles = 16 << (cnt >> 14);
    int pix = tiles * 8;
    int wrap = cnt & BGCNT_WRAP;
    u32 charBase = BG_CHAR_ADDR((cnt >> 2) & 3);
    u8 *map = (u8 *)BG_SCREEN_ADDR((cnt >> 8) & 31);

    for (int x = 0; x < FB_W; x++)
    {
        if (WinAllows(x, y, bg))
        {
            int tx = fx >> 8;
            int ty = fy >> 8;

            if (wrap)
            {
                tx = Wrap(tx, pix);
                ty = Wrap(ty, pix);
            }

            if (tx >= 0 && ty >= 0 && tx < pix && ty < pix)
            {
                int tile = map[(ty >> 3) * tiles + (tx >> 3)];
                Put(x, TilePix(charBase, tile, tx & 7, ty & 7, 1, 0), 0);
            }
        }

        fx += pa;
        fy += pc;
    }
}

static void DrawSprites(int pri, int y)
{
    struct OamData *oam = (struct OamData *)OAM;
    int oneD = REG_DISPCNT & DISPCNT_OBJ_1D_MAP;

    for (int i = 127; i >= 0; i--)
    {
        struct OamData s = oam[i];

        if (s.affineMode == ST_OAM_AFFINE_ERASE || s.priority != pri || s.shape > 2)
            continue;

        int w = sSprW[s.shape][s.size];
        int h = sSprH[s.shape][s.size];
        int sy = s.y;
        int sx = s.x;

        if (sy > 160)
            sy -= 256;
        if (sx > 240)
            sx -= 512;

        int ly = y - sy;
        if (ly < 0 || ly >= h)
            continue;

        int hflip = !s.affineMode && (s.matrixNum & ST_OAM_HFLIP);
        int vflip = !s.affineMode && (s.matrixNum & ST_OAM_VFLIP);
        int bpp8 = s.bpp;
        int row = vflip ? (h - 1 - ly) : ly;
        int tileRow = row >> 3;
        int py = row & 7;
        int tilesAcross = w >> 3;
        int rowStride = oneD ? (bpp8 ? tilesAcross * 2 : tilesAcross) : (bpp8 ? 16 : 32);

        for (int x = 0; x < w; x++)
        {
            int fx = sx + x;
            int col = hflip ? (w - 1 - x) : x;
            int tileCol = col >> 3;
            int px = col & 7;
            int tile = s.tileNum + tileRow * rowStride + (bpp8 ? tileCol * 2 : tileCol);
            if (!WinAllows(fx, y, 4))
                continue;
            int idx = TilePix(OBJ_VRAM0, tile, px, py, bpp8, 1);
            Put(fx, idx, 256 + (bpp8 ? 0 : s.paletteNum * 16));
        }
    }
}

void VideoScanline(int y)
{
    u16 dispcnt = REG_DISPCNT;
    u16 backdrop = Pal(0);

    if (dispcnt & DISPCNT_FORCED_BLANK)
        backdrop = 0x7FFF;

    for (int x = 0; x < FB_W; x++)
        sLine[x] = backdrop;

    int mode = dispcnt & 7;

    if (y == 0)
    {
        sAffX[0] = Aff28(REG_BG2X);
        sAffY[0] = Aff28(REG_BG2Y);
        sAffX[1] = Aff28(REG_BG3X);
        sAffY[1] = Aff28(REG_BG3Y);
    }

    if (mode <= 2 && !(dispcnt & DISPCNT_FORCED_BLANK))
    {
        for (int pri = 3; pri >= 0; pri--)
        {
            for (int bg = 3; bg >= 0; bg--)
            {
                if (!(dispcnt & (DISPCNT_BG0_ON << bg)))
                    continue;
                if ((*(vu16 *)(REG_ADDR_BG0CNT + bg * 2) & 3) != pri)
                    continue;

                int affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
                if (mode == 2 && bg < 2)
                    continue;

                if (affine)
                    DrawAffineBG(bg, y);
                else
                    DrawTextBG(bg, y);
            }

            if (dispcnt & DISPCNT_OBJ_ON)
                DrawSprites(pri, y);
        }

        sAffX[0] += (s16)REG_BG2PB;
        sAffY[0] += (s16)REG_BG2PD;
        sAffX[1] += (s16)REG_BG3PB;
        sAffY[1] += (s16)REG_BG3PD;
    }
    else if (mode == 3 && !(dispcnt & DISPCNT_FORCED_BLANK))
    {
        memcpy(sLine, (u16 *)VRAM + y * FB_W, FB_W * 2);
    }
    else if (mode == 4 && !(dispcnt & DISPCNT_FORCED_BLANK))
    {
        u8 *src = (u8 *)VRAM + ((dispcnt & 0x10) ? 0xA000 : 0) + y * FB_W;
        for (int x = 0; x < FB_W; x++)
            sLine[x] = Pal(src[x]);
    }

    for (int x = 0; x < FB_W; x++)
    {
        u16 c = sLine[x];
        sRgb[y][x][0] = ((c & 31) * 255) / 31;
        sRgb[y][x][1] = (((c >> 5) & 31) * 255) / 31;
        sRgb[y][x][2] = (((c >> 10) & 31) * 255) / 31;
    }
}

static SDL_Window *sWin;
static SDL_Renderer *sRen;
static SDL_Texture *sTex;

static void PollInput(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_QUIT)
            exit(0);
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
            exit(0);
    }

    const Uint8 *k = SDL_GetKeyboardState(NULL);
    u16 keys = KEYS_MASK;

    if (k[SDL_SCANCODE_Z]) keys &= ~A_BUTTON;
    if (k[SDL_SCANCODE_X]) keys &= ~B_BUTTON;
    if (k[SDL_SCANCODE_BACKSPACE]) keys &= ~SELECT_BUTTON;
    if (k[SDL_SCANCODE_RETURN]) keys &= ~START_BUTTON;
    if (k[SDL_SCANCODE_RIGHT]) keys &= ~DPAD_RIGHT;
    if (k[SDL_SCANCODE_LEFT]) keys &= ~DPAD_LEFT;
    if (k[SDL_SCANCODE_UP]) keys &= ~DPAD_UP;
    if (k[SDL_SCANCODE_DOWN]) keys &= ~DPAD_DOWN;
    if (k[SDL_SCANCODE_S]) keys &= ~R_BUTTON;
    if (k[SDL_SCANCODE_A]) keys &= ~L_BUTTON;
    REG_KEYINPUT = keys;
}

void VideoPresent(void)
{
    PollInput();
    SDL_UpdateTexture(sTex, NULL, sRgb, FB_W * 3);
    SDL_RenderClear(sRen);
    SDL_RenderCopy(sRen, sTex, NULL, NULL);
    SDL_RenderPresent(sRen);
}

void VideoInit(void)
{
    memset(sRgb, 0, sizeof(sRgb));
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        PortableFatal(SDL_GetError());
    sWin = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 720, 480, SDL_WINDOW_RESIZABLE);
    sRen = SDL_CreateRenderer(sWin, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sWin || !sRen)
        PortableFatal(SDL_GetError());
    SDL_RenderSetLogicalSize(sRen, FB_W, FB_H);
    sTex = SDL_CreateTexture(sRen, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    if (!sTex)
        PortableFatal(SDL_GetError());
}
