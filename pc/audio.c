// SDL output for the portable m4a mixer.

#include <SDL.h>
#include "global.h"

static SDL_AudioDeviceID sDev;
static int sRate;

void AudioInit(void)
{
}

void AudioPush(const s16 *stereo, int frames, int rate)
{
    if (frames <= 0 || rate <= 0)
        return;

    if (!sDev || sRate != rate)
    {
        if (sDev)
            SDL_CloseAudioDevice(sDev);
        SDL_AudioSpec w = {0};
        w.freq = rate;
        w.format = AUDIO_S16SYS;
        w.channels = 2;
        w.samples = 256;
        sDev = SDL_OpenAudioDevice(NULL, 0, &w, NULL, 0);
        sRate = rate;
        if (sDev)
            SDL_PauseAudioDevice(sDev, 0);
    }
    if (!sDev)
        return;

    // ponytail: drop if the queue is already ~150ms; keeps F-hold from exploding latency
    if (SDL_GetQueuedAudioSize(sDev) > (u32)(rate * 4 / 6))
        return;
    SDL_QueueAudio(sDev, stereo, (u32)frames * 4);
}
