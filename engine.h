// DualAudio engine - shared by the window app (dualaudio_gui.cpp) and the test CLI (dualaudio_cli.cpp).
//
//   * The Windows DEFAULT output plays normally (Netflix/YouTube keep their own A/V sync with it).
//   * We loopback-capture that device and mirror the exact samples to a second headphone.
//   * The mirror is delayed so both headphones line up; the delay is measured automatically by
//     playing short chirps through each headphone and timing them with the laptop mic.
//   * Bluetooth clocks drift; a tiny speed trim (max 0.2 %) through a windowed-sinc resampler
//     keeps the mirror locked without clicks or any loss of treble.
//   * Audio stays 32-bit float end to end; the main headphone's path is untouched by us.
#pragma once
#include "devices.h"
#include "resampler.h"
#include <ksmedia.h>
#include <avrt.h>
#include <stdio.h>
#include <wchar.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <mutex>
#include <cmath>
#include <functional>

#pragma comment(lib, "avrt.lib")

static std::atomic<bool> g_run{false};
static WAVEFORMATEX* g_fmt = nullptr;     // mix format of the main (default) device
static bool g_isFloat = false;
static std::atomic<int> g_srcPeak{0};     // level meter of the main device, 0..100
static std::wstring g_engDefId, g_engDefName;
static std::thread g_capTh;

// ---------------------------------------------------------------- lock-free single-producer ring
struct Ring {
    std::vector<BYTE> buf;
    size_t cap = 0;
    UINT32 fs = 0;
    std::atomic<size_t> w{0}, r{0};

    void init(size_t frames, UINT32 frameSize) { cap = frames; fs = frameSize; buf.assign(frames * frameSize, 0); w = 0; r = 0; }
    size_t count() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire); }

    void write(const BYTE* src, size_t n) {  // producer; src==nullptr -> silence
        size_t wp = w.load(std::memory_order_relaxed);
        size_t freeF = cap - (wp - r.load(std::memory_order_acquire));
        if (n > freeF) n = freeF;
        for (size_t done = 0; done < n;) {
            size_t i = (wp + done) % cap, chunk = std::min(n - done, cap - i);
            if (src) memcpy(&buf[i * fs], src + done * fs, chunk * fs);
            else     memset(&buf[i * fs], 0, chunk * fs);
            done += chunk;
        }
        w.store(wp + n, std::memory_order_release);
    }
    size_t read(BYTE* dst, size_t n) {       // consumer; dst==nullptr -> discard
        size_t rp = r.load(std::memory_order_relaxed);
        size_t avail = w.load(std::memory_order_acquire) - rp;
        if (n > avail) n = avail;
        if (dst) {
            for (size_t done = 0; done < n;) {
                size_t i = (rp + done) % cap, chunk = std::min(n - done, cap - i);
                memcpy(dst + done * fs, &buf[i * fs], chunk * fs);
                done += chunk;
            }
        }
        r.store(rp + n, std::memory_order_release);
        return n;
    }
    size_t peek(BYTE* dst, size_t n) const { // consumer: copy without consuming
        size_t rp = r.load(std::memory_order_relaxed);
        size_t avail = w.load(std::memory_order_acquire) - rp;
        if (n > avail) n = avail;
        for (size_t done = 0; done < n;) {
            size_t i = (rp + done) % cap, chunk = std::min(n - done, cap - i);
            memcpy(dst + done * fs, &buf[i * fs], chunk * fs);
            done += chunk;
        }
        return n;
    }
};

struct Target {
    std::wstring id, name;
    std::atomic<int> delayMs{0};
    Ring ring;
    std::atomic<int> queueMs{0};
    std::atomic<int> outPeak{0};     // level meter of the mirror output, 0..100
    std::atomic<long> jumps{0}, gaps{0};
    std::atomic<int> ppm{0};         // current speed trim, parts per million
    std::atomic<int> state{0};       // 0 connecting, 1 playing, 2 lost
    std::thread th;
};
static std::vector<std::unique_ptr<Target>> g_targets;

