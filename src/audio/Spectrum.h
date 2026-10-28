#pragma once

#include <vector>

// Turns audio samples into the bar heights of a spectrum analyser: a Hann
// windowed FFT, grouped into logarithmically spaced bands.
class Spectrum
{
public:
    explicit Spectrum(int fftSize = 2048);

    int fftSize() const { return m_size; }
    // 'samples' holds fftSize() mono samples. Returns one level per band,
    // 0 (-69 dB or less) .. 1 (-15 dB band energy). Pink noise at music level (-18 dBFS RMS) gives an
    // even row at about two thirds of the height when split into ~40 bands.
    std::vector<float> analyse(const float *samples, int sampleRate, int bands, float minHz = 40.f,
                               float maxHz = 16000.f);

private:
    void fft(std::vector<float> &re, std::vector<float> &im) const;

    int m_size;
    std::vector<float> m_window;
};
