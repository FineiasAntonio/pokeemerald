// Host layer for the native x86-32 build.
//
// Three jobs: map the GBA address space at its hardware addresses so the game's
// pointers keep working untouched, implement the BIOS calls in plain C, and
// drive a frame loop that stands in for the hardware interrupts.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <fcntl.h>

#include "global.h"
#include "main.h"
#include "gba/m4a_internal.h"

void VideoPoll(void);
int VideoFastForward(void);

#undef CpuSet
#undef CpuFastSet

// Indices into gIntrTable, which follows the hardware priority order set up by
// gIntrTableTemplate in main.c.
#define INTR_INDEX_VCOUNT 0
#define INTR_INDEX_HBLANK 3
#define INTR_INDEX_VBLANK 4

#define SCANLINES_VISIBLE 160
#define SCANLINES_TOTAL   228

#define GPIO_REGION_START  0x08000000 // cartridge header and the RTC's GPIO pins
#define GPIO_REGION_SIZE   0x10000
#define FLASH_REGION_START 0x0E000000
#define FLASH_REGION_SIZE  0x20000

static u32 sFrameCount;
static u32 sFrameLimit;
int gPortableSkipToNewGame;

void AgbMain(void);

// Stands in for the ARM interrupt dispatcher that crt0 copies into IWRAM. The
// game copies these bytes but never executes them here; PortableRunFrame calls
// the handlers directly.
u8 IntrMain[0x800];

