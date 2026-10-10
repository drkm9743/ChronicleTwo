// The sound output: SDL asks for samples, and the sound processor mixes them
// under iop::Lock(), running the IOP timers every 1/240 s of sound.

#include "../iop/iop.h"
#include "servers.h"
#include "spu2.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace pc {
namespace audio {

namespace {

const int kFramesPerTick = kSampleRate / kTickRate;

/** Frames mixed since the last timer tick. */
int frames_since_tick;

std::atomic<bool> output_disabled(false);

void SDLCALL Feed(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    (void) userdata;
    (void) total_amount;
    int                frames = additional_amount / (int) (2 * sizeof(short));
    std::vector<short> buffer(frames * 2);

    Render(buffer.data(), frames);
    SDL_PutAudioStreamData(stream, buffer.data(), frames * 2 * (int) sizeof(short));
}

/** Without an audio device: mix in real time and throw the sound away, so the servers keep time. */
void RunSilently() {
    std::vector<short> buffer(kFramesPerTick * 2);
    auto               next = std::chrono::steady_clock::now();

    for (;;) {
        Render(buffer.data(), kFramesPerTick);
        next += std::chrono::microseconds(1000000 / kTickRate);
        std::this_thread::sleep_until(next);
    }
}

} // namespace

void Render(short *out, int frames) {
    std::lock_guard<std::recursive_mutex> guard(iop::Lock());

    while (frames > 0) {
        int chunk = std::min(frames, kFramesPerTick - frames_since_tick);

        GetSpu().Mix(out, chunk);
        out += chunk * 2;
        frames -= chunk;
        frames_since_tick += chunk;

        if (frames_since_tick == kFramesPerTick) {
            frames_since_tick = 0;
            EzMidiTick();
        }
    }
}

void DisableOutput() {
    output_disabled = true;
}

void StartOutput() {
    static std::once_flag started;

    if (output_disabled) {
        return;
    }

    std::call_once(started, [] {
        SDL_AudioSpec    spec = {SDL_AUDIO_S16, 2, kSampleRate};
        SDL_AudioStream *stream = NULL;

        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, NULL);
        }

        if (stream != NULL && SDL_ResumeAudioStreamDevice(stream)) {
            return;
        }

        SDL_Log("No sound output (%s); sound is mixed and dropped", SDL_GetError());
        std::thread(RunSilently).detach();
    });
}

} // namespace audio
} // namespace pc
