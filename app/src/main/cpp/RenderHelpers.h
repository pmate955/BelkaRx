#pragma once

#include <cstdint>
#include <vector>

struct SpectrumRenderParams {
    int surfaceWidth;
    int surfaceHeight;
    int topMargin;
    int sensitivity;
    int contrast;
    int colorScale;
    bool filled;
    bool constantColor;
    float smoothingAlpha;
    double decayDbPerSecond;
    double frameDeltaSec;
};

bool drawSpectrumFrame(
    uint32_t* dest,
    int stride,
    const std::vector<double>& lineMagnitudes,
    std::vector<double>& smoothedBuffer,
    std::vector<double>& decayBuffer,
    const SpectrumRenderParams& params);