// ---------------------------------------------------------------- calibration shared state
struct Rec { std::vector<float> s; std::vector<std::pair<size_t, UINT64>> stamps; UINT32 rate = 0; };
static std::mutex g_calMx;
static std::atomic<bool> g_muteMirrors{false};
static std::atomic<bool> g_recLoop{false};
static std::atomic<bool> g_calibrating{false};
static Target* g_calTarget = nullptr;
static Rec g_loopRec;
static std::vector<float> g_keeperChirp; static size_t g_keeperPos = 0;
static std::vector<float> g_injectChirp; static size_t g_injectPos = 0; static UINT64 g_injectQpc = 0;

// ---------------------------------------------------------------- helpers
static int PeakOfF(const float* f, size_t samples) {
    float m = 0;
    for (size_t i = 0; i < samples; i += 4) { float a = f[i] < 0 ? -f[i] : f[i]; if (a > m) m = a; }
    int p = (int)(m * 100); return p > 100 ? 100 : p;
}
static float SampleAt(const BYTE* p, const WAVEFORMATEX* f, size_t frame) {
    const BYTE* q = p + frame * f->nBlockAlign;
    bool isF = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
               (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE*)f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    if (isF && f->wBitsPerSample == 32) return *(const float*)q;
    if (f->wBitsPerSample == 16) return *(const short*)q / 32768.f;
    if (f->wBitsPerSample == 32) return *(const int*)q / 2147483648.f;
    if (f->wBitsPerSample == 24) { int v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); return v / 2147483648.f; }
    return 0.f;
}
static void RecAppend(Rec& r, const BYTE* src, UINT32 n, const WAVEFORMATEX* f, UINT64 qpc) {
    r.stamps.push_back({r.s.size(), qpc});
    for (UINT32 i = 0; i < n; i++) r.s.push_back(src ? SampleAt(src, f, i) : 0.f);
}

