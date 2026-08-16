// C stand-in for src/m4a_1.s: sequencer, DirectSound mixer, CGB mix, SDL push.

#include <string.h>
#include "global.h"
#include "gba/m4a_internal.h"

void AudioPush(const s16 *stereo, int frames, int rate);
u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

char SoundMainRAM[0x800];

extern void *const gMPlayJumpTableTemplate[];
extern const u8 gClockTable[];
extern const s8 gDeltaEncodingTable[];

u32 umul3232H32(u32 a, u32 b)
{
    return (u32)(((u64)a * b) >> 32);
}

void MPlayJumpTableCopy(MPlayFunc *dst)
{
    memcpy(dst, gMPlayJumpTableTemplate, 36 * sizeof(void *));
}

void SoundMainBTM(void *dest)
{
    memset(dest, 0, 64);
}

void RealClearChain(void *x)
{
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;
    struct SoundChannel *next;
    struct SoundChannel *prev;

    if (!track)
        return;
    next = chan->nextChannelPointer;
    prev = chan->prevChannelPointer;
    if (prev)
        prev->nextChannelPointer = next;
    else
        track->chan = next;
    if (next)
        next->prevChannelPointer = prev;
    chan->track = NULL;
}

static u8 ReadByte(struct MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

static void ReadPtr(struct MusicPlayerTrack *track)
{
    u8 *p = track->cmdPtr;
    uintptr_t addr = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    track->cmdPtr = (u8 *)addr;
}

void ply_fine(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;

    (void)m;
    for (chan = track->chan; chan; chan = chan->nextChannelPointer)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(chan);
    }
    track->flags = 0;
}

void ply_goto(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    ReadPtr(track);
}

void ply_patt(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    if (track->patternLevel < 3)
    {
        track->patternStack[track->patternLevel++] = track->cmdPtr + 4;
        ply_goto(m, track);
    }
    else
        ply_fine(m, track);
}

void ply_pend(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    if (track->patternLevel)
        track->cmdPtr = track->patternStack[--track->patternLevel];
}

void ply_rept(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    if (*track->cmdPtr == 0)
    {
        track->cmdPtr++;
        ply_goto(m, track);
    }
    else if (++track->repN < ReadByte(track))
        ply_goto(m, track);
    else
    {
        track->repN = 0;
        track->cmdPtr += 1 + 4;
    }
}

void ply_prio(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->priority = ReadByte(track);
}

void ply_tempo(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    u16 bpm = ReadByte(track) * 2;
    m->tempoD = bpm;
    m->tempoI = (bpm * m->tempoU) / 256;
}

void ply_keysh(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->keyShift = ReadByte(track);
    track->flags |= 0xC;
}

void ply_voice(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    track->tone = m->tone[ReadByte(track)];
}

void ply_vol(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->vol = ReadByte(track);
    track->flags |= 0x3;
}

void ply_pan(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->pan = ReadByte(track) - 0x40;
    track->flags |= 0x3;
}

void ply_bend(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->bend = ReadByte(track) - 0x40;
    track->flags |= 0xC;
}

void ply_bendr(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->bendRange = ReadByte(track);
    track->flags |= 0xC;
}

void ply_lfodl(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->lfoDelay = ReadByte(track);
}

void ply_modt(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    u8 type = ReadByte(track);
    (void)m;
    if (type != track->modT)
    {
        track->modT = type;
        track->flags |= 0xF;
    }
}

void ply_tune(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->tune = ReadByte(track) - 0x40;
    track->flags |= 0xC;
}

void ply_port(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    vu8 *reg;
    (void)m;
    reg = (vu8 *)(REG_ADDR_NR10 + ReadByte(track));
    *reg = ReadByte(track);
}

void ply_lfos(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->lfoSpeed = ReadByte(track);
    if (!track->lfoSpeed)
        ClearModM(track);
}

void ply_mod(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    (void)m;
    track->mod = ReadByte(track);
    if (!track->mod)
        ClearModM(track);
}