static void MapAddressSpace(void)
{
    static const struct { u32 addr; u32 size; const char *name; } regions[] = {
        { EWRAM_START,       0x40000,           "EWRAM" },
        { IWRAM_START,       0x8000,            "IWRAM" },
        { REG_BASE,          0x400,             "IO"    },
        { PLTT,              PLTT_SIZE,         "PLTT"  },
        { VRAM,              VRAM_SIZE,         "VRAM"  },
        { OAM,               OAM_SIZE,          "OAM"   },
        { GPIO_REGION_START,  GPIO_REGION_SIZE,  "GPIO"  },
    };

    for (u32 i = 0; i < ARRAY_COUNT(regions); i++)
    {
        void *p = mmap((void *)regions[i].addr, regions[i].size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

        if (p == MAP_FAILED)
        {
            fprintf(stderr, "could not map %s at %08x\n", regions[i].name, regions[i].addr);
            exit(1);
        }
    }

    int fd = open("pokeemerald.sav", O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, FLASH_REGION_SIZE) != 0)
    {
        fprintf(stderr, "could not open pokeemerald.sav\n");
        exit(1);
    }
    void *flash = mmap((void *)FLASH_REGION_START, FLASH_REGION_SIZE,
                       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    close(fd);
    if (flash == MAP_FAILED)
    {
        fprintf(stderr, "could not map FLASH at %08x\n", FLASH_REGION_START);
        exit(1);
    }
}

void PortableFatal(const char *msg)
{
    fprintf(stderr, "fatal: %s (frame %u)\n", msg, sFrameCount);
    exit(1);
}

static void OnCrash(int sig)
{
    void *bt[32];
    int n = backtrace(bt, 32);

    fprintf(stderr, "crash signal %d frame %u\n", sig, sFrameCount);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

static void CallIntr(u32 index)
{
    if (REG_IME && gIntrTable[index])
        gIntrTable[index]();
}

// Replaces waiting on a V-blank interrupt: runs one display frame's worth of
// interrupts, then returns to the game as if it had just woken up.
void PortableRunFrame(void)
{
    int draw = !VideoFastForward() || (sFrameCount & 3) == 0;

    for (u32 line = 0; line < SCANLINES_TOTAL; line++)
    {
        REG_VCOUNT = line;

        if (line == 0)
            REG_DISPSTAT &= ~DISPSTAT_VBLANK;

        if (line < SCANLINES_VISIBLE && (REG_IE & INTR_FLAG_HBLANK))
            CallIntr(INTR_INDEX_HBLANK);

        if (line < SCANLINES_VISIBLE && draw)
            VideoScanline(line);

        if (line == (REG_DISPSTAT >> 8) && (REG_IE & INTR_FLAG_VCOUNT))
            CallIntr(INTR_INDEX_VCOUNT);

        if (line == SCANLINES_VISIBLE)
        {
            REG_DISPSTAT |= DISPSTAT_VBLANK;

            if (REG_IE & INTR_FLAG_VBLANK)
                CallIntr(INTR_INDEX_VBLANK);
        }
    }

    // Game logic runs after WaitForVBlank, i.e. during vblank. SetGpuReg
    // writes through immediately only when VCOUNT is in 161..225.
    REG_VCOUNT = 200;

    sFrameCount++;
    if (draw)
        VideoPresent();
    else
        VideoPoll();

    if (sFrameLimit && sFrameCount >= sFrameLimit)
    {
        printf("ran %u frames\n", sFrameCount);
        exit(0);
    }
}

// Replaces the DMA controller. Every DmaSet/DmaCopy/DmaFill in the game routes
// through here (see DmaSetUnchecked in include/gba/macro.h).
void PortableDma(const void *src, void *dest, u32 control)
{
    u32 flags = control >> 16;
    u32 count = control & 0xFFFF;

    if (!(flags & DMA_ENABLE))
        return;

    // ponytail: immediate transfers only. H-blank and sound-FIFO DMAs need the
    // scanline timing loop, which arrives with the PPU and the mixer.
    if ((flags & DMA_START_MASK) != DMA_START_NOW)
        return;

    if (count == 0)
        count = 0x10000;

    u32 unit = (flags & DMA_32BIT) ? 4 : 2;

    if (flags & DMA_SRC_FIXED)
    {
        for (u32 i = 0; i < count; i++)
            memcpy((u8 *)dest + i * unit, src, unit);
    }
    else
    {
        memcpy(dest, src, count * unit);
    }
}

// BIOS calls.

s32 Div(s32 num, s32 denom)
{
    return num / denom;
}

u16 Sqrt(u32 num)
{
    return (u16)sqrt((double)num);
}

u16 ArcTan2(s16 x, s16 y)
{
    // The BIOS returns a full turn as 0x10000, measured counter-clockwise.
    return (u16)(atan2((double)y, (double)x) * 0x8000 / M_PI);
}

void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    u32 unit = (control & CPU_SET_32BIT) ? 4 : 2;

    if (control & CPU_SET_SRC_FIXED)
    {
        for (u32 i = 0; i < count; i++)
            memcpy((u8 *)dest + i * unit, src, unit);
    }
    else
    {
        memcpy(dest, src, count * unit);
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;

    if (control & CPU_FAST_SET_SRC_FIXED)
    {
        for (u32 i = 0; i < count; i++)
            memcpy((u8 *)dest + i * 4, src, 4);
    }
    else
    {
        memcpy(dest, src, count * 4);
    }
}

static void LZ77UnComp(const u32 *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = dest;
    u32 remaining = (in[1] | (in[2] << 8) | (in[3] << 16));

    in += 4;

    while (remaining)
    {
        u8 flags = *in++;

        for (u32 bit = 0; bit < 8 && remaining; bit++)
        {
            if (flags & (0x80 >> bit))
            {
                u32 length = (in[0] >> 4) + 3;
                u32 offset = (((in[0] & 0xF) << 8) | in[1]) + 1;

                in += 2;

                if (length > remaining)
                    length = remaining;

                for (u32 i = 0; i < length; i++, out++)
                    *out = *(out - offset);

                remaining -= length;
            }
            else
            {
                *out++ = *in++;
                remaining--;
            }
        }
    }
}

void LZ77UnCompWram(const u32 *src, void *dest)
{
    LZ77UnComp(src, dest);
}

void LZ77UnCompVram(const u32 *src, void *dest)
{
    LZ77UnComp(src, dest);
}

static void RLUnComp(const u32 *src, void *dest)
{
    const u8 *in = (const u8 *)src;
    u8 *out = dest;
    u32 remaining = (in[1] | (in[2] << 8) | (in[3] << 16));

    in += 4;

    while (remaining)
    {
        u8 flags = *in++;
        u32 length = (flags & 0x7F) + ((flags & 0x80) ? 3 : 1);

        if (length > remaining)
            length = remaining;

        if (flags & 0x80)
        {
            memset(out, *in++, length);
        }
        else
        {
            memcpy(out, in, length);
            in += length;
        }

        out += length;
        remaining -= length;
    }
}

void RLUnCompWram(const u32 *src, void *dest)
{
    RLUnComp(src, dest);
}

void RLUnCompVram(const u32 *src, void *dest)
{
    RLUnComp(src, dest);
}

// ponytail: naive trigonometry, accurate to within a unit or two of the BIOS.
// The PPU work will show whether sprites need the exact BIOS rounding.
void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    s16 *out = dest;
    s32 stride = offset / 2; // callers give the gap between elements in bytes

    for (s32 i = 0; i < count; i++)
    {
        double angle = src[i].rotation * 2.0 * M_PI / 0x10000;
        double sinA = sin(angle);
        double cosA = cos(angle);

        out[0] = (s16)(src[i].xScale * cosA);
        out[stride] = (s16)(-src[i].xScale * sinA);
        out[stride * 2] = (s16)(src[i].yScale * sinA);
        out[stride * 3] = (s16)(src[i].yScale * cosA);

        out += stride * 4;
    }
}

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    for (s32 i = 0; i < count; i++)
    {
        double angle = src[i].alpha * 2.0 * M_PI / 0x10000;
        double sinA = sin(angle);
        double cosA = cos(angle);

        dest[i].pa = (s16)(src[i].sx * cosA);
        dest[i].pb = (s16)(-src[i].sx * sinA);
        dest[i].pc = (s16)(src[i].sy * sinA);
        dest[i].pd = (s16)(src[i].sy * cosA);

        dest[i].dx = src[i].texX - (src[i].scrX * dest[i].pa + src[i].scrY * dest[i].pb);
        dest[i].dy = src[i].texY - (src[i].scrX * dest[i].pc + src[i].scrY * dest[i].pd);
    }
}

