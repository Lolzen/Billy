#include "audio/Spectrum.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kRange = 54.f;    // dB shown from the bottom to the top
constexpr float kHeadroom = 15.f; // a full bar is this much below full scale
}

Spectrum::Spectrum(int fftSize) : m_size(fftSize), m_window(size_t(fftSize))
{
    for (int i = 0; i < m_size; ++i)
        m_window[size_t(i)] = 0.5f * (1.f - std::cos(2.f * float(M_PI) * float(i) / float(m_size - 1)));
}

void Spectrum::fft(std::vector<float> &re, std::vector<float> &im) const
{
    const int n = m_size;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[size_t(i)], re[size_t(j)]);
            std::swap(im[size_t(i)], im[size_t(j)]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = -2.f * float(M_PI) / float(len);
        const float wr = std::cos(angle), wi = std::sin(angle);
        for (int i = 0; i < n; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int k = 0; k < len / 2; ++k) {
                const size_t a = size_t(i + k), b = size_t(i + k + len / 2);
                const float tr = re[b] * cr - im[b] * ci;
                const float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

std::vector<float> Spectrum::analyse(const float *samples, int sampleRate, int bands, float minHz, float maxHz)
{
    std::vector<float> re(static_cast<size_t>(m_size)), im(static_cast<size_t>(m_size), 0.f);
    for (int i = 0; i < m_size; ++i)
        re[size_t(i)] = samples[i] * m_window[size_t(i)];
    fft(re, im);

    std::vector<float> levels(size_t(bands), 0.f);
    if (sampleRate <= 0 || bands <= 0)
        return levels;
    maxHz = std::min(maxHz, sampleRate / 2.f);
    const float binHz = float(sampleRate) / float(m_size);
    // A full scale sine gives a peak of size/4 with the Hann window.
    const float reference = float(m_size) / 4.f;
    for (int b = 0; b < bands; ++b) {
        const float f0 = minHz * std::pow(maxHz / minHz, float(b) / float(bands));
        const float f1 = minHz * std::pow(maxHz / minHz, float(b + 1) / float(bands));
        int k0 = int(f0 / binHz), k1 = int(f1 / binHz);
        k0 = std::clamp(k0, 1, m_size / 2 - 1);
        k1 = std::clamp(std::max(k1, k0 + 1), k0 + 1, m_size / 2);
        // Energy of the band, so broad sounds count as much as single tones.
        float power = 0.f;
        for (int k = k0; k < k1; ++k)
            power += re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)];
        float dB = 10.f * std::log10(std::max(power / (reference * reference), 1e-12f));
        // -kHeadroom dB fills the bar, -(kHeadroom + kRange) dB leaves it empty.
        levels[size_t(b)] = std::clamp((dB + kHeadroom + kRange) / kRange, 0.f, 1.f);
    }
    return levels;
}
