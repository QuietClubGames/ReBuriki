// Audio output via the Windows waveOut API (part of Windows, no extra libraries), plus an
// optional WAV recording for checking the sound without speakers.
//
// The emulator produces audio in step with emulated time (one frame's worth per vblank), and
// the window is paced to the real frame rate, so playback just queues buffers. If the queue
// gets too far ahead (e.g. running unthrottled) old audio is dropped rather than lagging.
#include "audio_out.h"
#include "frontend/frontend.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

FILE* g_wav = nullptr;
uint32_t g_wav_frames = 0;
int g_rate = 44100;

#ifdef _WIN32
HWAVEOUT g_wo = nullptr;
struct Buf { WAVEHDR hdr; std::vector<int16_t> data; };
std::deque<Buf*> g_queued;
std::vector<int16_t> g_pending;
constexpr int CHUNK_FRAMES = 735;          // ~1/60 s per buffer
constexpr size_t MAX_QUEUED = 8;           // ~130 ms of latency at most

void reclaim() {
    while (!g_queued.empty() && (g_queued.front()->hdr.dwFlags & WHDR_DONE)) {
        Buf* b = g_queued.front();
        g_queued.pop_front();
        waveOutUnprepareHeader(g_wo, &b->hdr, sizeof(WAVEHDR));
        delete b;
    }
}

void submit(const int16_t* s, int frames) {
    reclaim();
    if (g_queued.size() >= MAX_QUEUED) return;   // running ahead of real time: drop
    Buf* b = new Buf;
    b->data.assign(s, s + frames * 2);
    // volume from the menu (perceptual curve); playback only, so WAV recordings stay exact
    const int vol = fe_volume();
    if (vol < 100) {
        const int v = vol * vol / 100;
        for (auto& x : b->data) x = (int16_t)(x * v / 100);
    }
    memset(&b->hdr, 0, sizeof(b->hdr));
    b->hdr.lpData = (LPSTR)b->data.data();
    b->hdr.dwBufferLength = (DWORD)(b->data.size() * sizeof(int16_t));
    waveOutPrepareHeader(g_wo, &b->hdr, sizeof(WAVEHDR));
    waveOutWrite(g_wo, &b->hdr, sizeof(WAVEHDR));
    g_queued.push_back(b);
}
#endif

void wav_header(FILE* f, uint32_t frames) {
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32((uint32_t)g_rate); u32((uint32_t)g_rate * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(frames * 4);
}

} // namespace

void audio_out_init(int sample_rate, bool play, const char* wav_path) {
    g_rate = sample_rate;
    if (wav_path) {
        g_wav = fopen(wav_path, "wb");
        if (g_wav) wav_header(g_wav, 0);
        else fprintf(stderr, "[audio] can't write %s\n", wav_path);
    }
#ifdef _WIN32
    if (play) {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = (DWORD)sample_rate;
        fmt.wBitsPerSample = 16; fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = (DWORD)sample_rate * 4;
        if (waveOutOpen(&g_wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            g_wo = nullptr;
            fprintf(stderr, "[audio] no audio device; continuing silently\n");
        }
    }
#endif
}

void audio_out_push(const int16_t* s, int frames) {
    if (g_wav) { fwrite(s, 4, (size_t)frames, g_wav); g_wav_frames += (uint32_t)frames; }
#ifdef _WIN32
    if (!g_wo) return;
    g_pending.insert(g_pending.end(), s, s + frames * 2);
    while (g_pending.size() >= (size_t)CHUNK_FRAMES * 2) {
        submit(g_pending.data(), CHUNK_FRAMES);
        g_pending.erase(g_pending.begin(), g_pending.begin() + CHUNK_FRAMES * 2);
    }
#endif
}

// The pause menu stops the sound card instead of letting it run dry (and resumes where it stopped).
void audio_out_pause(bool on) {
#ifdef _WIN32
    if (!g_wo) return;
    if (on) waveOutPause(g_wo);
    else waveOutRestart(g_wo);
#else
    (void)on;
#endif
}

void audio_out_close(void) {
    if (g_wav) {
        fseek(g_wav, 0, SEEK_SET);
        wav_header(g_wav, g_wav_frames);
        fclose(g_wav);
        g_wav = nullptr;
    }
}