void ply_endtie(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    u8 key = *track->cmdPtr;
    struct SoundChannel *chan;

    (void)m;
    if (key < 0x80)
    {
        track->key = key;
        track->cmdPtr++;
    }
    else
        key = track->key;

    for (chan = track->chan; chan; chan = chan->nextChannelPointer)
    {
        if ((chan->statusFlags & 0x83) && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP) && chan->midiKey == key)
        {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
            return;
        }
    }
}

static void ChnVolSet(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    s32 right = (u8)(chan->rhythmPan + 128) * chan->velocity * track->volMR / 128 / 128;
    s32 left = (u8)(127 - chan->rhythmPan) * chan->velocity * track->volML / 128 / 128;
    if (right > 255) right = 255;
    if (left > 255) left = 255;
    chan->rightVolume = (u8)right;
    chan->leftVolume = (u8)left;
}

void ply_note(u32 clock, struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    struct ToneData *inst = &track->tone;
    struct SoundChannel *chan = NULL;
    u8 key = track->key;
    u8 type = inst->type;
    s8 forcedPan = 0;
    u16 prio;
    u8 cgbType;
    s32 transposed;

    track->gateTime = gClockTable[clock];
    if (*track->cmdPtr < 0x80)
    {
        track->key = *track->cmdPtr++;
        if (*track->cmdPtr < 0x80)
        {
            track->velocity = *track->cmdPtr++;
            if (*track->cmdPtr < 0x80)
                track->gateTime += *track->cmdPtr++;
        }
    }
    key = track->key;

    if (type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
    {
        u8 idx;
        if (type & TONEDATA_TYPE_SPL)
        {
            u8 *table = *(u8 **)&inst->attack;
            idx = table[track->key];
            inst = ((struct ToneData *)inst->wav) + idx;
        }
        else
        {
            inst = ((struct ToneData *)inst->wav) + track->key;
        }
        if (inst->type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL))
            return;
        if (type & TONEDATA_TYPE_RHY)
        {
            if (inst->pan_sweep & 0x80)
                forcedPan = ((s8)(inst->pan_sweep & 0x7F) - 0x40) * 2;
            key = inst->key;
        }
    }

    prio = m->priority + track->priority;
    if (prio > 255)
        prio = 255;
    cgbType = inst->type & TONEDATA_TYPE_CGB;

    if (cgbType)
    {
        if (!si->cgbChans)
            return;
        chan = (struct SoundChannel *)(si->cgbChans + cgbType - 1);
        if ((chan->statusFlags & SOUND_CHANNEL_SF_ON) && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP))
        {
            if (chan->priority > prio || (chan->priority == prio && chan->track < track))
                return;
        }
    }
    else
    {
        u16 p = prio;
        struct MusicPlayerTrack *t = track;
        int foundStop = 0;
        int i;
        struct SoundChannel *cur = si->chans;

        for (i = 0; i < si->maxChans; i++, cur++)
        {
            if (!(cur->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                chan = cur;
                break;
            }
            if ((cur->statusFlags & SOUND_CHANNEL_SF_STOP) && !foundStop)
            {
                foundStop = 1;
                p = cur->priority;
                t = cur->track;
                chan = cur;
            }
            else if (((cur->statusFlags & SOUND_CHANNEL_SF_STOP) && foundStop)
                  || (!(cur->statusFlags & SOUND_CHANNEL_SF_STOP) && !foundStop))
            {
                if (cur->priority < p)
                {
                    p = cur->priority;
                    t = cur->track;
                    chan = cur;
                }
                else if (cur->priority == p && cur->track > t)
                {
                    t = cur->track;
                    chan = cur;
                }
                else if (cur->priority == p && cur->track == t)
                    chan = cur;
            }
        }
    }

    if (!chan)
        return;

    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan)
        track->chan->prevChannelPointer = chan;
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay)
        ClearModM(track);
    TrkVolPitSet(m, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = (u8)prio;
    chan->key = key;
    chan->rhythmPan = forcedPan;
    chan->type = inst->type;
    chan->wav = inst->wav;
    chan->attack = inst->attack;
    chan->decay = inst->decay;
    chan->sustain = inst->sustain;
    chan->release = inst->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    ChnVolSet(chan, track);

    transposed = chan->key + track->keyM;
    if (transposed < 0)
        transposed = 0;

    if (cgbType)
    {
        struct CgbChannel *cgb = (struct CgbChannel *)chan;
        cgb->length = inst->length;
        if ((inst->pan_sweep & 0x80) || (inst->pan_sweep & 0x70) == 0)
            cgb->sweep = 8;
        else
            cgb->sweep = inst->pan_sweep;
        cgb->frequency = si->MidiKeyToCgbFreq(cgbType, (u8)transposed, track->pitM);
        cgb->wavePointer = (u32 *)inst->wav;
    }
    else if (chan->wav)
        chan->frequency = MidiKeyToFreq(chan->wav, (u8)transposed, track->pitM);

    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= ~0xF;
}