// ---------------------------------------------------------------- capture (loopback of the main device)
static void CaptureThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD ti = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &ti);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<BYTE> injBuf;

    while (g_run) {
        ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> cap, keep; ComPtr<IAudioCaptureClient> cc; ComPtr<IAudioRenderClient> kr;
        UINT32 kbuf = 0;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->GetDevice(g_engDefId.c_str(), &dev);

        // "keeper": play silence on the main device so loopback delivers a continuous clock
        // (also keeps Bluetooth from sleeping, which would change its latency)
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)keep.GetAddressOf());
        if (SUCCEEDED(hr)) hr = keep->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, g_fmt, nullptr);
        if (SUCCEEDED(hr)) hr = keep->GetBufferSize(&kbuf);
        if (SUCCEEDED(hr)) hr = keep->GetService(IID_PPV_ARGS(&kr));
        if (SUCCEEDED(hr)) hr = keep->Start();

        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)cap.GetAddressOf());
        if (SUCCEEDED(hr)) hr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, g_fmt, nullptr);
        if (SUCCEEDED(hr)) hr = cap->SetEventHandle(ev);
        if (SUCCEEDED(hr)) hr = cap->GetService(IID_PPV_ARGS(&cc));
        if (SUCCEEDED(hr)) hr = cap->Start();

        const UINT32 fs = g_fmt->nBlockAlign, ch = g_fmt->nChannels;
        while (g_run && SUCCEEDED(hr)) {
            WaitForSingleObject(ev, 5);   // event-driven, with a 5 ms fallback poll
            UINT32 pad = 0; BYTE* p = nullptr;
            if (SUCCEEDED(keep->GetCurrentPadding(&pad)) && kbuf > pad &&
                SUCCEEDED(kr->GetBuffer(kbuf - pad, &p))) {
                UINT32 n = kbuf - pad; bool wrote = false;
                {
                    std::lock_guard<std::mutex> lk(g_calMx);
                    if (g_isFloat && g_keeperPos < g_keeperChirp.size()) {   // calibration chirp -> main device
                        float* f = (float*)p;
                        for (UINT32 i = 0; i < n; i++) {
                            float v = g_keeperPos < g_keeperChirp.size() ? g_keeperChirp[g_keeperPos++] : 0.f;
                            for (UINT32 c = 0; c < ch; c++) f[i * ch + c] = v;
                        }
                        wrote = true;
                    }
                }
                kr->ReleaseBuffer(n, wrote ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
            }

            UINT32 pk = 0;
            while (SUCCEEDED(hr = cc->GetNextPacketSize(&pk)) && pk) {
                BYTE* d; UINT32 n; DWORD fl; UINT64 qpc = 0;
                hr = cc->GetBuffer(&d, &n, &fl, nullptr, &qpc);
                if (FAILED(hr)) break;
                const BYTE* src = (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : d;
                if (src && g_isFloat) { int pkv = PeakOfF((const float*)src, (size_t)n * ch); if (pkv > g_srcPeak) g_srcPeak = pkv; }
                const BYTE* toRing = g_muteMirrors ? nullptr : src;
                const BYTE* toCal = toRing;
                if (g_recLoop || g_calibrating) {
                    std::lock_guard<std::mutex> lk(g_calMx);
                    if (g_recLoop) RecAppend(g_loopRec, src, n, g_fmt, qpc);
                    if (g_isFloat && g_injectPos < g_injectChirp.size()) {    // calibration chirp -> mirror only
                        injBuf.resize((size_t)n * fs);
                        if (toRing) memcpy(injBuf.data(), toRing, (size_t)n * fs); else memset(injBuf.data(), 0, (size_t)n * fs);
                        if (g_injectPos == 0) g_injectQpc = qpc;
                        float* f = (float*)injBuf.data();
                        for (UINT32 i = 0; i < n && g_injectPos < g_injectChirp.size(); i++) {
                            float v = g_injectChirp[g_injectPos++];
                            for (UINT32 c = 0; c < ch; c++) f[i * ch + c] = v;
                        }
                        toCal = injBuf.data();
                    }
                }
                for (auto& t : g_targets) t->ring.write(t.get() == g_calTarget ? toCal : toRing, n);
                cc->ReleaseBuffer(n);
            }
        }
        if (cap) cap->Stop();
        if (keep) keep->Stop();
        for (int i = 0; i < 50 && g_run; i++) Sleep(10);   // device lost -> reopen
    }
    CloseHandle(ev);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
}

