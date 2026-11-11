// Automatic sync calibration: times a chirp through each headphone with the laptop mic.
#pragma once
#include "engine.h"

// ---------------------------------------------------------------- auto-calibration via laptop mic
// Plays a chirp through the main headphone only, then through the mirror only, and times each
// arrival at the laptop mic against the loopback clock. Mic latency is common to both and cancels.
static std::vector<float> MakeChirp(UINT32 rate, float amp) {
    const double dur = 0.05, f0 = 1500, f1 = 7000, PI = 3.14159265358979;
    size_t n = (size_t)(dur * rate); std::vector<float> c(n);
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / rate, w = 0.5 - 0.5 * cos(2 * PI * i / (n - 1));
        c[i] = (float)(amp * w * sin(2 * PI * (f0 * t + (f1 - f0) * t * t / (2 * dur))));
    }
    return c;
}
struct Hit { double idx = -1, snr = 0; };
static Hit FindChirp(const std::vector<float>& x, const std::vector<float>& c) {
    Hit h; if (x.size() < c.size() + 16) return h;
    size_t N = x.size() - c.size(), m = c.size();
    std::vector<float> r(N);
    for (size_t i = 0; i < N; i++) { double s = 0; const float* xp = &x[i]; for (size_t j = 0; j < m; j++) s += c[j] * xp[j]; r[i] = (float)fabs(s); }
    size_t best = std::max_element(r.begin(), r.end()) - r.begin();
    double e = 0; size_t k = 0;
    for (size_t i = 0; i < N; i++) if (i + m < best || i > best + m) { e += (double)r[i] * r[i]; k++; }
    double rms = k ? sqrt(e / k) : 1e-9;
    h.idx = (double)best; h.snr = r[best] / (rms + 1e-12);
    return h;
}
static double TimeAt(const Rec& r, double idx) {   // seconds on the QPC clock
    if (r.stamps.empty()) return 0;
    size_t k = 0;
    for (size_t i = 0; i < r.stamps.size(); i++) if ((double)r.stamps[i].first <= idx && r.stamps[i].second) k = i;
    return r.stamps[k].second * 1e-7 + (idx - (double)r.stamps[k].first) / r.rate;
}
static void MicPump(IAudioCaptureClient* cc, const WAVEFORMATEX* f, Rec* r) {
    UINT32 pk = 0;
    while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk) {
        BYTE* d; UINT32 n; DWORD fl; UINT64 qpc = 0;
        if (FAILED(cc->GetBuffer(&d, &n, &fl, nullptr, &qpc))) return;
        if (r) RecAppend(*r, (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : d, n, f, qpc);
        cc->ReleaseBuffer(n);
    }
}
static bool Cancelled(std::atomic<bool>* c) { return !g_run || (c && c->load()); }

// One timed chirp. Returns latency in ms (loopback clock -> mic arrival) or NAN.
static double Trial(bool viaMirror, IAudioCaptureClient* cc, const WAVEFORMATEX* mf,
                    const std::vector<float>& chirpOut, const std::vector<float>& chirpMic, double& snr,
                    std::atomic<bool>* cancel) {
    Rec mic; mic.rate = mf->nSamplesPerSec;
    MicPump(cc, mf, nullptr);
    { std::lock_guard<std::mutex> lk(g_calMx); g_loopRec = Rec(); g_loopRec.rate = g_fmt->nSamplesPerSec; }
    if (!viaMirror) g_recLoop = true;
    ULONGLONG t0 = GetTickCount64(); bool fired = false;
    while (GetTickCount64() - t0 < 1500) {
        if (Cancelled(cancel)) break;
        MicPump(cc, mf, &mic);
        if (!fired && GetTickCount64() - t0 >= 250) {
            std::lock_guard<std::mutex> lk(g_calMx);
            if (viaMirror) { g_injectChirp = chirpOut; g_injectPos = 0; g_injectQpc = 0; }
            else           { g_keeperChirp = chirpOut; g_keeperPos = 0; }
            fired = true;
        }
        Sleep(5);
    }
    g_recLoop = false;
    double tRef = 0;
    if (viaMirror) {
        std::lock_guard<std::mutex> lk(g_calMx);
        tRef = g_injectQpc * 1e-7; g_injectChirp.clear(); g_injectPos = 0;
        if (!g_injectQpc) return NAN;
    } else {
        Rec lr;
        { std::lock_guard<std::mutex> lk(g_calMx); lr = g_loopRec; g_keeperChirp.clear(); g_keeperPos = 0; }
        Hit hl = FindChirp(lr.s, chirpOut);
        if (hl.snr < 8) return NAN;
        tRef = TimeAt(lr, hl.idx);
    }
    if (Cancelled(cancel)) return NAN;
    Hit hm = FindChirp(mic.s, chirpMic);
    snr = hm.snr;
    if (hm.snr < 7) return NAN;
    return (TimeAt(mic, hm.idx) - tRef) * 1000.0;
}
static double Median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