void TrackStop(struct MusicPlayerInfo *m, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;
    (void)m;
    if (!(track->flags & MPT_FLG_EXIST))
        return;
    for (chan = track->chan; chan; chan = chan->nextChannelPointer)
    {
        if (chan->statusFlags)
        {
            u8 cgbType = chan->type & TONEDATA_TYPE_CGB;
            if (cgbType)
                SOUND_INFO_PTR->CgbOscOff(cgbType);
            chan->statusFlags = 0;
        }
        chan->track = NULL;
    }
    track->chan = NULL;
}

void MPlayMain(struct MusicPlayerInfo *m)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    u32 i;

    if (m->ident != ID_NUMBER)
        return;
    m->ident++;

    if (m->MPlayMainNext)
        m->MPlayMainNext(m->musicPlayerNext);

    if (m->status & MUSICPLAYER_STATUS_PAUSE)
        goto done;

    FadeOutBody(m);
    if (m->status & MUSICPLAYER_STATUS_PAUSE)
        goto done;

    m->tempoC += m->tempoI;
    while (m->tempoC >= 150)
    {
        u16 bits = 0;
        for (i = 0; i < m->trackCount; i++)
        {
            struct MusicPlayerTrack *tr = m->tracks + i;
            struct SoundChannel *chan;

            if (!(tr->flags & MPT_FLG_EXIST))
                continue;
            bits |= (1 << i);

            chan = tr->chan;
            while (chan)
            {
                struct SoundChannel *next = chan->nextChannelPointer;
                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
                    ClearChain(chan);
                else if (chan->gateTime && --chan->gateTime == 0)
                    chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                chan = next;
            }

            if (tr->flags & MPT_FLG_START)
            {
                memset(tr, 0, 0x40);
                tr->flags = MPT_FLG_EXIST;
                tr->bendRange = 2;
                tr->volX = 64;
                tr->lfoSpeed = 22;
                tr->tone.type = 1;
            }

            while (tr->wait == 0)
            {
                u8 ev = *tr->cmdPtr;
                if (ev < 0x80)
                    ev = tr->runningStatus;
                else
                {
                    tr->cmdPtr++;
                    if (ev >= 0xBD)
                        tr->runningStatus = ev;
                }

                if (ev >= 0xCF)
                    si->plynote(ev - 0xCF, m, tr);
                else if (ev >= 0xB1)
                {
                    m->cmd = ev - 0xB1;
                    si->MPlayJumpTable[m->cmd](m, tr);
                    if (tr->flags == 0)
                        goto nextTrack;
                }
                else
                    tr->wait = gClockTable[ev - 0x80];
            }
            tr->wait--;

            if (tr->lfoSpeed && tr->mod)
            {
                s8 r;
                if (tr->lfoDelayC)
                {
                    tr->lfoDelayC--;
                    goto nextTrack;
                }
                tr->lfoSpeedC += tr->lfoSpeed;
                if (tr->lfoSpeedC >= 0x40 && tr->lfoSpeedC < 0xC0)
                    r = 128 - tr->lfoSpeedC;
                else if (tr->lfoSpeedC >= 0xC0)
                    r = (s8)(tr->lfoSpeedC - 256);
                else
                    r = tr->lfoSpeedC;
                r = (s8)(tr->mod * r / 64);
                if (r != tr->modM)
                {
                    tr->modM = r;
                    tr->flags |= tr->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                }
            }
        nextTrack:;
        }

        m->clock++;
        if (bits == 0)
        {
            m->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        m->status = bits;
        m->tempoC -= 150;
    }

    for (i = 0; i < m->trackCount; i++)
    {
        struct MusicPlayerTrack *tr = m->tracks + i;
        struct SoundChannel *chan;
        if (!(tr->flags & MPT_FLG_EXIST) || !(tr->flags & 0xF))
            continue;
        TrkVolPitSet(m, tr);
        for (chan = tr->chan; chan; chan = chan->nextChannelPointer)
        {
            u8 cgbType;
            if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
            {
                ClearChain(chan);
                continue;
            }
            cgbType = chan->type & TONEDATA_TYPE_CGB;
            if (tr->flags & MPT_FLG_VOLCHG)
            {
                ChnVolSet(chan, tr);
                if (cgbType)
                    ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_VOL;
            }
            if (tr->flags & MPT_FLG_PITCHG)
            {
                s32 key = chan->key + tr->keyM;
                if (key < 0)
                    key = 0;
                if (cgbType)
                {
                    ((struct CgbChannel *)chan)->frequency = si->MidiKeyToCgbFreq(cgbType, (u8)key, tr->pitM);
                    ((struct CgbChannel *)chan)->modify |= CGB_CHANNEL_MO_PIT;
                }
                else
                    if (chan->wav)
                        chan->frequency = MidiKeyToFreq(chan->wav, (u8)key, tr->pitM);
            }
        }
        tr->flags &= ~0xF;
    }

done:
    m->ident = ID_NUMBER;
}