// ---------------------------------------------------------------- render to the mirror headphone
static void TargetThread(Target* t) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD ti = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &ti);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const UINT32 fs = g_fmt->nBlockAlign, rate = g_fmt->nSamplesPerSec, ch = g_fmt->nChannels;
    const long long safety = rate * 30 / 1000;   // 30 ms cushion against capture/Bluetooth burst jitter
    const long long hard = rate * 40 / 1000;     // beyond 40 ms off -> jump (with fade) instead of slewing
    const UINT32 fadeLen = rate / 200;           // 5 ms fade-in after any jump/gap
    SincResampler rs; rs.init(ch);

    while (g_run) {
        ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> ac; ComPtr<IAudioRenderClient> rc;
        UINT32 bf = 0; BYTE* out = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->GetDevice(t->id.c_str(), &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf());
        if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, g_fmt, nullptr);
        if (SUCCEEDED(hr)) hr = ac->SetEventHandle(ev);
        if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&bf);
        if (SUCCEEDED(hr)) hr = ac->GetService(IID_PPV_ARGS(&rc));
        if (SUCCEEDED(hr) && SUCCEEDED(rc->GetBuffer(bf, &out))) rc->ReleaseBuffer(bf, AUDCLNT_BUFFERFLAGS_SILENT);
        t->ring.read(nullptr, t->ring.count());   // drop stale audio
        if (SUCCEEDED(hr)) hr = ac->Start();
        double ema = -1;
        UINT32 fade = 0;

        while (g_run && SUCCEEDED(hr)) {
            t->state = 1;
            WaitForSingleObject(ev, 500);
            UINT32 pad = 0;
            if (FAILED(hr = ac->GetCurrentPadding(&pad))) break;
            UINT32 n = bf - pad;
            if (!n) continue;
            if (FAILED(hr = rc->GetBuffer(n, &out))) break;

            long long desired = (long long)t->delayMs.load() * rate / 1000 + safety;
            long long cnt = (long long)t->ring.count();
            long long diff = cnt - desired;
            DWORD flags = 0;

            if (diff < -hard || (ema < 0 && diff < 0)) {
                flags = AUDCLNT_BUFFERFLAGS_SILENT;      // filling up to the wanted delay
                if (ema >= 0) t->jumps++;
                ema = -1;
            } else if (!g_isFloat) {
                size_t got = t->ring.read(out, n);
                if (got < n) { memset(out + got * fs, 0, (n - got) * fs); t->gaps++; }
            } else {
                if (ema < 0 || diff > hard) {
                    if (diff > 0) t->ring.read(nullptr, (size_t)diff);   // snap exactly to the delay
                    if (ema >= 0) t->jumps++;
                    cnt = desired; ema = (double)cnt; fade = fadeLen;
                    rs.reset();
                }
                ema = ema * 0.98 + cnt * 0.02;
                double errMs = (ema - (double)desired) * 1000.0 / rate;
                double ratio = 1.0 + std::clamp(errMs * 0.0002, -0.002, 0.002);   // tiny speed trim
                t->ppm = (int)lround((ratio - 1.0) * 1e6);

                float* o = (float*)out;
                bool ok = rs.process(t->ring, o, n, ratio);           // windowed-sinc: no treble loss
                for (UINT32 i = 0; fade && i < n; i++, fade--) {
                    float g = 1.f - (float)fade / fadeLen;
                    for (UINT32 c = 0; c < ch; c++) o[i * ch + c] *= g;
                }
                if (!ok) { t->gaps++; ema = -1; }
                int pk = PeakOfF(o, (size_t)n * ch); if (pk > t->outPeak) t->outPeak = pk;
            }
            rc->ReleaseBuffer(n, flags);
            t->queueMs = (int)(((long long)t->ring.count() + pad) * 1000 / rate);
        }
        if (ac) ac->Stop();
        if (t->state == 1) t->state = 2;
        for (int i = 0; i < 100 && g_run; i++) Sleep(10);   // headphone gone -> keep retrying
    }
    CloseHandle(ev);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
}

// ---------------------------------------------------------------- start / stop
static Target* StartEngine(const std::wstring& mirrorId, const std::wstring& mirrorName, int delayMs) {
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> def; ComPtr<IAudioClient> ac;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) ||
        FAILED(def->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf())) ||
        FAILED(ac->GetMixFormat(&g_fmt)))
        return nullptr;
    g_engDefId = DevId(def.Get()); g_engDefName = DevName(def.Get());
    g_isFloat = false;
    if (g_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) g_isFloat = true;
    if (g_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        ((WAVEFORMATEXTENSIBLE*)g_fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) g_isFloat = true;
    g_isFloat = g_isFloat && g_fmt->wBitsPerSample == 32;

    auto t = std::make_unique<Target>();
    t->id = mirrorId; t->name = mirrorName; t->delayMs = delayMs;
    t->ring.init(g_fmt->nSamplesPerSec * 2, g_fmt->nBlockAlign);   // up to ~1.9 s of delay
    Target* raw = t.get();
    g_targets.push_back(std::move(t));
    g_run = true;
    raw->th = std::thread(TargetThread, raw);
    g_capTh = std::thread(CaptureThread);
    return raw;
}
static void StopEngine() {
    g_run = false;
    if (g_capTh.joinable()) g_capTh.join();
    for (auto& t : g_targets) if (t->th.joinable()) t->th.join();
    g_targets.clear();
    if (g_fmt) { CoTaskMemFree(g_fmt); g_fmt = nullptr; }
    g_srcPeak = 0;
}
