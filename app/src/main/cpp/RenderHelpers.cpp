#include "RenderHelpers.h"

#include <algorithm>
#include <cstring>

#include "ColorScales.h"

namespace {

double liftSpectrumColorNormalized(double normalized) {
    constexpr double kSpectrumColorOffset = 0.15;
    constexpr double kSpectrumColorFloor = 0.2;

    double shifted = kSpectrumColorOffset + normalized * (1.0 - kSpectrumColorOffset);
    if (shifted < kSpectrumColorFloor) {
        shifted = kSpectrumColorFloor;
    }
    return shifted > 1.0 ? 1.0 : shifted;
}

}  // namespace

bool drawSpectrumFrame(
    uint32_t* dest,
    int stride,
    const std::vector<double>& lineMagnitudes,
    std::vector<double>& decayBuffer,
    const SpectrumRenderParams& params) {
    if ((int)lineMagnitudes.size() != params.surfaceWidth) {
        return false;
    }

    int effectiveSensitivity = static_cast<int>(params.sensitivity * 0.75);

    if ((int)decayBuffer.size() != params.surfaceWidth) {
        decayBuffer.assign(params.surfaceWidth, -1000.0);
    }

    const uint32_t backgroundColor = (params.colorScale == 5) ? 0xFFA49A79u : 0xFF000000u;
    if (stride == params.surfaceWidth) {
        std::fill_n(dest, params.surfaceWidth * params.surfaceHeight, backgroundColor);
    } else {
        for (int y = 0; y < params.surfaceHeight; y++) {
            std::fill_n(&dest[y * stride], params.surfaceWidth, backgroundColor);
        }
    }

    double refLevel = 250.0 - effectiveSensitivity;
    double displayRange = 160.0 - (params.contrast / 300.0) * 140.0;
    double minLevel = refLevel - displayRange;
    double invDisplayRange = 1.0 / displayRange;
    double decayRate = 0.75;
    int plotHeight = params.surfaceHeight - params.topMargin;
    int prevY = -1;

    for (int x = 0; x < params.surfaceWidth; x++) {
        double currentMag = lineMagnitudes[x];
        double signalForPlot;
        if (params.fastMode) {
            decayBuffer[x] = currentMag;
            signalForPlot = currentMag;
        } else {
            if (currentMag > decayBuffer[x]) {
                decayBuffer[x] = currentMag;
            } else {
                decayBuffer[x] -= decayRate;
            }
            signalForPlot = decayBuffer[x];
        }

        double normalized = (signalForPlot - minLevel) * invDisplayRange;
        normalized = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);

        double colorNormalized = liftSpectrumColorNormalized(normalized);
        if (params.constantColor) {
            colorNormalized = 0.75;
        }

        uint32_t traceColor = getColorWithParams(
            colorNormalized * 255.0,
            effectiveSensitivity,
            params.contrast,
            params.colorScale);

        int yPos = params.surfaceHeight - 1 - (int)(normalized * plotHeight);
        yPos = yPos < params.topMargin ? params.topMargin : (yPos >= params.surfaceHeight ? params.surfaceHeight - 1 : yPos);

        if (params.filled) {
            int bottomY = params.surfaceHeight - 1;
            for (int y = yPos; y <= bottomY; y++) {
                dest[y * stride + x] = traceColor;
            }
        } else {
            if (prevY != -1) {
                int step = (yPos > prevY) ? 1 : -1;
                int y = prevY;
                while (y != yPos) {
                    dest[y * stride + x] = traceColor;
                    y += step;
                }
                dest[yPos * stride + x] = traceColor;
            } else {
                dest[yPos * stride + x] = traceColor;
            }
            prevY = yPos;
        }
    }

    return true;
}

void blitWaterfallFrame(
    uint32_t* dest,
    int stride,
    const std::vector<uint32_t>& waterfallBuffer,
    int surfaceWidth,
    int surfaceHeight,
    int topMargin,
    int waterfallRows,
    int headRow,
    uint32_t topMarginColor) {
    int visibleTopMargin = topMargin < surfaceHeight ? topMargin : surfaceHeight;
    for (int y = 0; y < visibleTopMargin; y++) {
        std::fill_n(&dest[y * stride], surfaceWidth, topMarginColor);
    }

    if (waterfallRows <= 0 || waterfallBuffer.empty()) {
        return;
    }

    if (stride == surfaceWidth) {
        int firstChunkRows = waterfallRows - headRow;
        std::memcpy(
            &dest[topMargin * surfaceWidth],
            &waterfallBuffer[headRow * surfaceWidth],
            firstChunkRows * surfaceWidth * sizeof(uint32_t));

        if (headRow > 0) {
            std::memcpy(
                &dest[(topMargin + firstChunkRows) * surfaceWidth],
                waterfallBuffer.data(),
                headRow * surfaceWidth * sizeof(uint32_t));
        }
        return;
    }

    for (int y = topMargin; y < surfaceHeight; y++) {
        int dataRow = y - topMargin;
        int physicalRow = headRow + dataRow;
        if (physicalRow >= waterfallRows) {
            physicalRow -= waterfallRows;
        }
        std::memcpy(
            &dest[y * stride],
            &waterfallBuffer[physicalRow * surfaceWidth],
            surfaceWidth * sizeof(uint32_t));
    }
}
