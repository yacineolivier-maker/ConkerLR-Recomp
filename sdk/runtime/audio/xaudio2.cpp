// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {
// XBE_ENREGISTRER_SON=<fichier.wav> : copie du mélange en WAV 16 bits stéréo, même son coupé (arrière-plan) ;
// l'en-tête est remis à jour chaque seconde, le fichier reste lisible si le processus est tué. Une ligne de journal
// par seconde : crête et niveau efficace, pour vérifier les sons sans les écouter.
struct Recorder {
    FILE* f = nullptr;
    uint32_t bytes = 0, frames = 0, seconds = 0;
    float peak = 0;
    double sum = 0;
    void Header() {
        auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
        fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f), u32(36 + bytes), fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(1), u16(2);
        u32(kRate), u32(kRate * 4), u16(4), u16(16), fwrite("data", 1, 4, f), u32(bytes);
        fseek(f, 0, SEEK_END);
        fflush(f);
    }
    void Open() {
        const char* path = std::getenv("XBE_ENREGISTRER_SON");
        if (!path || !*path) return;
        f = std::fopen(path, "wb");
        if (!f) {
            LOGE("audio: enregistrement impossible dans %s", path);
            return;
        }
        Header();
        LOGI("audio: enregistrement du mélange dans %s", path);
    }
    void Add(const float* b, uint32_t n) {
        if (!f) return;
        std::vector<int16_t> pcm(n * 2);
        for (uint32_t i = 0; i < n * 2; i++) {
            float v = std::fmax(-1.0f, std::fmin(1.0f, b[i]));
            peak = std::fmax(peak, std::fabs(v));
            sum += double(v) * v;
            pcm[i] = int16_t(std::lrint(v * 32767.0f));
        }
        fwrite(pcm.data(), 2, pcm.size(), f);
        bytes += n * 4;
        frames += n;
        if (frames >= kRate) {
            double rms = std::sqrt(sum / (double(frames) * 2));
            LOGI("audio: seconde %u : crête %.3f, efficace %.1f dB", ++seconds, peak,
                 rms > 0 ? 20 * std::log10(rms) : -120.0);
            frames = 0, peak = 0, sum = 0;
            Header();
        }
    }
};
}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master);
        if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&voice, &wf);
        if (FAILED(hr)) {
            LOGE("audio: XAudio2 unavailable (%08X), no sound", unsigned(hr));
            return;
        }
        if (app::Background()) master->SetVolume(0);
        Recorder rec;
        rec.Open();
        voice->Start();
        constexpr uint32_t kFrames = kRate / 100;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 10;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * 2);
        uint32_t depth = kMinQueued;
        bool stopped = false;
        for (uint32_t n = 0;;) {
            // paused: the voice stops where it is, nothing more is mixed until the game is back
            if (app::Paused() != stopped) {
                stopped = !stopped;
                if (stopped) voice->Stop();
                else voice->Start();
            }
            if (stopped) {
                Sleep(5);
                continue;
            }
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
            if (n && !st.BuffersQueued && depth < kMaxQueued) {
                depth++;
                LOGI("audio: output starved, queueing %u ms", depth * 10);
            }
            if (st.BuffersQueued >= depth) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            rec.Add(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
