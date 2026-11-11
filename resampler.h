// Windowed-sinc resampler used for the mirror's drift correction.
//
// The mirror runs off a different Bluetooth clock than the main headphone, so we play it back
// a hair faster or slower (at most 0.2 %). Linear interpolation would do that but audibly dulls
// the treble (about -6 dB at 16 kHz). This 48-tap Kaiser-windowed sinc keeps the passband flat to
// 20 kHz and pushes interpolation artefacts far below audibility. At an exact ratio of 1 with
// zero phase it is bit-exact passthrough.
#pragma once
#include <vector>
#include <cmath>
#include <cstddef>
#include <algorithm>

struct SincResampler {
    enum { T = 48, H = T / 2, P = 1024 };  // taps, half-width, filter phases
    std::vector<float> tab;                // (P + 1) * T coefficients
    std::vector<float> in;                 // interleaved input frames (history + lookahead)
    double pos = 0;                        // next output position, in input frames from in[0]
    unsigned ch = 2;

    void init(unsigned channels) {
        ch = channels;
        tab.assign((size_t)(P + 1) * T, 0.f);
        const double beta = 10.0, PI = 3.14159265358979323846;
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double i0b = bessel0(beta);
        for (int p = 0; p <= P; p++) {
            double f = (double)p / P, sum = 0;
            std::vector<double> h(T);
            for (int k = 0; k < T; k++) {
                double x = k - (H - 1) - f;                       // distance from the output instant
                double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(PI * x) / (PI * x);
                double wv = x / H;
                double win = std::fabs(wv) >= 1 ? 0.0 : bessel0(beta * std::sqrt(1 - wv * wv)) / i0b;
                h[k] = s * win; sum += h[k];
            }
            for (int k = 0; k < T; k++) tab[(size_t)p * T + k] = (float)(h[k] / sum);   // unity gain at DC
        }
        reset();
    }
    void reset() { in.assign((size_t)(H - 1) * ch, 0.f); pos = H - 1; }

    // Produce n output frames at the given speed ratio, pulling input from src.read(bytes, frames).
    // Returns false if the source ran dry (the missing input is treated as silence).
    template <class Src>
    bool process(Src& src, float* out, unsigned n, double ratio) {
        size_t L = in.size() / ch;
        size_t last = (size_t)std::floor(pos + (n - 1) * ratio) + H;
        bool ok = true;
        if (last + 1 > L) {
            size_t need = last + 1 - L;
            in.resize((L + need) * ch);
            size_t got = src.read((unsigned char*)&in[L * ch], need);
            if (got < need) { std::fill(in.begin() + (L + got) * ch, in.end(), 0.f); ok = false; }
        }
        float h[T];
        for (unsigned i = 0; i < n; i++) {
            double p = pos + i * ratio;
            size_t i0 = (size_t)p;
            double fp = (p - (double)i0) * P;
            int pi = std::min((int)fp, P - 1);
            float pf = (float)(fp - pi);
            const float* h0 = &tab[(size_t)pi * T];
            const float* h1 = h0 + T;
            for (int k = 0; k < T; k++) h[k] = h0[k] + (h1[k] - h0[k]) * pf;
            const float* x = &in[(i0 - (H - 1)) * ch];
            for (unsigned c = 0; c < ch; c++) {
                float acc = 0;
                for (int k = 0; k < T; k++) acc += x[(size_t)k * ch + c] * h[k];
                out[(size_t)i * ch + c] = acc;
            }
        }
        pos += n * ratio;
        size_t drop = (size_t)std::floor(pos) - (H - 1);
        in.erase(in.begin(), in.begin() + drop * ch);
        pos -= (double)drop;
        return ok;
    }
};
