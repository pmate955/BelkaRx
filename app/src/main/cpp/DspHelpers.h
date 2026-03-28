#pragma once

#include <cstdint>
#include <vector>

struct SpectrumSpan {
    int visibleBins;
    int startBin;
};

void unpackIqSamples(const int16_t* interleavedIq, int fftSize, bool swapIq, std::vector<float>& real, std::vector<float>& imag);
SpectrumSpan computeSpectrumSpan(int fftSize, int sampleRate, bool zoomEnabled);
void computeLineMagnitudes(
    const std::vector<float>& real,
    const std::vector<float>& imag,
    int fftSize,
    int surfaceWidth,
    bool zoomEnabled,
    const SpectrumSpan& span,
    std::vector<double>& outMagnitudes,
    double& minMag,
    double& maxMag);
