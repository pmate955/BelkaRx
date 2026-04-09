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

SpectrumSpan computeSpectrumSpan(int fftSize, int sampleRate, bool zoomEnabled, bool hasCustomZoomCenter, double customZoomCenterHz) {
    int baseBins = fftSize / 4;
    int minLogicalBin = -(baseBins / 2);
    int maxLogicalBin = minLogicalBin + baseBins - 1;
    if (zoomEnabled) {
        int visibleBins = baseBins / 2;
        // Keep the historically watched IF frequency centered in zoom view.
        // Using double precision avoids integer truncation drift.
        constexpr double kZoomTrackedHz = 6350.0;
        double binHz = static_cast<double>(sampleRate) / static_cast<double>(fftSize);
        double centerHz = hasCustomZoomCenter ? customZoomCenterHz : kZoomTrackedHz;
        int startBin = static_cast<int>(std::lround(centerHz / binHz - (visibleBins * 0.5)));

        // Keep the whole zoom window inside the 48 kHz band.
        int minStart = minLogicalBin;
        int maxStart = maxLogicalBin - visibleBins + 1;
        if (startBin < minStart) startBin = minStart;
        if (startBin > maxStart) startBin = maxStart;
        return {visibleBins, startBin};
    }

    return {baseBins, minLogicalBin};
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
            int bin2 = (bin1 + 1 < span.visibleBins) ? (bin1 + 1) : bin1;
            double frac = binPos - bin1;
            double invFrac = 1.0 - frac;

            int logicalBin1 = span.startBin + bin1;
            int logicalBin2 = span.startBin + bin2;
            int shiftedBin1 = logicalBin1 >= 0 ? logicalBin1 : logicalBin1 + fftSize;
            int shiftedBin2 = logicalBin2 >= 0 ? logicalBin2 : logicalBin2 + fftSize;

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