// The game's own variables live in the executable's .bss, not in the mapped
// work RAM, so only the display memory is worth clearing here.
void RegisterRamReset(u32 resetFlags)
{
    if (resetFlags & RESET_PALETTE)
        memset((void *)PLTT, 0, PLTT_SIZE);

    if (resetFlags & RESET_VRAM)
        memset((void *)VRAM, 0, VRAM_SIZE);

    if (resetFlags & RESET_OAM)
        memset((void *)OAM, 0, OAM_SIZE);
}

void SoftReset(u32 resetFlags)
{
    printf("soft reset after %u frames\n", sFrameCount);
    exit(0);
}

void VBlankIntrWait(void)
{
    PortableRunFrame();
}

int MultiBoot(struct MultiBootParam *mp)
{
    return 1; // no link cable to boot over
}

// GameCube link boot, unreachable without a cable.

void GameCubeMultiBoot_Init(void *mb) {}
void GameCubeMultiBoot_HandleSerialInterrupt(void *mb) {}
void GameCubeMultiBoot_Main(void *mb) {}
void GameCubeMultiBoot_ExecuteProgram(void *mb) {}
void GameCubeMultiBoot_Quit(void) {}

int main(int argc, char **argv)
{
    sFrameLimit = (argc > 1) ? (u32)strtoul(argv[1], NULL, 10) : 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGSEGV, OnCrash);
    signal(SIGBUS, OnCrash);
    gPortableSkipToNewGame = getenv("POKE_NEWGAME") != NULL;
    MapAddressSpace();
    VideoInit();
    REG_VCOUNT = 200; // BIOS leaves the CPU in vblank; SetGpuReg checks this
    REG_KEYINPUT = KEYS_MASK; // buttons are active low, so all released

    if (sFrameLimit)
        printf("running %u frames\n", sFrameLimit);
    else
        printf("running (SDL2)\n");
    AgbMain();

    return 0;
}
