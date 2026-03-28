#include "ColorScales.h"

#include <cmath>

namespace {

void applyRainbowScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    double norm = intensityInt / 255.0;
    if (norm < 0.25) {
        double t = norm * 4.0;
        r = 0;
        g = 0;
        b = static_cast<uint8_t>(t * 255);
    } else if (norm < 0.5) {
        double t = (norm - 0.25) * 4.0;
        r = 0;
        g = static_cast<uint8_t>(t * 255);
        b = static_cast<uint8_t>((1.0 - t) * 255 + t * 150);
    } else if (norm < 0.75) {
        double t = (norm - 0.5) * 4.0;
        r = static_cast<uint8_t>(t * 255);
        g = 255;
        b = static_cast<uint8_t>((1.0 - t) * 150);
    } else {
        double t = (norm - 0.75) * 4.0;
        r = 255;
        g = static_cast<uint8_t>((1.0 - t) * 255);
        b = 0;
    }
}

void applyLightBlueScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    double norm = intensityInt / 255.0;
    if (norm < 0.33) {
        r = 0;
        g = 0;
        b = static_cast<uint8_t>(norm * 3.0 * 150);
    } else if (norm < 0.66) {
        double t = (norm - 0.33) * 3.0;
        r = 0;
        g = static_cast<uint8_t>(t * 206);
        b = static_cast<uint8_t>(150 + t * 105);
    } else {
        double t = (norm - 0.66) * 3.0;
        r = static_cast<uint8_t>(t * 255);
        g = static_cast<uint8_t>(206 + t * 49);
        b = 255;
    }
}

void applyGrayscaleScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    auto gray = static_cast<uint8_t>(intensityInt);
    r = gray;
    g = gray;
    b = gray;
}

uint8_t lerpChannel(uint8_t start, uint8_t end, double t) {
    return static_cast<uint8_t>(start + (end - start) * t);
}

void applyGradientScale(
    int intensityInt,
    uint8_t& r,
    uint8_t& g,
    uint8_t& b,
    const double* positions,
    const uint8_t colors[][3],
    int stopCount) {
    double norm = intensityInt / 255.0;
    if (norm <= positions[0]) {
        r = colors[0][0];
        g = colors[0][1];
        b = colors[0][2];
        return;
    }
    if (norm >= positions[stopCount - 1]) {
        r = colors[stopCount - 1][0];
        g = colors[stopCount - 1][1];
        b = colors[stopCount - 1][2];
        return;
    }

    for (int i = 0; i < stopCount - 1; i++) {
        if (norm <= positions[i + 1]) {
            double localT = (norm - positions[i]) / (positions[i + 1] - positions[i]);
            r = lerpChannel(colors[i][0], colors[i + 1][0], localT);
            g = lerpChannel(colors[i][1], colors[i + 1][1], localT);
            b = lerpChannel(colors[i][2], colors[i + 1][2], localT);
            return;
        }
    }
}

void applyGreenPhosphorScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    static const double positions[] = {0.0, 0.35, 0.65, 0.85, 1.0};
    static const uint8_t colors[][3] = {
        {6, 18, 6},
        {18, 78, 18},
        {32, 190, 32},
        {140, 255, 140},
        {228, 255, 196}
    };
    applyGradientScale(intensityInt, r, g, b, positions, colors, 5);
}

void applyCoolHotScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    double norm = intensityInt / 255.0;
    if (norm < 0.25) {
        double t = norm * 4.0;
        r = static_cast<uint8_t>(t * 100);
        g = 0;
        b = static_cast<uint8_t>(t * 150);
    } else if (norm < 0.5) {
        double t = (norm - 0.25) * 4.0;
        r = static_cast<uint8_t>(100 + t * 155);
        g = 0;
        b = static_cast<uint8_t>((1.0 - t) * 150);
    } else if (norm < 0.75) {
        double t = (norm - 0.5) * 4.0;
        r = 255;
        g = static_cast<uint8_t>(t * 200);
        b = 0;
    } else {
        double t = (norm - 0.75) * 4.0;
        r = 255;
        g = static_cast<uint8_t>(200 + t * 55);
        b = static_cast<uint8_t>(t * 255);
    }
}

void applyLcdScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t baseR = 0x79;
    const uint8_t baseG = 0x9A;
    const uint8_t baseB = 0xA4;
    double norm = intensityInt / 255.0;

    if (norm < 0.45) {
        double t = norm / 0.45;
        t = pow(t, 0.75);
        const uint8_t gray = 0x38;
        r = lerpChannel(baseR, gray, t);
        g = lerpChannel(baseG, gray, t);
        b = lerpChannel(baseB, gray, t);
    } else {
        double t = (norm - 0.45) / 0.55;
        t = pow(t, 0.70);
        const uint8_t gray = 0x38;
        r = lerpChannel(gray, 0x00, t);
        g = lerpChannel(gray, 0x00, t);
        b = lerpChannel(gray, 0x00, t);
    }
}

}  // namespace

uint32_t getColorWithParams(double intensity, int sensitivity, int contrast, int scale) {
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    intensity = intensity + (sensitivity - 100) * 0.8;
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    double contrastExponent = 0.5 + (contrast / 300.0) * 4.0;
    intensity = intensity / 255.0;
    intensity = pow(intensity, contrastExponent);
    intensity = intensity * 255.0;

    int intensityInt = static_cast<int>(intensity);
    if (intensityInt < 0) intensityInt = 0;
    if (intensityInt > 255) intensityInt = 255;

    uint8_t r, g, b;
    if (scale == 0) {
        applyRainbowScale(intensityInt, r, g, b);
    } else if (scale == 1) {
        applyLightBlueScale(intensityInt, r, g, b);
    } else if (scale == 2) {
        applyGrayscaleScale(intensityInt, r, g, b);
    } else if (scale == 3) {
        applyCoolHotScale(intensityInt, r, g, b);
    } else if (scale == 4) {
        applyGreenPhosphorScale(intensityInt, r, g, b);
    } else {
        applyLcdScale(intensityInt, r, g, b);
    }

    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

void buildColorLut(int sensitivity, int contrast, int scale, std::array<uint32_t, 256>& lut) {
    for (int i = 0; i < 256; i++) {
        lut[i] = getColorWithParams(static_cast<double>(i), sensitivity, contrast, scale);
    }
}
