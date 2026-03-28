#pragma once

#include <array>
#include <cstdint>

uint32_t getColorWithParams(double intensity, int sensitivity, int contrast, int scale);
void buildColorLut(int sensitivity, int contrast, int scale, std::array<uint32_t, 256>& lut);
