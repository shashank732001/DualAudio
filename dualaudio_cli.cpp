// dualaudio-cli: headless test/diagnostic tool for the DualAudio engine.
//   dualaudio-cli --list
//   dualaudio-cli --target N [--delay MS] [--calibrate] [--quiet] [--seconds S]
//   dualaudio-cli --selftest      (measures the drift-correction resampler's audio quality)
#include "calibrate.h"
#include <io.h>
#include <fcntl.h>

static BOOL WINAPI OnCtrl(DWORD) { g_run = false; return TRUE; }

// ---------------------------------------------------------------- resampler quality self-test
struct VecSrc {
    const float* d; size_t frames, pos = 0; unsigned ch;
    size_t read(unsigned char* dst, size_t n) {
        size_t k = std::min(n, frames - pos);
        memcpy(dst, d + pos * ch, k * ch * sizeof(float)); pos += k; return k;
    }
};
// Least-squares fit of a sine at freq w (rad/sample); returns gain (vs amp) in dB and SNR in dB.
static void Fit(const std::vector<float>& y, size_t from, double w, double amp, double& gainDb, double& snrDb) {
    double s11 = 0, s22 = 0, s12 = 0, b1 = 0, b2 = 0;
    for (size_t i = from; i < y.size(); i++) { double s = sin(w * i), c = cos(w * i); s11 += s * s; s22 += c * c; s12 += s * c; b1 += y[i] * s; b2 += y[i] * c; }
    double det = s11 * s22 - s12 * s12, a = (b1 * s22 - b2 * s12) / det, b = (b2 * s11 - b1 * s12) / det;
    double sig = 0, res = 0;
    for (size_t i = from; i < y.size(); i++) { double f = a * sin(w * i) + b * cos(w * i); sig += f * f; res += (y[i] - f) * (y[i] - f); }
    gainDb = 20 * log10(sqrt(a * a + b * b) / amp);
    snrDb = 10 * log10(sig / std::max(res, 1e-30));
}
static void SelfTest() {
    const unsigned rate = 48000, ch = 2; const size_t N = rate * 2; const double PI = 3.14159265358979, amp = 0.5;
    const double freqs[] = {1000, 5000, 10000, 15000, 18000, 20000};
    const double ratios[] = {1.0, 1.0002, 1.002, 0.998};
    wprintf(L"\n Drift-correction quality at 48 kHz (gain = level change, SNR = everything that isn't the tone)\n\n");
    wprintf(L"   tone    ratio   | sinc (now): gain    SNR   | linear (before): gain    SNR\n");
    for (double f : freqs) for (double r : ratios) {
        std::vector<float> x(N * ch);
        for (size_t i = 0; i < N; i++) for (unsigned c = 0; c < ch; c++) x[i * ch + c] = (float)(amp * sin(2 * PI * f * i / rate));
        size_t M = (size_t)((N - 400) / r);
        // windowed-sinc (what the app uses now)
        SincResampler rs; rs.init(ch); VecSrc src{x.data(), N, 0, ch};
        std::vector<float> o(M * ch), y(M);
        for (size_t done = 0; done < M; done += 480) { unsigned n = (unsigned)std::min<size_t>(480, M - done); rs.process(src, &o[done * ch], n, r); }
        for (size_t i = 0; i < M; i++) y[i] = o[i * ch];
        // linear interpolation (what the app used before)
        std::vector<float> yl(M);
        for (size_t i = 0; i < M; i++) { double p = i * r; size_t k = (size_t)p; double fr = p - k; yl[i] = (float)(x[k * ch] + (x[(k + 1) * ch] - x[k * ch]) * fr); }
        double w = 2 * PI * f * r / rate, g1, s1, g2, s2;
        Fit(y, 4000, w, amp, g1, s1);
        Fit(yl, 4000, w, amp, g2, s2);
        // sinc output is delayed by H-1 input frames; fit handles any phase, so no alignment needed
        wprintf(L"  %5.0f Hz  %.4f  |       %+6.2f dB  %5.0f dB |            %+6.2f dB  %5.0f dB\n", f, r, g1, s1, g2, s2);
    }
    wprintf(L"\n (CD-quality 16-bit audio has an SNR of about 96 dB)\n");
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    bool listOnly = false, calibrate = false, quiet = false; int seconds = 10, delay = 0, target = 0;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--selftest") { SelfTest(); return 0; }
        else if (a == L"--list") listOnly = true;
        else if (a == L"--calibrate") calibrate = true;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"--target" && i + 1 < argc) target = _wtoi(argv[++i]);
        else if (a == L"--delay" && i + 1 < argc) delay = _wtoi(argv[++i]);
        else if (a == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else { wprintf(L"usage: dualaudio-cli --list | --target N [--delay MS] [--calibrate] [--quiet] [--seconds S]\n"); return 1; }
    }
    std::wstring defId, defName;
    auto devs = EnumOutputs(&defId, &defName);
    for (size_t i = 0; i < devs.size(); i++)
        wprintf(L" [%zu] %s%s\n", i + 1, devs[i].name.c_str(), devs[i].id == defId ? L"   <- default (main)" : L"");
    if (listOnly) return 0;
    if (target < 1 || target > (int)devs.size() || devs[target - 1].id == defId) { wprintf(L"pick a --target that is not the default\n"); return 1; }

    Target* t = StartEngine(devs[target - 1].id, devs[target - 1].name, delay);
    if (!t) { wprintf(L"engine failed to start\n"); return 1; }
    Sleep(1500);
    if (calibrate) {
        CalResult r = Calibrate(t, quiet ? 0.12f : 0.6f, quiet ? 2 : 4, true,
                                [](const std::wstring& s) { wprintf(L" %s\n", s.c_str()); fflush(stdout); }, nullptr);
        wprintf(L" RESULT ok=%d delay=%d mirrorSlower=%d slowerBy=%d residual=%.0f\n", r.ok, r.delay, r.mirrorSlower, r.slowerBy, r.residual);
    }
    for (int s = 0; s < seconds && g_run; s++) {
        Sleep(1000);
        wprintf(L" %-24.24s delay %4d ms  %s  buf %3d ms  trim %+5d ppm  jumps %ld  gaps %ld  level %d/%d\n",
                t->name.c_str(), t->delayMs.load(), t->state == 1 ? L"ok  " : t->state == 2 ? L"LOST" : L"wait",
                t->queueMs.load(), t->ppm.load(), t->jumps.load(), t->gaps.load(), g_srcPeak.exchange(0), t->outPeak.exchange(0));
        fflush(stdout);
    }
    StopEngine();
    CoUninitialize();
    return 0;
}