static s8 sBdpcm[64];

static s8 Bdpcm(struct SoundChannel *chan, u32 idx)
{
    u32 block = idx >> 6;
    if (chan->dummy4 != block)
    {
        u8 *p;
        s32 s;
        int i;
        chan->dummy4 = block;
        p = (u8 *)chan->wav->data + block * 0x21;
        s = (s8)*p++;
        sBdpcm[0] = (s8)s;
        sBdpcm[1] = (s8)(s += gDeltaEncodingTable[*p++ & 0xF]);
        for (i = 2; i < 64; i += 2)
        {
            u32 t = *p++;
            sBdpcm[i] = (s8)(s += gDeltaEncodingTable[t >> 4]);
            sBdpcm[i + 1] = (s8)(s += gDeltaEncodingTable[t & 0xF]);
        }
    }
    return sBdpcm[idx & 63];
}

static int TickEnv(struct SoundChannel *chan)
{
    u8 st = chan->statusFlags;
    u8 env;

    if ((st & 0xC7) == 0)
        return 0;

    if (st & SOUND_CHANNEL_SF_START)
    {
        if (st & SOUND_CHANNEL_SF_STOP)
        {
            chan->statusFlags = 0;
            return 0;
        }
        chan->statusFlags = SOUND_CHANNEL_SF_ENV_ATTACK;
        if (chan->wav && (*((u8 *)chan->wav + 3) & 0xC0))
            chan->statusFlags |= SOUND_CHANNEL_SF_LOOP;
        if (chan->wav && chan->wav->type)
            chan->currentPointer = (s8 *)(uintptr_t)0;
        else
            chan->currentPointer = chan->wav ? chan->wav->data : NULL;
        chan->count = chan->wav ? chan->wav->size : 0;
        chan->fw = 0;
        chan->dummy4 = 0xFFFFFFFF;
        chan->envelopeVolume = 0;
        st = chan->statusFlags;
    }

    env = chan->envelopeVolume;
    if (st & SOUND_CHANNEL_SF_IEC)
    {
        if (--chan->pseudoEchoLength == 0)
        {
            chan->statusFlags = 0;
            return 0;
        }
        return 1;
    }
    if (st & SOUND_CHANNEL_SF_STOP)
    {
        chan->envelopeVolume = (u8)(env * chan->release / 256);
        if (chan->envelopeVolume > chan->pseudoEchoVolume)
            return 1;
        if (chan->pseudoEchoVolume == 0)
        {
            chan->statusFlags = 0;
            return 0;
        }
        chan->statusFlags |= SOUND_CHANNEL_SF_IEC;
        return 1;
    }

    switch (st & SOUND_CHANNEL_SF_ENV)
    {
    case SOUND_CHANNEL_SF_ENV_DECAY:
        chan->envelopeVolume = (u8)(env * chan->decay / 256);
        if (chan->envelopeVolume <= chan->sustain)
        {
            if (chan->sustain == 0)
            {
                if (chan->pseudoEchoVolume == 0)
                {
                    chan->statusFlags = 0;
                    return 0;
                }
                chan->statusFlags |= SOUND_CHANNEL_SF_IEC;
                return 1;
            }
            chan->envelopeVolume = chan->sustain;
            chan->statusFlags--;
        }
        break;
    case SOUND_CHANNEL_SF_ENV_ATTACK:
    {
        u16 n = env + chan->attack;
        if (n > 255)
        {
            chan->envelopeVolume = 255;
            chan->statusFlags--;
        }
        else
            chan->envelopeVolume = (u8)n;
        break;
    }
    default:
        break;
    }
    return 1;
}

