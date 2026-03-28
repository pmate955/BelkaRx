#include "DspHelpers.h"

#include <cmath>

void unpackIqSamples(const int16_t* interleavedIq, int fftSize, bool swapIq, std::vector<float>& real, std::vector<float>& imag) {
    for (int i = 0; i < fftSize; i++) {
        if (swapIq) {
            real[i] = interleavedIq[2 * i + 1] / 32768.0f;
            imag[i] = interleavedIq[2 * i] / 32768.0f;
        } else {
            real[i] = interleavedIq[2 * i] / 32768.0f;
            imag[i] = interleavedIq[2 * i + 1] / 32768.0f;
        }
    }
}

SpectrumSpan computeSpectrumSpan(int fftSize, int sampleRate, bool zoomEnabled) {
    int baseBins = fftSize / 4;
    if (zoomEnabled) {
        int visibleBins = baseBins / 2;
        int binHz = sampleRate / fftSize;
        int startBin = (8000 / binHz) - visibleBins / 2;
        return {visibleBins, startBin};
    }

    return {baseBins, -(baseBins / 2)};
}

void computeLineMagnitudes(
    const std::vector<float>& real,
    const std::vector<float>& imag,
    int fftSize,
    int surfaceWidth,
    bool zoomEnabled,
    const SpectrumSpan& span,
    std::vector<double>& outMagnitudes,
    double& minMag,
    double& maxMag) {
    if ((int)outMagnitudes.size() != surfaceWidth) {
        outMagnitudes.resize(surfaceWidth);
    }

    minMag = 1e9;
    maxMag = -1e9;
    double invSurfaceWidth = 1.0 / surfaceWidth;

    for (int x = 0; x < surfaceWidth; x++) {
        double logMag;
        if (zoomEnabled) {
            double binPos = (x * (double)span.visibleBins) * invSurfaceWidth;
            int bin1 = (int)binPos;
            int bin2 = bin1 + 1;
            double frac = binPos - bin1;
            double invFrac = 1.0 - frac;

            int shiftedBin1 = span.startBin + bin1;
            int shiftedBin2 = span.startBin + bin2;

            if (shiftedBin1 < 0) shiftedBin1 += fftSize;
            else if (shiftedBin1 >= fftSize) shiftedBin1 -= fftSize;
            if (shiftedBin2 < 0) shiftedBin2 += fftSize;
            else if (shiftedBin2 >= fftSize) shiftedBin2 -= fftSize;

            double magSq1 = real[shiftedBin1] * real[shiftedBin1] + imag[shiftedBin1] * imag[shiftedBin1];
            double magSq2 = real[shiftedBin2] * real[shiftedBin2] + imag[shiftedBin2] * imag[shiftedBin2];
            double interpMagSq = magSq1 * invFrac + magSq2 * frac;
            logMag = 10.0 * log10(interpMagSq + 1e-12) + 120.0;
        } else {
            int binIdxInSpan = static_cast<int>((x * span.visibleBins) * invSurfaceWidth);
            int binIdx = span.startBin + binIdxInSpan;
            int shiftedBin = (binIdx >= 0) ? binIdx : binIdx + fftSize;
            if (shiftedBin >= fftSize) shiftedBin -= fftSize;

            double magSq = real[shiftedBin] * real[shiftedBin] + imag[shiftedBin] * imag[shiftedBin];
            logMag = 10.0 * log10(magSq + 1e-12) + 120.0;
        }

        outMagnitudes[x] = logMag;
        if (logMag < minMag) minMag = logMag;
        if (logMag > maxMag) maxMag = logMag;
    }
}