struct CalResult {
    bool ok = false;           // both headphones heard clearly
    double mainMs = 0, mirrorMs = 0;
    int delay = 0;             // delay now applied to the mirror
    bool mirrorSlower = false; // mirror lags the main headphone even at 0 delay -> swap roles
    int slowerBy = 0;
    double residual = NAN;     // verified remaining offset (ms)
    std::wstring mic;
};
using LogFn = std::function<void(const std::wstring&)>;

static CalResult Calibrate(Target* t, float amp, int rounds, bool verify, LogFn log, std::atomic<bool>* cancel) {
    CalResult res;
    wchar_t buf[512];
    if (!g_isFloat) { if (log) log(L"Calibration needs a float mix format."); return res; }
    // the laptop's built-in mic; never a Bluetooth headset mic (that forces call-quality mode)
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDeviceCollection> col; ComPtr<IMMDevice> micDev;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return res;
    UINT n = 0; col->GetCount(&n);
    int bestScore = -1;
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d; if (FAILED(col->Item(i, &d))) continue;
        std::wstring nm = DevName(d.Get());
        if (nm.find(L"Headset") != std::wstring::npos || nm.find(L"Hands-Free") != std::wstring::npos ||
            nm.find(L"VB-Audio") != std::wstring::npos || nm.find(L"AirPods") != std::wstring::npos) continue;
        int score = nm.find(L"Microphone") != std::wstring::npos ? 2 : 1;
        if (score > bestScore) { bestScore = score; micDev = d; res.mic = nm; }
    }
    if (!micDev) { if (log) log(L"No built-in microphone found."); return res; }
    ComPtr<IAudioClient> mac; ComPtr<IAudioCaptureClient> mcc; WAVEFORMATEX* mf = nullptr;
    if (FAILED(micDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)mac.GetAddressOf())) ||
        FAILED(mac->GetMixFormat(&mf)) ||
        FAILED(mac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mf, nullptr)) ||
        FAILED(mac->GetService(IID_PPV_ARGS(&mcc))) || FAILED(mac->Start())) {
        if (log) log(L"Could not open the microphone.");
        if (mf) CoTaskMemFree(mf);
        return res;
    }
    if (log) { swprintf(buf, 512, L"calibrating %s (main) vs %s (mirror), mic: %s", g_engDefName.c_str(), t->name.c_str(), res.mic.c_str()); log(buf); }
    auto chirpOut = MakeChirp(g_fmt->nSamplesPerSec, amp), chirpMic = MakeChirp(mf->nSamplesPerSec, amp);

    g_calTarget = t; g_calibrating = true; g_muteMirrors = true;
    for (int i = 0, w = (t->delayMs + 500) / 10; i < w && !Cancelled(cancel); i++) Sleep(10);   // drain queued audio

    std::vector<double> A, B;
    for (int k = 0; k < rounds && !Cancelled(cancel); k++) {
        double sa = 0, sb = 0;
        double a = Trial(false, mcc.Get(), mf, chirpOut, chirpMic, sa, cancel);
        double b = Trial(true, mcc.Get(), mf, chirpOut, chirpMic, sb, cancel);
        if (log) { swprintf(buf, 512, L"round %d: main %.0f ms (snr %.0f)  mirror %.0f ms (snr %.0f)", k + 1, a, sa, b, sb); log(buf); }
        if (!std::isnan(a)) A.push_back(a);
        if (!std::isnan(b)) B.push_back(b);
    }
    size_t need = rounds >= 3 ? 2 : 1;
    res.ok = !Cancelled(cancel) && A.size() >= need && B.size() >= need;
    if (res.ok) {
        res.mainMs = Median(A); res.mirrorMs = Median(B);
        int cur = t->delayMs.load();
        int want = (int)lround(cur + (res.mainMs - res.mirrorMs));
        if (want < 0) { res.slowerBy = -want; res.mirrorSlower = -want > 5; want = 0; }
        res.delay = std::clamp(want, 0, 1500);
        t->delayMs = res.delay;
        if (log) { swprintf(buf, 512, L"main %.0f ms, mirror %.0f ms -> mirror delay %d ms%s", res.mainMs, res.mirrorMs, res.delay,
                            res.mirrorSlower ? L" (mirror is SLOWER: swap roles)" : L""); log(buf); }
        if (verify && !res.mirrorSlower) {
            for (int i = 0; i < 120 && !Cancelled(cancel); i++) Sleep(10);
            double sa = 0, sb = 0;
            double a = Trial(false, mcc.Get(), mf, chirpOut, chirpMic, sa, cancel);
            double b = Trial(true, mcc.Get(), mf, chirpOut, chirpMic, sb, cancel);
            if (!std::isnan(a) && !std::isnan(b)) res.residual = b - a;
            if (log) { swprintf(buf, 512, L"verify: remaining offset %+.0f ms", res.residual); log(buf); }
        }
    } else if (log) log(L"could not hear both headphones clearly");
    g_muteMirrors = false; g_calibrating = false; g_calTarget = nullptr;
    mac->Stop(); CoTaskMemFree(mf);
    return res;
}