static s16 Sat16(s32 v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (s16)v;
}

static void MixChan(struct SoundInfo *si, struct SoundChannel *chan, s16 *out, int n)
{
    struct WaveData *wav = chan->wav;
    u32 v = chan->envelopeVolume * (si->masterVolume + 1) / 16;
    s32 envR = chan->rightVolume * v / 256;
    s32 envL = chan->leftVolume * v / 256;
    s32 loopLen = 0;
    s8 *loopStart = NULL;
    s32 left = (s32)chan->count;
    float pos = chan->fw / 65536.0f;
    float step;
    int i;
    int compressed;
    int reverse;

    if (!wav || !chan->currentPointer || envR + envL == 0)
        return;

    if (chan->statusFlags & SOUND_CHANNEL_SF_LOOP)
    {
        loopStart = wav->data + wav->loopStart;
        loopLen = (s32)(wav->size - wav->loopStart);
    }

    compressed = wav->type != 0;
    reverse = (chan->type & 0x10) != 0;
    step = (chan->type & TONEDATA_TYPE_FIX) ? 1.0f : (float)chan->frequency / (float)si->pcmFreq;

    for (i = 0; i < n; i++)
    {
        s32 idx;
        s8 s;
        if (left <= 0)
        {
            if (loopLen <= 0)
            {
                chan->statusFlags = 0;
                return;
            }
            left += loopLen;
            if (compressed)
                chan->currentPointer = (s8 *)(uintptr_t)wav->loopStart;
            else
                chan->currentPointer = loopStart;
        }

        if (compressed)
        {
            idx = (s32)(uintptr_t)chan->currentPointer + (s32)pos;
            if (reverse)
                s = Bdpcm(chan, (u32)((s32)wav->size - 1 - idx));
            else
                s = Bdpcm(chan, (u32)idx);
        }
        else
        {
            idx = (s32)pos;
            if (idx >= left)
                idx = left - 1;
            if (idx < 0)
                idx = 0;
            s = chan->currentPointer[idx];
        }

        out[i * 2]     = Sat16(out[i * 2]     + s * envL * 4);
        out[i * 2 + 1] = Sat16(out[i * 2 + 1] + s * envR * 4);

        pos += step;
        if (pos >= 1.0f)
        {
            u32 adv = (u32)pos;
            pos -= (float)adv;
            left -= (s32)adv;
            if (compressed)
                chan->currentPointer = (s8 *)((uintptr_t)chan->currentPointer + adv);
            else
                chan->currentPointer += adv;
        }
    }
    chan->fw = (u32)(pos * 65536.0f);
    chan->count = (u32)(left < 0 ? 0 : left);
}

static void MixCgb(struct SoundInfo *si, s16 *out, int n)
{
    static float phase[4];
    static u16 lfsr = 0x7FFF;
    struct CgbChannel *ch = si->cgbChans;
    int c, i;

    if (!ch)
        return;

    for (c = 0; c < 4; c++)
    {
        int vol = ch[c].envelopeVolume;
        int period;
        float hz, step;
        int pan;
        if (!(ch[c].statusFlags & SOUND_CHANNEL_SF_ON) || vol <= 0)
            continue;
        pan = ch[c].pan ? ch[c].pan : 0xFF;
        period = (int)(ch[c].frequency & 0x7FF);

        if (c == 3)
        {
            int shift = (ch[c].frequency >> 4) & 0xF;
            int div = ch[c].frequency & 7;
            hz = 524288.0f / ((div ? div : 0.5f) * (float)(1 << (shift + 1)));
            step = hz / (float)si->pcmFreq;
            for (i = 0; i < n; i++)
            {
                s16 s;
                phase[c] += step;
                while (phase[c] >= 1.0f)
                {
                    u16 bit = (lfsr ^ (lfsr >> 1)) & 1;
                    lfsr = (u16)((lfsr >> 1) | (bit << 14));
                    phase[c] -= 1.0f;
                }
                s = (s16)((lfsr & 1) ? vol * 400 : -vol * 400);
                if (pan & 0xF0) out[i * 2]     = Sat16(out[i * 2] + s);
                if (pan & 0x0F) out[i * 2 + 1] = Sat16(out[i * 2 + 1] + s);
            }
            continue;
        }

        if (period >= 2047)
            continue;
        hz = 131072.0f / (float)(2048 - period);
        step = hz / (float)si->pcmFreq;
        if (c == 2)
        {
            u8 *ram = (u8 *)REG_ADDR_WAVE_RAM0;
            for (i = 0; i < n; i++)
            {
                int idx;
                int nib;
                s16 s;
                phase[c] += step * 32.0f;
                while (phase[c] >= 32.0f)
                    phase[c] -= 32.0f;
                idx = (int)phase[c];
                nib = (idx & 1) ? (ram[idx >> 1] & 0xF) : (ram[idx >> 1] >> 4);
                s = (s16)((nib - 8) * vol * 80);
                if (pan & 0xF0) out[i * 2]     = Sat16(out[i * 2] + s);
                if (pan & 0x0F) out[i * 2 + 1] = Sat16(out[i * 2 + 1] + s);
            }
        }
        else
        {
            int duty = ((uintptr_t)ch[c].wavePointer) & 3;
            static const int th[4] = {4, 8, 16, 24};
            for (i = 0; i < n; i++)
            {
                int idx;
                s16 s;
                phase[c] += step * 32.0f;
                while (phase[c] >= 32.0f)
                    phase[c] -= 32.0f;
                idx = (int)phase[c];
                s = (s16)((idx < th[duty] ? vol : -vol) * 400);
                if (pan & 0xF0) out[i * 2]     = Sat16(out[i * 2] + s);
                if (pan & 0x0F) out[i * 2 + 1] = Sat16(out[i * 2 + 1] + s);
            }
        }
    }
}

void SoundMain(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    static s16 out[704 * 2];
    int n;
    int i;

    if (!si || si->ident != ID_NUMBER)
        return;
    si->ident++;

    if (si->MPlayMainHead && si->musicPlayerHead)
        si->MPlayMainHead(si->musicPlayerHead);

    if (si->CgbSound)
        si->CgbSound();

    n = si->pcmSamplesPerVBlank;
    if (n < 1)
        n = 1;
    if (n > 704)
        n = 704;
    memset(out, 0, (size_t)n * 4);

    for (i = 0; i < si->maxChans; i++)
    {
        struct SoundChannel *chan = &si->chans[i];
        if ((chan->type & TONEDATA_TYPE_CGB) == 0 && TickEnv(chan))
            MixChan(si, chan, out, n);
    }
    MixCgb(si, out, n);
    AudioPush(out, n, si->pcmFreq > 0 ? si->pcmFreq : 13379);

    si->ident = ID_NUMBER;
}

void m4aSoundVSync(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;
    if (!si)
        return;
    if ((s8)(--si->pcmDmaCounter) <= 0)
        si->pcmDmaCounter = si->pcmDmaPeriod;
}
